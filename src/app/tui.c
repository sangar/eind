#include "tui.h"

#include <stdio.h>
#include <time.h>

#include "mc/concurrency/cancel.h"
#include "mc/concurrency/queue.h"
#include "mc/concurrency/threadpool.h"
#include "mc/platform/platform.h"
#include "mc/platform/terminal.h"
#include "mc/text/fmt.h"
#include "mc/text/utf8.h"
#include "../index/search.h"

/* How long the input must be idle before a search starts, and how long an escape sequence may take to arrive. */
enum { TYPING_PAUSE_MS = 40, ESCAPE_WAIT_MS = 30 };

static const char SCREEN_SETUP[] = "\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[2J";
static const char SCREEN_RESTORE[] = "\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";

/*
 * A Search runs on its own detached thread so the input never blocks.
 * Cancelling only raises the flag; the thread keeps going until it notices,
 * then hands itself back through the done queue and the main loop frees it.
 * The job, its copy of the input and its hits live in its arena, and it
 * holds a reference to the snapshot.
 */
typedef struct {
    Arena *arena;
    Snapshot *s;
    ThreadPool *cpu;
    Queue *done;
    Terminal *terminal;
    String input;
    QueryDefaults defaults;
    Cancel cancel;
    IdList hits;
    Error status;
    Err err;
    int64_t elapsed_ns;
} Search;

static void *run_search(void *argument) {
    Search *job = argument;
    int64_t start = clock_monotonic_ns();
    QueryNode *node;
    job->status = query_parse(job->arena, job->input, job->defaults, &node, &job->err);
    if (job->status == ERR_OK)
        job->status = search_run(job->cpu, job->arena, job->s, node, &job->cancel, &job->hits, &job->err);
    /* A blank query lists the whole index; leaving it in index order keeps that instant even for millions. */
    bool blank = str_trim(job->input).len == 0;
    if (job->status == ERR_OK && !blank && !cancel_requested(&job->cancel))
        search_top(job->arena, job->s, job->hits.items, job->hits.count, SORT_NAME, false, -1);
    job->elapsed_ns = clock_monotonic_ns() - start;
    /* The main loop owns the job once it is queued, so nothing may touch it afterwards. */
    Terminal *terminal = job->terminal;
    (void)queue_push(job->done, job);
    terminal_wake(terminal);
    return nullptr;
}

static void free_search(Search *job) {
    if (!job) return;
    snapshot_release(job->s);
    cancel_destroy(&job->cancel);
    arena_destroy(job->arena);
}

typedef struct {
    Arena *arena; /* the view and its input */
    Arena *frame; /* one drawn frame, reset for the next */
    Snapshot *s;
    QueryDefaults defaults;
    ThreadPool *cpu;
    Terminal *terminal;
    Queue *done;
    TerminalSize size;

    StringBuilder input;
    Search *shown; /* the search whose hits are listed, or nullptr */
    size_t selected, top;
    Err error; /* shown instead of the status while its message is set */

    Search *search;  /* the search whose result is still wanted */
    int outstanding; /* searches that have not reported yet, wanted or not */
    bool searching, accept_when_done;
    int64_t settle_at_ns; /* when to start the next search, or 0 */
} View;

static IdList shown_hits(const View *v) { return v->shown ? v->shown->hits : (IdList){0}; }

static void cancel_search(View *v) {
    if (!v->search) return;
    cancel_request(&v->search->cancel);
    v->search = nullptr;
}

static void start_search(View *v) {
    cancel_search(v);
    Arena *arena = arena_create(64 * 1024);
    Search *job = arena_push(arena, sizeof *job);
    *job = (Search){.arena = arena,
                    .s = v->s,
                    .cpu = v->cpu,
                    .done = v->done,
                    .terminal = v->terminal,
                    .input = str_copy(arena, (String){v->input.data, v->input.len}),
                    .defaults = v->defaults};
    cancel_init(&job->cancel);
    snapshot_retain(v->s);
    Thread thread;
    Error e = thread_start(&thread, run_search, job, &v->error);
    if (e != ERR_OK) {
        free_search(job);
        (void)err_wrap(&v->error, e, "cannot start search");
        v->searching = false;
        return;
    }
    thread_detach(&thread);
    v->search = job;
    v->outstanding++;
    v->searching = true;
}

static void apply_search(View *v, Search *job) {
    v->search = nullptr;
    v->searching = false;
    v->selected = v->top = 0;
    if (job->status == ERR_OK) {
        free_search(v->shown);
        v->shown = job;
        v->error.msg[0] = '\0';
    } else {
        v->error = job->err;
        free_search(job);
    }
}

