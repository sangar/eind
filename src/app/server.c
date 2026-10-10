#include "server.h"

#include <stdatomic.h>
#include <string.h>

#include "mc/concurrency/cancel.h"
#include "mc/encoding/json.h"
#include "mc/platform/platform.h"
#include "mc/text/fmt.h"
#include "mc/text/path.h"
#include "../index/search.h"
#include "output.h"

enum { MAX_REQUEST_LINE = 1 << 20, ACCEPT_POLL_MS = 250, READ_CHUNK = 16384 };

typedef struct Conn Conn;

/* The server, its paths and its list of connections live in its arena; the list only under mutex. */
struct Server {
    Arena *arena;
    Index *ix;
    ThreadPool *cpu;
    String index_path;
    String socket_path;
    int listen_fd;
    atomic_bool stopping;
    Thread accept_thread;

    Mutex mutex;
    Cond idle;
    Conn **conns;
    size_t conn_count, conn_cap;
};

/* A Request, its JSON and its response live in its arena. */
typedef struct Request {
    Arena *arena;
    Conn *conn;
    const Node *body;
    Cancel cancel;
    Thread thread;
} Request;

/* A Conn and its read buffer live in its arena. */
struct Conn {
    Arena *arena;
    Server *srv;
    int fd;
    Mutex write_mutex;
};

String default_socket_path(Arena *arena) {
    String path;
    if (env_get(arena, S("EIND_SOCKET"), &path) && path.len) return path;
    if (env_get(arena, S("XDG_RUNTIME_DIR"), &path) && path.len) return path_join(arena, path, S("eind.sock"));
    if (!env_get(arena, S("TMPDIR"), &path) || path.len == 0) path = S("/tmp");
    return path_join(arena, path, str_format(arena, "eind-%d.sock", process_user_id()));
}

bool server_running(String socket_path) {
    int fd;
    if (net_connect_unix(socket_path, &fd, nullptr) != ERR_OK) return false;
    net_close(fd);
    return true;
}

/* ---- request handling ---- */

static void send_line(Conn *c, StringBuilder *out) {
    str_builder_append_char(out, '\n');
    mutex_lock(&c->write_mutex);
    if (net_send(c->fd, (String){out->data, out->len}, nullptr) != ERR_OK) net_shutdown(c->fd);
    mutex_unlock(&c->write_mutex);
}

/* append_id echoes a request's id: numbers as they were written, anything else as JSON. */
static void append_id(StringBuilder *out, const Node *id) {
    if (id->kind == NODE_INT || id->kind == NODE_FLOAT) {
        str_builder_append(out, id->text);
    } else if (id->kind == NODE_STRING) {
        json_append_quoted(out, id->text);
    } else {
        str_builder_append(out, json_encode(out->arena, id));
    }
}

static StringBuilder begin_response(Arena *arena, const Node *body) {
    StringBuilder out = str_builder_create(arena, 1024);
    const Node *id = body ? node_get(body, S("id")) : nullptr;
    str_builder_append_char(&out, '{');
    if (id) {
        str_builder_append(&out, S("\"id\":"));
        append_id(&out, id);
        str_builder_append_char(&out, ',');
    }
    return out;
}

static void send_error(Conn *c, Arena *arena, const Node *body, String msg) {
    StringBuilder out = begin_response(arena, body);
    str_builder_append(&out, S("\"error\":"));
    json_append_quoted(&out, msg);
    str_builder_append_char(&out, '}');
    send_line(c, &out);
}

static void send_cancelled(Request *r) {
    StringBuilder out = begin_response(r->arena, r->body);
    str_builder_append(&out, S("\"cancelled\":true}"));
    send_line(r->conn, &out);
}

/* request_int reads a number field, accepting a fraction as JSON clients may send one. */
static int64_t request_int(const Node *body, const char *key, int64_t fallback) {
    const Node *n = node_get(body, S(key));
    if (n && n->kind == NODE_INT) return n->integer;
    if (n && n->kind == NODE_FLOAT && n->number > -9.2e18 && n->number < 9.2e18) return (int64_t)n->number;
    return fallback;
}

static bool request_bool(const Node *body, const char *key) { return node_get_bool(body, S(key), false); }

static String request_string(const Node *body, const char *key) { return node_get_string(body, S(key), S("")); }

static void handle_status(Request *r) {
    Server *srv = r->conn->srv;
    Snapshot *s = index_acquire(srv->ix);
    int64_t files, dirs;
    snap_stats(s, &files, &dirs);
    StringBuilder out = begin_response(r->arena, r->body);
    str_builder_append_format(&out, "\"files\":%lld,\"folders\":%lld,\"roots\":[", (long long)files, (long long)dirs);
    for (size_t i = 0; i < s->roots.count; i++) {
        if (i) str_builder_append_char(&out, ',');
        json_append_quoted(&out, s->roots.items[i]);
    }
    String built = fmt_rfc3339(r->arena, s->built_at * NS_PER_SECOND, clock_local_offset(s->built_at), false);
    str_builder_append_format(&out, "],\"built\":\"%.*s\",\"index\":", (int)built.len, built.data);
    json_append_quoted(&out, srv->index_path);
    str_builder_append_char(&out, '}');
    send_line(r->conn, &out);
    snapshot_release(s);
}

