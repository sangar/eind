#include "tui.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>

#include "../core/arena.h"
#include "../core/threadpool.h"
#include "../index/search.h"

/* How long the input must be idle before a search starts. */
#define TYPING_PAUSE_MS 40
#define ESCAPE_WAIT_MS 30

extern char **environ;

static volatile sig_atomic_t resized; // a signal handler can only set a flag; modern-c: allow global-mutable

static void on_winch(int sig) {
    (void)sig;
    resized = 1;
}

/*
 * A Search runs on its own detached thread so the input never blocks. Cancelling
 * only raises the flag; the thread keeps going until it notices, then reports
 * itself through the wake pipe and the main loop frees it. The job holds a
 * reference to the snapshot so a search still running when the view closes
 * stays safe.
 */
typedef struct {
    Snapshot *s;
    ThreadPool *cpu;
    char *input;
    QueryDefaults defaults;
    atomic_int cancel;
    int wake_fd;
    U32Vec hits;
    SearchStatus status;
    Err err;
    double elapsed_ms;
} Search;

static void *run_search(void *arg) {
    Search *job = arg;
    int64_t start = monotonic_us();
    Arena arena;
    arena_init(&arena, 4096);
    QueryNode *node = query_parse(&arena, job->input, job->defaults, &job->err);
    job->status = node ? search_run(job->cpu, job->s, node, &job->cancel, &job->hits, &job->err) : SEARCH_ERROR;
    bool blank = strspn(job->input, " \t") == strlen(job->input);
    /* A blank query lists the whole index; leaving it in index order keeps that instant even for millions. */
    if (job->status == SEARCH_OK && !blank && !atomic_load(&job->cancel))
        search_top(job->s, job->hits.data, job->hits.len, SORT_NAME, false, -1);
    arena_free(&arena);
    job->elapsed_ms = elapsed_ms_since(start);
    /* The main loop owns the job from here on, so nothing may touch it after this write. */
    if (write(job->wake_fd, &job, sizeof job) < 0) {
        /* the view has closed and the process is exiting */
    }
    return NULL;
}

static void free_search(Search *job) {
    snapshot_release(job->s);
    u32vec_free(&job->hits);
    free(job->input);
    free(job);
}

typedef struct {
    Snapshot *s;
    QueryDefaults defaults;
    ThreadPool *cpu;
    int tty;
    int wake[2];
    int width, height;

    StrBuf input;
    U32Vec hits;
    size_t selected, top;
    double elapsed_ms;
    char error[512];

    Search *search;  /* the search whose result is still wanted */
    int outstanding; /* searches that have not reported yet, wanted or not */
    bool searching, accept_when_done;
    int64_t settle_at; /* when to start the next search, or 0 */
} View;