/* collect_searches takes every finished search off the done queue; only the current one changes the view. */
static bool collect_searches(View *v) {
    bool applied = false;
    void *item;
    while (queue_pop_timeout(v->done, 0, &item)) {
        Search *job = item;
        v->outstanding--;
        if (job == v->search) {
            apply_search(v, job);
            applied = true;
        } else {
            free_search(job);
        }
    }
    return applied;
}

static String selected_path(const View *v, Arena *arena) {
    IdList hits = shown_hits(v);
    if (!hits.count) return S("");
    StringBuilder path = str_builder_create(arena, 256);
    return snap_path(v->s, hits.items[v->selected], &path);
}

static void move(View *v, int64_t delta) {
    IdList hits = shown_hits(v);
    if (!hits.count) return;
    int64_t next = (int64_t)v->selected + delta;
    if (next < 0) next = 0;
    if (next >= (int64_t)hits.count) next = (int64_t)hits.count - 1;
    v->selected = (size_t)next;
}

static void open_selected(View *v) {
    ArenaMark mark = arena_mark(v->frame);
    String path = selected_path(v, v->frame);
    /* A failure shows in the status line through v->error. */
    if (path.len) (void)process_open_default(path, &v->error);
    arena_release(mark);
}

/* ---- drawing ---- */

/* put appends at most width code points of s and returns how many it wrote. */
static int put(StringBuilder *frame, String s, int width) {
    int cols = 0;
    size_t p = 0;
    while (p < s.len && cols < width) {
        size_t start = p++;
        while (p < s.len && ((unsigned char)s.data[p] & 0xC0) == 0x80) p++;
        str_builder_append(frame, str_slice(s, start, p));
        cols++;
    }
    return cols;
}

static void pad(StringBuilder *frame, int n) {
    for (int i = 0; i < n; i++) str_builder_append_char(frame, ' ');
}

static int min_int(int a, int b) { return a < b ? a : b; }
static int max_int(int a, int b) { return a > b ? a : b; }

/* skip_columns drops the first n code points of s. */
static String skip_columns(String s, int n) {
    size_t p = 0;
    for (; n > 0 && p < s.len; n--) {
        p++;
        while (p < s.len && ((unsigned char)s.data[p] & 0xC0) == 0x80) p++;
    }
    return str_slice(s, p, s.len);
}

static void draw_row(View *v, StringBuilder *frame, uint32_t id, bool selected) {
    int width = v->size.columns;
    bool is_dir = snap_is_dir(v->s, id);
    char when[32];
    time_t mtime = (time_t)snap_mtime(v->s, id);
    struct tm tm;
    localtime_r(&mtime, &tm);
    strftime(when, sizeof when, "%Y-%m-%d %H:%M", &tm);
    String size = is_dir ? S("") : fmt_bytes(v->frame, snap_size(v->s, id));
    String meta = str_format(v->frame, "%9.*s  %s", (int)size.len, size.data, when);
    int meta_width = (int)meta.len + 1;
    String base = selected ? S("\x1b[0;7m") : S("\x1b[0m");
    str_builder_append(frame, base);
    if (is_dir) str_builder_append(frame, S("\x1b[1;34m"));
    int x = put(frame, S(" "), width);
    x += put(frame, snap_name_view(v->s, id), width - x);
    str_builder_append(frame, base);
    int available = width - x - meta_width - 2;
    uint32_t parent = snap_parent(v->s, id);
    if (parent != NO_PARENT && available > 4) {
        StringBuilder dir_path = str_builder_create(v->frame, 256);
        String dir = snap_path(v->s, parent, &dir_path);
        int len = (int)utf8_count(dir);
        str_builder_append(frame, S("\x1b[2m"));
        x += put(frame, S("  "), 2);
        if (len > available) {
            x += put(frame, S("…"), 1);
            x += put(frame, skip_columns(dir, len - available + 1), available - 1);
        } else {
            x += put(frame, dir, available);
        }
        str_builder_append(frame, base);
    }
    if (width - meta_width > x) pad(frame, width - meta_width - x);
    if (width >= meta_width) {
        str_builder_append(frame, S("\x1b[2m"));
        put(frame, meta, meta_width);
    }
    str_builder_append(frame, S("\x1b[0m"));
}