static void send_results(Request *r, const Snapshot *s, const uint32_t *hits, size_t total, size_t count, int64_t start) {
    StringBuilder out = begin_response(r->arena, r->body);
    StringBuilder path = str_builder_create(r->arena, 256);
    Arena *temporary = arena_create(4096);
    double elapsed_ms = (double)(clock_monotonic_ns() - start) / (double)NS_PER_MILLISECOND;
    str_builder_append_format(&out, "\"total\":%zu,\"elapsed_ms\":%.6g,\"results\":[", total, elapsed_ms);
    for (size_t k = 0; k < count; k++) {
        OutRecord rec = record_of(s, hits[k], &path);
        if (k) str_builder_append_char(&out, ',');
        record_json(&out, temporary, &rec);
    }
    str_builder_append(&out, S("]}"));
    send_line(r->conn, &out);
    arena_destroy(temporary);
}

static void handle_search(Request *r) {
    int64_t start = clock_monotonic_ns();
    const Node *body = r->body;
    Err err;
    SortKey sort = SORT_RELEVANCE;
    String sort_name = request_string(body, "sort");
    if (sort_name.len && sort_key_parse(sort_name, &sort, &err) != ERR_OK) {
        send_error(r->conn, r->arena, body, S(err.msg));
        return;
    }
    QueryDefaults defaults = {.regex = request_bool(body, "regex"),
                              .case_sensitive = request_bool(body, "case"),
                              .whole_word = request_bool(body, "whole_word"),
                              .match_path = request_bool(body, "match_path")};
    QueryNode *node;
    if (query_parse(r->arena, request_string(body, "query"), defaults, &node, &err) != ERR_OK) {
        send_error(r->conn, r->arena, body, S(err.msg));
        return;
    }
    node = query_restrict(r->arena, node, request_string(body, "path"), request_bool(body, "files"),
                          request_bool(body, "dirs"));

    Snapshot *s = index_acquire(r->conn->srv->ix);
    IdList hits;
    Error e = search_run(r->conn->srv->cpu, r->arena, s, node, &r->cancel, &hits, &err);
    if (e == ERR_CANCELLED) {
        send_cancelled(r);
    } else if (e != ERR_OK) {
        send_error(r->conn, r->arena, body, S(err.msg));
    } else {
        size_t total = hits.count;
        int64_t offset_value = request_int(body, "offset", 0);
        size_t offset = offset_value > 0 ? min_size((size_t)offset_value, total) : 0;
        int64_t limit = request_int(body, "limit", DEFAULT_LIMIT);
        size_t kept = 0;
        if (limit != 0) {
            int64_t keep = limit < 0 || limit > INT64_MAX - (int64_t)offset ? -1 : (int64_t)offset + limit;
            Arena *scratch = arena_create(0);
            kept = sort == SORT_RELEVANCE ? search_rank(scratch, s, hits.items, total, node, keep)
                                          : search_top(scratch, s, hits.items, total, sort, request_bool(body, "descending"), keep);
            arena_destroy(scratch);
        }
        size_t from = min_size(offset, kept);
        size_t count = kept - from;
        if (limit >= 0 && (uint64_t)limit < count) count = (size_t)limit;
        if (cancel_requested(&r->cancel)) {
            send_cancelled(r);
        } else {
            send_results(r, s, hits.items + from, total, count, start);
        }
    }
    snapshot_release(s);
}

static void *run_request(void *argument) {
    Request *r = argument;
    String op = request_string(r->body, "op");
    if (op.len == 0 || str_equal(op, S("search"))) {
        handle_search(r);
    } else if (str_equal(op, S("status"))) {
        handle_status(r);
    } else {
        send_error(r->conn, r->arena, r->body, str_format(r->arena, "unknown op \"%.*s\"", (int)op.len, op.data));
    }
    return nullptr;
}

static void finish_request(Request *r) {
    if (!r) return;
    cancel_request(&r->cancel);
    thread_join(&r->thread);
    cancel_destroy(&r->cancel);
    arena_destroy(r->arena);
}

/* ---- connections ---- */

static void track(Server *srv, Conn *c) {
    mutex_lock(&srv->mutex);
    srv->conns = arena_grow(srv->arena, srv->conns, &srv->conn_cap, srv->conn_count, sizeof *srv->conns);
    srv->conns[srv->conn_count++] = c;
    mutex_unlock(&srv->mutex);
}

static void untrack(Server *srv, Conn *c) {
    mutex_lock(&srv->mutex);
    for (size_t i = 0; i < srv->conn_count; i++) {
        if (srv->conns[i] == c) {
            srv->conns[i] = srv->conns[--srv->conn_count];
            break;
        }
    }
    if (srv->conn_count == 0) cond_broadcast(&srv->idle);
    mutex_unlock(&srv->mutex);
}

