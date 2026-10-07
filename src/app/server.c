#include "server.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "../index/search.h"
#include "output.h"

/* sun_path holds 104 bytes on macOS and the BSDs, 108 on Linux; leave room for the NUL. */
#define MAX_SOCKET_PATH 103
#define MAX_REQUEST_LINE (1 << 20)

typedef struct Conn Conn;

struct Server {
    Index *ix;
    ThreadPool *cpu;
    char *index_path;
    char *socket_path;
    int listen_fd;
    atomic_bool stopping;
    pthread_t accept_thread;

    pthread_mutex_t mu;
    pthread_cond_t idle;
    Conn **conns;
    size_t conn_count, conn_cap;
};

typedef struct Request {
    Conn *conn;
    Arena arena;
    const JsonValue *body;
    atomic_int cancel;
    pthread_t thread;
} Request;

struct Conn {
    Server *srv;
    int fd;
    pthread_mutex_t write_mu;
};

char *default_socket_path(void) {
    const char *p = getenv("EIND_SOCKET");
    if (p && *p) return xstrdup(p);
    const char *runtime = getenv("XDG_RUNTIME_DIR");
    if (runtime && *runtime) return path_join(runtime, "eind.sock");
    const char *tmp = getenv("TMPDIR");
    StrBuf sb = {0};
    sb_printf(&sb, "eind-%d.sock", (int)getuid());
    char *path = path_join(tmp && *tmp ? tmp : "/tmp", sb.data);
    sb_free(&sb);
    return path;
}

static int connect_unix(const char *path) {
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof addr.sun_path) return -1;
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    if (connect(fd, (struct sockaddr *)&addr, sizeof addr) != 0) {
        close(fd);
        return -1;
    }
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
    return fd;
}

bool server_running(const char *socket_path) {
    int fd = connect_unix(socket_path);
    if (fd < 0) return false;
    close(fd);
    return true;
}