static void draw(View *v) {
    arena_reset(v->frame);
    int width = v->size.columns;
    String input = {v->input.data, v->input.len};
    StringBuilder frame = str_builder_create(v->frame, 64 * 1024);
    str_builder_append(&frame, S("\x1b[?25l\x1b[H\x1b[0;1m> "));
    put(&frame, input, width - 2);
    str_builder_append(&frame, S("\x1b[0m\x1b[K\r\n\x1b[K"));

    IdList hits = shown_hits(v);
    int list_height = v->size.rows - 3;
    if (list_height >= 1) {
        if (v->selected < v->top) v->top = v->selected;
        if (v->selected >= v->top + (size_t)list_height) v->top = v->selected - (size_t)list_height + 1;
        for (int row = 0; row < list_height; row++) {
            str_builder_append(&frame, S("\r\n\x1b[K"));
            size_t i = v->top + (size_t)row;
            if (i < hits.count) draw_row(v, &frame, hits.items[i], i == v->selected);
        }
        String shown = fmt_thousands(v->frame, (int64_t)hits.count);
        String total = fmt_thousands(v->frame, snap_live_count(v->s));
        String status;
        if (v->error.msg[0]) {
            status = str_format(v->frame, " %s", v->error.msg);
        } else if (v->searching) {
            status = str_format(v->frame, " %.*s of %.*s objects  searching...", (int)shown.len, shown.data,
                                (int)total.len, total.data);
        } else {
            double elapsed_ms = v->shown ? (double)v->shown->elapsed_ns / (double)NS_PER_MILLISECOND : 0;
            status = str_format(v->frame, " %.*s of %.*s objects  %.0fms", (int)shown.len, shown.data, (int)total.len,
                                total.data, elapsed_ms);
        }
        String help = S("Enter print path  Ctrl-O open  Esc quit ");
        int help_width = (int)help.len;
        str_builder_append(&frame, S("\r\n\x1b[0;7m"));
        int x = put(&frame, status, max_int(width - help_width, 0));
        pad(&frame, width - help_width - x);
        put(&frame, help, width - max_int(x, width - help_width));
        str_builder_append(&frame, S("\x1b[0m"));
    }
    int cursor = 3 + (int)utf8_count(input);
    str_builder_append_format(&frame, "\x1b[1;%dH\x1b[?25h", min_int(cursor, width));
    /* Nothing useful is left to do when the terminal is gone. */
    (void)terminal_write(v->terminal, (String){frame.data, frame.len}, nullptr);
}

/* ---- input ---- */

typedef enum { ACT_NONE, ACT_QUIT, ACT_ACCEPT, ACT_INPUT_CHANGED } Action;

static int read_byte(View *v, int timeout_ms) {
    unsigned char c;
    return terminal_read_byte(v->terminal, timeout_ms, &c) ? c : -1;
}

static void delete_last_char(StringBuilder *input) {
    while (input->len > 0) {
        unsigned char c = (unsigned char)input->data[--input->len];
        if ((c & 0xC0) != 0x80) break;
    }
}

static void delete_word(StringBuilder *input) {
    while (input->len > 0 && input->data[input->len - 1] == ' ') input->len--;
    while (input->len > 0 && input->data[input->len - 1] != ' ') input->len--;
}

static void mouse(View *v, int button, int y, bool press) {
    if (!press) return;
    if (button == 64) {
        move(v, -3);
    } else if (button == 65) {
        move(v, 3);
    } else if (button == 0 && y >= 3) {
        size_t row = v->top + (size_t)(y - 3);
        if (row < shown_hits(v).count) v->selected = row;
    }
}

static bool is_sequence(const char *seq, const char *name) { return str_equal(S(seq), S(name)); }

static Action escape_sequence(View *v) {
    int c = read_byte(v, ESCAPE_WAIT_MS);
    if (c < 0) return ACT_QUIT; /* a lone Esc */
    if (c != '[' && c != 'O') return ACT_NONE;
    char seq[32];
    size_t n = 0;
    int b;
    while ((b = read_byte(v, ESCAPE_WAIT_MS)) >= 0 && n < sizeof seq - 1) {
        seq[n++] = (char)b;
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') break;
    }
    seq[n] = '\0';
    int64_t page = max_int(v->size.rows - 3, 1);
    int64_t all = (int64_t)shown_hits(v).count;
    if (seq[0] == '<') {
        int button, x, y;
        char kind;
        if (sscanf(seq + 1, "%d;%d;%d%c", &button, &x, &y, &kind) == 4) mouse(v, button, y, kind == 'M');
    } else if (is_sequence(seq, "A")) {
        move(v, -1);
    } else if (is_sequence(seq, "B")) {
        move(v, 1);
    } else if (is_sequence(seq, "5~")) {
        move(v, -page);
    } else if (is_sequence(seq, "6~")) {
        move(v, page);
    } else if (is_sequence(seq, "H") || is_sequence(seq, "1~") || is_sequence(seq, "7~")) {
        move(v, -all);
    } else if (is_sequence(seq, "F") || is_sequence(seq, "4~") || is_sequence(seq, "8~")) {
        move(v, all);
    }
    return ACT_NONE;
}