static void query_size(View *v) {
    struct winsize ws;
    if (ioctl(v->tty, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        v->width = ws.ws_col;
        v->height = ws.ws_row;
    } else {
        v->width = 80;
        v->height = 24;
    }
}

static void cancel_search(View *v) {
    if (!v->search) return;
    atomic_store(&v->search->cancel, 1);
    v->search = NULL;
}

static void start_search(View *v) {
    cancel_search(v);
    Search *job = xcalloc(1, sizeof *job);
    job->s = v->s;
    job->cpu = v->cpu;
    job->input = xstrdup(sb_cstr(&v->input));
    job->defaults = v->defaults;
    job->wake_fd = v->wake[1];
    atomic_init(&job->cancel, 0);
    snapshot_retain(v->s);
    pthread_attr_t detached;
    pthread_attr_init(&detached);
    pthread_attr_setdetachstate(&detached, PTHREAD_CREATE_DETACHED);
    pthread_t thread;
    int rc = pthread_create(&thread, &detached, run_search, job);
    pthread_attr_destroy(&detached);
    if (rc != 0) {
        free_search(job);
        snprintf(v->error, sizeof v->error, "cannot start search: %s", strerror(rc));
        v->searching = false;
        return;
    }
    v->search = job;
    v->outstanding++;
    v->searching = true;
}

static void apply_search(View *v, Search *job) {
    v->search = NULL;
    v->searching = false;
    v->elapsed_ms = job->elapsed_ms;
    v->selected = v->top = 0;
    u32vec_free(&v->hits);
    if (job->status == SEARCH_OK) {
        v->hits = job->hits;
        job->hits = (U32Vec){0};
        v->error[0] = '\0';
    } else {
        snprintf(v->error, sizeof v->error, "%s", job->err.msg);
    }
    free_search(job);
}

/* collect_searches takes every finished search off the wake pipe; only the current one changes the view. */
static bool collect_searches(View *v) {
    bool applied = false;
    Search *job;
    while (read(v->wake[0], &job, sizeof job) == sizeof job) {
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

static void selected_path(const View *v, StrBuf *out) {
    sb_clear(out);
    if (v->hits.len) snap_path(v->s, v->hits.data[v->selected], out);
}

static void move(View *v, long delta) {
    if (!v->hits.len) return;
    long next = (long)v->selected + delta;
    if (next < 0) next = 0;
    if (next >= (long)v->hits.len) next = (long)v->hits.len - 1;
    v->selected = (size_t)next;
}

static void open_with_system(const char *path) {
#ifdef __APPLE__
    char *argv[] = {"open", (char *)path, NULL};
#else
    char *argv[] = {"xdg-open", (char *)path, NULL};
#endif
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_addopen(&actions, STDIN_FILENO, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", O_WRONLY, 0);
    pid_t pid;
    posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
}

/* ---- drawing ---- */

/* put appends at most width code points of s and returns how many it wrote. */
static int put(StrBuf *frame, const char *s, int width) {
    int cols = 0;
    const char *p = s;
    while (*p && cols < width) {
        const char *start = p++;
        while (((unsigned char)*p & 0xC0) == 0x80) p++;
        sb_append(frame, start, (size_t)(p - start));
        cols++;
    }
    return cols;
}

static void pad(StrBuf *frame, int n) {
    for (int i = 0; i < n; i++) sb_putc(frame, ' ');
}

static void draw_row(View *v, StrBuf *frame, uint32_t id, bool selected) {
    bool is_dir = snap_is_dir(v->s, id);
    char size[32], when[32], meta[64];
    format_short_time(snap_mtime(v->s, id), when);
    snprintf(meta, sizeof meta, "%9s  %s", is_dir ? "" : human_size(snap_size(v->s, id), size), when);
    int meta_width = (int)strlen(meta) + 1;
    const char *base = selected ? "\x1b[0;7m" : "\x1b[0m";
    sb_puts(frame, base);
    if (is_dir) sb_puts(frame, "\x1b[1;34m");
    int x = put(frame, " ", v->width);
    x += put(frame, snap_name(v->s, id), v->width - x);
    sb_puts(frame, base);
    int available = v->width - x - meta_width - 2;
    uint32_t parent = snap_parent(v->s, id);
    if (parent != NO_PARENT && available > 4) {
        StrBuf dir = {0};
        snap_path(v->s, parent, &dir);
        int len = utf8_count(dir.data, dir.len);
        sb_puts(frame, "\x1b[2m");
        x += put(frame, "  ", 2);
        const char *shown = dir.data;
        if (len > available) {
            x += put(frame, "…", 1);
            for (int skip = len - available + 1; skip > 0; skip--) {
                shown++;
                while (((unsigned char)*shown & 0xC0) == 0x80) shown++;
            }
            x += put(frame, shown, available - 1);
        } else {
            x += put(frame, shown, available);
        }
        sb_free(&dir);
        sb_puts(frame, base);
    }
    if (v->width - meta_width > x) pad(frame, v->width - meta_width - x);
    if (v->width >= meta_width) {
        sb_puts(frame, "\x1b[2m");
        put(frame, meta, meta_width);
    }
    sb_puts(frame, "\x1b[0m");
}

static void draw(View *v) {
    StrBuf frame = {0};
    sb_puts(&frame, "\x1b[?25l\x1b[H\x1b[0;1m> ");
    put(&frame, sb_cstr(&v->input), v->width - 2);
    sb_puts(&frame, "\x1b[0m\x1b[K\r\n\x1b[K");

    int list_height = v->height - 3;
    if (list_height >= 1) {
        if (v->selected < v->top) v->top = v->selected;
        if (v->selected >= v->top + (size_t)list_height) v->top = v->selected - (size_t)list_height + 1;
        for (int row = 0; row < list_height; row++) {
            sb_puts(&frame, "\r\n\x1b[K");
            size_t i = v->top + (size_t)row;
            if (i < v->hits.len) draw_row(v, &frame, v->hits.data[i], i == v->selected);
        }
        char shown[32], total[32], status[600];
        commas((int64_t)v->hits.len, shown);
        commas(snap_live_count(v->s), total);
        if (v->error[0]) {
            snprintf(status, sizeof status, " %s", v->error);
        } else if (v->searching) {
            snprintf(status, sizeof status, " %s of %s objects  searching...", shown, total);
        } else {
            snprintf(status, sizeof status, " %s of %s objects  %.0fms", shown, total, v->elapsed_ms);
        }
        const char *help = "Enter print path  Ctrl-O open  Esc quit ";
        int help_width = (int)strlen(help);
        sb_puts(&frame, "\r\n\x1b[0;7m");
        int x = put(&frame, status, max_int(v->width - help_width, 0));
        pad(&frame, v->width - help_width - x);
        put(&frame, help, v->width - max_int(x, v->width - help_width));
        sb_puts(&frame, "\x1b[0m");
    }
    int cursor = 3 + utf8_count(v->input.data ? v->input.data : "", v->input.len);
    sb_printf(&frame, "\x1b[1;%dH\x1b[?25h", min_int(cursor, v->width));
    if (write(v->tty, frame.data, frame.len) < 0) {
        /* nothing useful to do when the terminal is gone */
    }
    sb_free(&frame);
}

/* ---- input ---- */

typedef enum { ACT_NONE, ACT_QUIT, ACT_ACCEPT, ACT_INPUT_CHANGED } Action;

/*
 * wait_readable waits until one of two descriptors is readable. It uses
 * select because macOS poll does not support terminal devices.
 */
static int wait_readable(int a, int b, int timeout_ms, bool *a_ready, bool *b_ready) {
    fd_set set;
    FD_ZERO(&set);
    FD_SET(a, &set);
    if (b >= 0) FD_SET(b, &set);
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    int rc = select(max_int(a, b) + 1, &set, NULL, NULL, timeout_ms < 0 ? NULL : &tv);
    *a_ready = rc > 0 && FD_ISSET(a, &set);
    if (b_ready) *b_ready = rc > 0 && b >= 0 && FD_ISSET(b, &set);
    return rc;
}

static int read_byte(int fd, int timeout_ms) {
    bool ready;
    if (wait_readable(fd, -1, timeout_ms, &ready, NULL) <= 0 || !ready) return -1;
    unsigned char c;
    return read(fd, &c, 1) == 1 ? c : -1;
}

static void delete_last_char(StrBuf *input) {
    while (input->len > 0) {
        unsigned char c = (unsigned char)input->data[--input->len];
        if ((c & 0xC0) != 0x80) break;
    }
    sb_cstr(input);
}

static void delete_word(StrBuf *input) {
    while (input->len > 0 && input->data[input->len - 1] == ' ') input->len--;
    while (input->len > 0 && input->data[input->len - 1] != ' ') input->len--;
    sb_cstr(input);
}

static void mouse(View *v, int button, int y, bool press) {
    if (!press) return;
    if (button == 64) {
        move(v, -3);
    } else if (button == 65) {
        move(v, 3);
    } else if (button == 0 && y >= 3) {
        size_t row = v->top + (size_t)(y - 3);
        if (row < v->hits.len) v->selected = row;
    }
}

static Action escape_sequence(View *v) {
    int c = read_byte(v->tty, ESCAPE_WAIT_MS);
    if (c < 0) return ACT_QUIT; /* a lone Esc */
    if (c != '[' && c != 'O') return ACT_NONE;
    char seq[32];
    size_t n = 0;
    int b;
    while ((b = read_byte(v->tty, ESCAPE_WAIT_MS)) >= 0 && n < sizeof seq - 1) {
        seq[n++] = (char)b;
        if ((b >= 'A' && b <= 'Z') || (b >= 'a' && b <= 'z') || b == '~') break;
    }
    seq[n] = '\0';
    int page = max_int(v->height - 3, 1);
    if (seq[0] == '<') {
        int button, x, y;
        char kind;
        if (sscanf(seq + 1, "%d;%d;%d%c", &button, &x, &y, &kind) == 4) mouse(v, button, y, kind == 'M');
    } else if (!strcmp(seq, "A")) {
        move(v, -1);
    } else if (!strcmp(seq, "B")) {
        move(v, 1);
    } else if (!strcmp(seq, "5~")) {
        move(v, -page);
    } else if (!strcmp(seq, "6~")) {
        move(v, page);
    } else if (!strcmp(seq, "H") || !strcmp(seq, "1~") || !strcmp(seq, "7~")) {
        move(v, -(long)v->hits.len);
    } else if (!strcmp(seq, "F") || !strcmp(seq, "4~") || !strcmp(seq, "8~")) {
        move(v, (long)v->hits.len);
    }
    return ACT_NONE;
}

static Action key(View *v, int c) {
    StrBuf path = {0};
    switch (c) {
    case 0x1b: return escape_sequence(v);
    case 0x03: return ACT_QUIT;
    case '\r':
    case '\n': return ACT_ACCEPT;
    case 0x0f: /* Ctrl-O */
        selected_path(v, &path);
        if (path.len) open_with_system(path.data);
        sb_free(&path);
        return ACT_NONE;
    case 0x7f:
    case 0x08:
        if (!v->input.len) return ACT_NONE;
        delete_last_char(&v->input);
        return ACT_INPUT_CHANGED;
    case 0x15: /* Ctrl-U */
        if (!v->input.len) return ACT_NONE;
        sb_clear(&v->input);
        return ACT_INPUT_CHANGED;
    case 0x17: /* Ctrl-W */
        if (!v->input.len) return ACT_NONE;
        delete_word(&v->input);
        return ACT_INPUT_CHANGED;
    case 0x0e: move(v, 1); return ACT_NONE;  /* Ctrl-N */
    case 0x10: move(v, -1); return ACT_NONE; /* Ctrl-P */
    }
    if (c >= 0x20) {
        sb_putc(&v->input, (char)c);
        return ACT_INPUT_CHANGED;
    }
    return ACT_NONE;
}

/* ---- terminal setup ---- */

static bool enter_raw(int tty, struct termios *saved, Err *err) {
    if (tcgetattr(tty, saved) != 0) {
        err_set(err, "terminal: %s", strerror(errno));
        return false;
    }
    struct termios raw = *saved;
    raw.c_iflag &= ~(tcflag_t)(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    raw.c_oflag &= ~(tcflag_t)OPOST;
    raw.c_cflag |= CS8;
    raw.c_lflag &= ~(tcflag_t)(ECHO | ICANON | IEXTEN | ISIG);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    tcsetattr(tty, TCSAFLUSH, &raw);
    const char *setup = "\x1b[?1049h\x1b[?1000h\x1b[?1006h\x1b[2J";
    return write(tty, setup, strlen(setup)) >= 0;
}

static void leave_raw(int tty, const struct termios *saved) {
    const char *restore = "\x1b[?1006l\x1b[?1000l\x1b[0m\x1b[?25h\x1b[?1049l";
    if (write(tty, restore, strlen(restore)) < 0) {
        /* the terminal is gone; nothing left to restore */
    }
    tcsetattr(tty, TCSAFLUSH, saved);
}

bool tui_run(Snapshot *s, QueryDefaults defaults, char **chosen, Err *err) {
    *chosen = NULL;
    View v = {.s = s, .defaults = defaults};
    v.tty = open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (v.tty < 0) {
        err_set(err, "cannot open the terminal: %s", strerror(errno));
        return false;
    }
    if (pipe(v.wake) != 0) {
        err_set(err, "pipe: %s", strerror(errno));
        close(v.tty);
        return false;
    }
    fcntl(v.wake[0], F_SETFL, O_NONBLOCK);
    struct termios saved;
    if (!enter_raw(v.tty, &saved, err)) {
        close(v.tty);
        return false;
    }
    struct sigaction sa = {.sa_handler = on_winch}, old_sa;
    sigemptyset(&sa.sa_mask);
    sigaction(SIGWINCH, &sa, &old_sa);
    query_size(&v);
    sb_cstr(&v.input);
    v.cpu = threadpool_create(cpu_count());
    start_search(&v);

    bool accepted = false;
    for (bool running = true; running;) {
        if (resized) {
            resized = 0;
            query_size(&v);
        }
        draw(&v);
        int timeout = -1;
        if (v.settle_at) timeout = (int)max_i64(v.settle_at - monotonic_ms(), 0);
        bool key_ready, search_done;
        if (wait_readable(v.tty, v.wake[0], timeout, &key_ready, &search_done) < 0) continue; /* EINTR from a resize */
        if (v.settle_at && monotonic_ms() >= v.settle_at) {
            v.settle_at = 0;
            start_search(&v);
        }
        if (search_done && collect_searches(&v) && v.accept_when_done) {
            accepted = true;
            running = false;
        }
        if (running && key_ready) {
            int c = read_byte(v.tty, 0);
            if (c < 0) running = false; /* the terminal went away */
            switch (c < 0 ? ACT_NONE : key(&v, c)) {
            case ACT_QUIT: running = false; break;
            case ACT_ACCEPT:
                if (v.searching) {
                    v.accept_when_done = true;
                    if (!v.search) {
                        v.settle_at = 0;
                        start_search(&v);
                    }
                } else {
                    accepted = true;
                    running = false;
                }
                break;
            case ACT_INPUT_CHANGED:
                cancel_search(&v);
                v.searching = true;
                v.settle_at = monotonic_ms() + TYPING_PAUSE_MS;
                break;
            case ACT_NONE: break;
            }
        }
    }
    if (accepted) {
        StrBuf path = {0};
        selected_path(&v, &path);
        if (path.len) *chosen = path.data;
        else sb_free(&path);
    }
    cancel_search(&v);
    leave_raw(v.tty, &saved);
    sigaction(SIGWINCH, &old_sa, NULL);
    close(v.tty);
    collect_searches(&v);
    /* A search still running keeps its snapshot, its pool and the pipe it reports to; the process is about to exit. */
    if (v.outstanding == 0) {
        close(v.wake[0]);
        close(v.wake[1]);
        threadpool_destroy(v.cpu);
    }
    sb_free(&v.input);
    u32vec_free(&v.hits);
    return true;
}