static bool write_all(int fd, const char *data, size_t len) {
    while (len > 0) {
        ssize_t n = write(fd, data, len);
        if (n < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        data += n;
        len -= (size_t)n;
    }
    return true;
}

/* ---- request handling ---- */

static void send_line(Conn *c, StrBuf *sb) {
    sb_putc(sb, '\n');
    pthread_mutex_lock(&c->write_mu);
    if (!write_all(c->fd, sb->data, sb->len)) shutdown(c->fd, SHUT_RDWR);
    pthread_mutex_unlock(&c->write_mu);
}

static void begin_response(StrBuf *sb, const JsonValue *body) {
    const JsonValue *id = json_get(body, "id");
    sb_putc(sb, '{');
    if (id) {
        sb_puts(sb, "\"id\":");
        sb_append(sb, id->raw, id->raw_len);
        sb_putc(sb, ',');
    }
}

static void send_error(Conn *c, const JsonValue *body, const char *msg) {
    StrBuf sb = {0};
    begin_response(&sb, body);
    sb_puts(&sb, "\"error\":");
    json_write_string(&sb, msg, strlen(msg));
    sb_putc(&sb, '}');
    send_line(c, &sb);
    sb_free(&sb);
}

static void send_cancelled(Conn *c, const JsonValue *body) {
    StrBuf sb = {0};
    begin_response(&sb, body);
    sb_puts(&sb, "\"cancelled\":true}");
    send_line(c, &sb);
    sb_free(&sb);
}

static void handle_status(Request *r) {
    Snapshot *s = index_acquire(r->conn->srv->ix);
    int64_t files, dirs;
    snap_stats(s, &files, &dirs);
    StrBuf sb = {0};
    char built[40];
    begin_response(&sb, r->body);
    sb_printf(&sb, "\"files\":%lld,\"folders\":%lld,\"roots\":[", (long long)files, (long long)dirs);
    for (size_t i = 0; i < s->roots.len; i++) {
        if (i) sb_putc(&sb, ',');
        json_write_string(&sb, s->roots.items[i], strlen(s->roots.items[i]));
    }
    sb_printf(&sb, "],\"built\":\"%s\",\"index\":", format_rfc3339(s->built_at, built));
    json_write_string(&sb, r->conn->srv->index_path, strlen(r->conn->srv->index_path));
    sb_putc(&sb, '}');
    send_line(r->conn, &sb);
    sb_free(&sb);
    snapshot_release(s);
}

static void handle_search(Request *r) {
    int64_t start = monotonic_us();
    const JsonValue *body = r->body;
    Err err;
    SortKey sort = SORT_RELEVANCE;
    const char *sort_name = json_string(body, "sort", "");
    if (*sort_name && !sort_key_parse(sort_name, &sort, &err)) {
        send_error(r->conn, body, err.msg);
        return;
    }
    QueryDefaults defaults = {.regex = json_bool(body, "regex"),
                              .case_sensitive = json_bool(body, "case"),
                              .whole_word = json_bool(body, "whole_word"),
                              .match_path = json_bool(body, "match_path")};
    QueryNode *node = query_parse(&r->arena, json_string(body, "query", ""), defaults, &err);
    if (!node) {
        send_error(r->conn, body, err.msg);
        return;
    }
    node = query_restrict(&r->arena, node, json_string(body, "path", ""), json_bool(body, "files"), json_bool(body, "dirs"));

    Snapshot *s = index_acquire(r->conn->srv->ix);
    U32Vec hits = {0};
    SearchStatus status = search_run(r->conn->srv->cpu, s, node, &r->cancel, &hits, &err);
    if (status == SEARCH_CANCELLED) {
        send_cancelled(r->conn, body);
    } else if (status == SEARCH_ERROR) {
        send_error(r->conn, body, err.msg);
    } else {
        size_t total = hits.len;
        double offset_value = json_number(body, "offset", 0);
        size_t offset = offset_value > 0 ? min_size((size_t)offset_value, total) : 0;
        long limit = (long)json_number(body, "limit", DEFAULT_LIMIT);
        size_t kept = 0;
        if (limit != 0) {
            long keep = limit < 0 ? -1 : (long)offset + limit;
            kept = sort == SORT_RELEVANCE ? search_rank(s, hits.data, total, node, keep)
                                          : search_top(s, hits.data, total, sort, json_bool(body, "descending"), keep);
        }
        size_t from = min_size(offset, kept);
        size_t count = kept - from;
        if (limit >= 0 && (size_t)limit < count) count = (size_t)limit;
        if (atomic_load(&r->cancel)) {
            send_cancelled(r->conn, body);
        } else {
            StrBuf sb = {0}, path = {0};
            begin_response(&sb, body);
            sb_printf(&sb, "\"total\":%zu,\"elapsed_ms\":%.6g,\"results\":[", total, elapsed_ms_since(start));
            for (size_t k = 0; k < count; k++) {
                OutRecord rec;
                record_of(s, hits.data[from + k], &rec, &path);
                if (k) sb_putc(&sb, ',');
                record_json(&sb, &rec);
            }
            sb_puts(&sb, "]}");
            send_line(r->conn, &sb);
            sb_free(&sb);
            sb_free(&path);
        }
    }
    u32vec_free(&hits);
    snapshot_release(s);
}

static void *run_request(void *arg) {
    Request *r = arg;
    const char *op = json_string(r->body, "op", "");
    if (!*op || strcmp(op, "search") == 0) {
        handle_search(r);
    } else if (strcmp(op, "status") == 0) {
        handle_status(r);
    } else {
        char msg[300];
        snprintf(msg, sizeof msg, "unknown op \"%s\"", op);
        send_error(r->conn, r->body, msg);
    }
    return NULL;
}

static void finish_request(Request *r) {
    if (!r) return;
    atomic_store(&r->cancel, 1);
    pthread_join(r->thread, NULL);
    arena_free(&r->arena);
    free(r);
}

/* ---- connections ---- */

static void track(Server *srv, Conn *c) {
    pthread_mutex_lock(&srv->mu);
    if (srv->conn_count == srv->conn_cap) {
        srv->conn_cap = srv->conn_cap ? srv->conn_cap * 2 : 8;
        srv->conns = xrealloc(srv->conns, srv->conn_cap * sizeof *srv->conns);
    }
    srv->conns[srv->conn_count++] = c;
    pthread_mutex_unlock(&srv->mu);
}

static void untrack(Server *srv, Conn *c) {
    pthread_mutex_lock(&srv->mu);
    for (size_t i = 0; i < srv->conn_count; i++) {
        if (srv->conns[i] == c) {
            srv->conns[i] = srv->conns[--srv->conn_count];
            break;
        }
    }
    if (srv->conn_count == 0) pthread_cond_broadcast(&srv->idle);
    pthread_mutex_unlock(&srv->mu);
}

static void handle_line(Conn *c, Request **previous, char *line, size_t len) {
    while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\r' || line[len - 1] == '\t')) len--;
    while (len > 0 && (*line == ' ' || *line == '\t')) line++, len--;
    if (len == 0) return;
    Request *r = xcalloc(1, sizeof *r);
    r->conn = c;
    arena_init(&r->arena, 8192);
    Err err;
    const char *text = arena_strndup(&r->arena, line, len); /* responses echo the id's raw bytes */
    r->body = json_parse(&r->arena, text, len, &err);
    if (!r->body || r->body->type != JSON_OBJECT) {
        char msg[600];
        snprintf(msg, sizeof msg, "invalid request: %s", r->body ? "expected a JSON object" : err.msg);
        send_error(c, NULL, msg);
        arena_free(&r->arena);
        free(r);
        return;
    }
    finish_request(*previous);
    atomic_init(&r->cancel, 0);
    pthread_create(&r->thread, NULL, run_request, r);
    *previous = r;
}

static void *serve_conn(void *arg) {
    Conn *c = arg;
    Request *previous = NULL;
    StrBuf buf = {0};
    char chunk[16384];
    for (;;) {
        ssize_t n = read(c->fd, chunk, sizeof chunk);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        sb_append(&buf, chunk, (size_t)n);
        char *start = buf.data, *nl;
        while ((nl = memchr(start, '\n', buf.len - (size_t)(start - buf.data))) != NULL) {
            handle_line(c, &previous, start, (size_t)(nl - start));
            start = nl + 1;
        }
        size_t rest = buf.len - (size_t)(start - buf.data);
        memmove(buf.data, start, rest);
        buf.len = rest;
        if (buf.len > MAX_REQUEST_LINE) break;
    }
    if (buf.len > 0 && buf.len <= MAX_REQUEST_LINE) handle_line(c, &previous, buf.data, buf.len);
    finish_request(previous);
    sb_free(&buf);
    close(c->fd);
    untrack(c->srv, c);
    pthread_mutex_destroy(&c->write_mu);
    free(c);
    return NULL;
}

static void *accept_loop(void *arg) {
    Server *srv = arg;
    while (!atomic_load(&srv->stopping)) {
        struct pollfd pfd = {.fd = srv->listen_fd, .events = POLLIN};
        if (poll(&pfd, 1, 250) <= 0) continue;
        int fd = accept(srv->listen_fd, NULL, NULL);
        if (fd < 0) continue;
#ifdef SO_NOSIGPIPE
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
        Conn *c = xcalloc(1, sizeof *c);
        c->srv = srv;
        c->fd = fd;
        pthread_mutex_init(&c->write_mu, NULL);
        track(srv, c);
        pthread_t t;
        pthread_create(&t, NULL, serve_conn, c);
        pthread_detach(t);
    }
    return NULL;
}

static int listen_unix(const char *path, Err *err) {
    if (strlen(path) > MAX_SOCKET_PATH) {
        err_set(err, "socket path \"%s\" is longer than %d bytes; choose a shorter --socket", path, MAX_SOCKET_PATH);
        return -1;
    }
    struct stat st;
    if (stat(path, &st) == 0) {
        if (server_running(path)) {
            err_set(err, "another eind daemon is already serving %s", path);
            return -1;
        }
        if (unlink(path) != 0) {
            err_set(err, "%s: %s", path, strerror(errno));
            return -1;
        }
    }
    char *dir = path_dir(path);
    bool ok = mkdir_p(dir, 0700, err);
    free(dir);
    if (!ok) return -1;
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    strcpy(addr.sun_path, path);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0 || bind(fd, (struct sockaddr *)&addr, sizeof addr) != 0 || listen(fd, 64) != 0) {
        err_set(err, "listen on %s: %s", path, strerror(errno));
        if (fd >= 0) close(fd);
        return -1;
    }
    chmod(path, 0600);
    return fd;
}