static Action key(View *v, int c) {
    switch (c) {
    case 0x1b: return escape_sequence(v);
    case 0x03: return ACT_QUIT;
    case '\r':
    case '\n': return ACT_ACCEPT;
    case 0x0f: /* Ctrl-O */
        open_selected(v);
        return ACT_NONE;
    case 0x7f:
    case 0x08:
        if (!v->input.len) return ACT_NONE;
        delete_last_char(&v->input);
        return ACT_INPUT_CHANGED;
    case 0x15: /* Ctrl-U */
        if (!v->input.len) return ACT_NONE;
        v->input.len = 0;
        return ACT_INPUT_CHANGED;
    case 0x17: /* Ctrl-W */
        if (!v->input.len) return ACT_NONE;
        delete_word(&v->input);
        return ACT_INPUT_CHANGED;
    case 0x0e: move(v, 1); return ACT_NONE;  /* Ctrl-N */
    case 0x10: move(v, -1); return ACT_NONE; /* Ctrl-P */
    }
    if (c >= 0x20) {
        str_builder_append_char(&v->input, (char)c);
        return ACT_INPUT_CHANGED;
    }
    return ACT_NONE;
}

/* ---- the loop ---- */

/* run_view reads keys and search results until the user accepts or quits; it reports whether they accepted. */
static bool run_view(View *v) {
    start_search(v);
    for (;;) {
        draw(v);
        int timeout = -1;
        if (v->settle_at_ns) {
            int64_t wait = (v->settle_at_ns - clock_monotonic_ns()) / NS_PER_MILLISECOND;
            timeout = wait < 0 ? 0 : (int)wait;
        }
        TerminalReady ready = terminal_wait(v->terminal, timeout);
        if (ready.resized) v->size = terminal_size(v->terminal);
        if (v->settle_at_ns && clock_monotonic_ns() >= v->settle_at_ns) {
            v->settle_at_ns = 0;
            start_search(v);
        }
        if (collect_searches(v) && v->accept_when_done) return true;
        if (!ready.input) continue;
        int c = read_byte(v, 0);
        if (c < 0) return false; /* the terminal went away */
        switch (key(v, c)) {
        case ACT_QUIT: return false;
        case ACT_ACCEPT:
            if (!v->searching) return true;
            v->accept_when_done = true;
            if (!v->search) {
                v->settle_at_ns = 0;
                start_search(v);
            }
            break;
        case ACT_INPUT_CHANGED:
            cancel_search(v);
            v->searching = true;
            v->settle_at_ns = clock_monotonic_ns() + TYPING_PAUSE_MS * NS_PER_MILLISECOND;
            break;
        case ACT_NONE: break;
        }
    }
}

Error tui_run(Arena *arena, Snapshot *s, QueryDefaults defaults, String *chosen, Err *err) {
    *chosen = S("");
    Terminal *terminal;
    Error e = terminal_open(&terminal, err);
    if (e != ERR_OK) return err_wrap(err, e, "cannot open the terminal");
    ThreadPool *cpu;
    e = threadpool_create(0, &cpu, err);
    if (e == ERR_OK) e = terminal_write(terminal, S(SCREEN_SETUP), err);
    if (e != ERR_OK) {
        terminal_close(terminal);
        return e;
    }
    View v = {.arena = arena_create(0),
              .frame = arena_create(0),
              .s = s,
              .defaults = defaults,
              .cpu = cpu,
              .terminal = terminal,
              .done = queue_create(),
              .size = terminal_size(terminal)};
    v.input = str_builder_create(v.arena, 256);
    if (run_view(&v)) *chosen = selected_path(&v, arena);
    cancel_search(&v);
    (void)terminal_write(terminal, S(SCREEN_RESTORE), nullptr);
    /* The searches still running report to the queue and wake the terminal, so both outlive them. */
    while (v.outstanding > 0) {
        free_search(queue_pop(v.done));
        v.outstanding--;
    }
    free_search(v.shown);
    terminal_close(terminal);
    threadpool_destroy(cpu);
    queue_destroy(v.done);
    arena_destroy(v.frame);
    arena_destroy(v.arena);
    return ERR_OK;
}