static void handle_line(Conn *c, Request **previous, String line) {
    line = str_trim(line);
    if (line.len == 0) return;
    Arena *arena = arena_create(8192);
    Request *r = arena_push(arena, sizeof *r);
    r->arena = arena;
    r->conn = c;
    Err err;
    Node *body;
    Error e = json_parse(arena, str_copy(arena, line), &body, &err);
    if (e != ERR_OK || body->kind != NODE_MAPPING) {
        send_error(c, arena, nullptr,
                   str_format(arena, "invalid request: %s", e == ERR_OK ? "expected a JSON object" : err.msg));
        arena_destroy(arena);
        return;
    }
    r->body = body;
    finish_request(*previous);
    *previous = nullptr;
    cancel_init(&r->cancel);
    if (thread_start(&r->thread, run_request, r, &err) != ERR_OK) {
        send_error(c, arena, body, S(err.msg));
        cancel_destroy(&r->cancel);
        arena_destroy(arena);
        return;
    }
    *previous = r;
}

static void *serve_conn(void *argument) {
    Conn *c = argument;
    Request *previous = nullptr;
    StringBuilder buf = str_builder_create(c->arena, READ_CHUNK);
    char chunk[READ_CHUNK];
    for (;;) {
        size_t got;
        if (net_receive(c->fd, chunk, sizeof chunk, &got, nullptr) != ERR_OK || got == 0) break;
        str_builder_append(&buf, (String){chunk, got});
        size_t start = 0, nl;
        while (str_find_char((String){buf.data + start, buf.len - start}, '\n', &nl)) {
            handle_line(c, &previous, (String){buf.data + start, nl});
            start += nl + 1;
        }
        memmove(buf.data, buf.data + start, buf.len - start);
        buf.len -= start;
        if (buf.len > MAX_REQUEST_LINE) break;
    }
    if (buf.len > 0 && buf.len <= MAX_REQUEST_LINE) handle_line(c, &previous, (String){buf.data, buf.len});
    finish_request(previous);
    net_close(c->fd);
    untrack(c->srv, c);
    mutex_destroy(&c->write_mutex);
    arena_destroy(c->arena);
    return nullptr;
}

static void *accept_loop(void *argument) {
    Server *srv = argument;
    while (!atomic_load(&srv->stopping)) {
        int fd;
        if (!net_accept(srv->listen_fd, ACCEPT_POLL_MS, &fd)) continue;
        Arena *arena = arena_create(4 * READ_CHUNK);
        Conn *c = arena_push(arena, sizeof *c);
        c->arena = arena;
        c->srv = srv;
        c->fd = fd;
        mutex_init(&c->write_mutex);
        track(srv, c);
        Thread thread;
        if (thread_start(&thread, serve_conn, c, nullptr) != ERR_OK) {
            untrack(srv, c);
            mutex_destroy(&c->write_mutex);
            net_close(fd);
            arena_destroy(arena);
            continue;
        }
        thread_detach(&thread);
    }
    return nullptr;
}

[[nodiscard]] static Error listen_unix(String path, int *fd, Err *err) {
    if (file_exists(path)) {
        if (server_running(path))
            return err_set(err, ERR_IO, "another eind daemon is already serving %.*s", (int)path.len, path.data);
        Error e = file_remove(path, err);
        if (e != ERR_OK) return e;
    }
    Error e = dir_create_all(path_dir(path), 0700, err);
    return e == ERR_OK ? net_listen_unix(path, 0600, fd, err) : e;
}

Error server_start(ThreadPool *cpu, Index *ix, String socket_path, String index_path, Server **server, Err *err) {
    *server = nullptr;
    Arena *arena = arena_create(4096);
    int fd;
    Error e = listen_unix(socket_path, &fd, err);
    if (e != ERR_OK) {
        arena_destroy(arena);
        return e;
    }
    Server *srv = arena_push(arena, sizeof *srv);
    srv->arena = arena;
    srv->ix = ix;
    srv->cpu = cpu;
    srv->index_path = str_copy(arena, index_path);
    srv->socket_path = str_copy(arena, socket_path);
    srv->listen_fd = fd;
    atomic_init(&srv->stopping, false);
    mutex_init(&srv->mutex);
    cond_init(&srv->idle);
    e = thread_start(&srv->accept_thread, accept_loop, srv, err);
    if (e != ERR_OK) {
        net_close(fd);
        (void)file_remove(srv->socket_path, nullptr);
        mutex_destroy(&srv->mutex);
        cond_destroy(&srv->idle);
        arena_destroy(arena);
        return e;
    }
    *server = srv;
    return ERR_OK;
}

void server_stop(Server *srv) {
    atomic_store(&srv->stopping, true);
    thread_join(&srv->accept_thread);
    net_close(srv->listen_fd);
    /* A socket already gone is what stopping wants anyway. */
    (void)file_remove(srv->socket_path, nullptr);
    mutex_lock(&srv->mutex);
    for (size_t i = 0; i < srv->conn_count; i++) net_shutdown(srv->conns[i]->fd);
    while (srv->conn_count > 0) cond_wait(&srv->idle, &srv->mutex);
    mutex_unlock(&srv->mutex);
    mutex_destroy(&srv->mutex);
    cond_destroy(&srv->idle);
    arena_destroy(srv->arena);
}