Server *server_start(ThreadPool *cpu, Index *ix, const char *socket_path, const char *index_path, Err *err) {
    int fd = listen_unix(socket_path, err);
    if (fd < 0) return NULL;
    Server *srv = xcalloc(1, sizeof *srv);
    srv->ix = ix;
    srv->cpu = cpu;
    srv->index_path = xstrdup(index_path);
    srv->socket_path = xstrdup(socket_path);
    srv->listen_fd = fd;
    atomic_init(&srv->stopping, false);
    pthread_mutex_init(&srv->mu, NULL);
    pthread_cond_init(&srv->idle, NULL);
    pthread_create(&srv->accept_thread, NULL, accept_loop, srv);
    return srv;
}

/* server_stop closes the listener and every open connection, so shutdown never waits on an idle client. */
void server_stop(Server *srv) {
    atomic_store(&srv->stopping, true);
    pthread_join(srv->accept_thread, NULL);
    close(srv->listen_fd);
    unlink(srv->socket_path);
    pthread_mutex_lock(&srv->mu);
    for (size_t i = 0; i < srv->conn_count; i++) shutdown(srv->conns[i]->fd, SHUT_RDWR);
    while (srv->conn_count > 0) pthread_cond_wait(&srv->idle, &srv->mu);
    pthread_mutex_unlock(&srv->mu);
    pthread_mutex_destroy(&srv->mu);
    pthread_cond_destroy(&srv->idle);
    free(srv->conns);
    free(srv->index_path);
    free(srv->socket_path);
    free(srv);
}
