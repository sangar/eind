#ifdef __linux__

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "watcher.h"

#define WATCH_MASK                                                                                       \
    (IN_CREATE | IN_DELETE | IN_MODIFY | IN_CLOSE_WRITE | IN_ATTRIB | IN_MOVED_FROM | IN_MOVED_TO | \
     IN_DELETE_SELF | IN_MOVE_SELF | IN_ONLYDIR | IN_DONT_FOLLOW)

/* inotify needs one watch per directory; dirs maps each watch descriptor back to its path. */
struct WatchBackend {
    int fd;
    char **dirs;
    size_t dir_cap;
    StrList pending;
    size_t pending_pos;
    bool warned_limit;
};

WatchBackend *backend_open(const StrList *roots, Err *err) {
    (void)roots;
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        err_set(err, "inotify: %s", strerror(errno));
        return NULL;
    }
    WatchBackend *b = xcalloc(1, sizeof *b);
    b->fd = fd;
    return b;
}

void backend_close(WatchBackend *b) {
    close(b->fd);
    for (size_t i = 0; i < b->dir_cap; i++) free(b->dirs[i]);
    free(b->dirs);
    strlist_free(&b->pending);
    free(b);
}

bool backend_needs_dirs(void) { return true; }

void backend_add_dir(WatchBackend *b, const char *path) {
    int wd = inotify_add_watch(b->fd, path, WATCH_MASK);
    if (wd < 0) {
        if (errno == ENOSPC && !b->warned_limit) {
            b->warned_limit = true;
            fprintf(stderr,
                    "eind: out of inotify watches; raise the limit with "
                    "`sudo sysctl fs.inotify.max_user_watches=1048576`\n");
        }
        return;
    }
    if ((size_t)wd >= b->dir_cap) {
        size_t cap = b->dir_cap ? b->dir_cap : 1024;
        while (cap <= (size_t)wd) cap *= 2;
        b->dirs = xrealloc(b->dirs, cap * sizeof *b->dirs);
        memset(b->dirs + b->dir_cap, 0, (cap - b->dir_cap) * sizeof *b->dirs);
        b->dir_cap = cap;
    }
    free(b->dirs[wd]);
    b->dirs[wd] = xstrdup(path);
}

static void read_events(WatchBackend *b) {
    char buf[64 * 1024] __attribute__((aligned(__alignof__(struct inotify_event))));
    for (;;) {
        ssize_t n = read(b->fd, buf, sizeof buf);
        if (n <= 0) return;
        for (char *p = buf; p < buf + n;) {
            struct inotify_event *e = (struct inotify_event *)p;
            p += sizeof *e + e->len;
            if (e->wd < 0 || (size_t)e->wd >= b->dir_cap || !b->dirs[e->wd]) continue;
            const char *dir = b->dirs[e->wd];
            if (e->mask & IN_IGNORED) {
                free(b->dirs[e->wd]);
                b->dirs[e->wd] = NULL;
                continue;
            }
            strlist_push_owned(&b->pending, e->len && e->name[0] ? path_join(dir, e->name) : xstrdup(dir));
        }
    }
}

int backend_next(WatchBackend *b, int timeout_ms, char **path, Err *err) {
    if (b->pending_pos == b->pending.len) {
        strlist_clear(&b->pending);
        b->pending_pos = 0;
        struct pollfd pfd = {.fd = b->fd, .events = POLLIN};
        int rc = poll(&pfd, 1, timeout_ms);
        if (rc < 0 && errno != EINTR) {
            err_set(err, "inotify: %s", strerror(errno));
            return -1;
        }
        if (rc <= 0) return 0;
        read_events(b);
        if (b->pending.len == 0) return 0;
    }
    *path = b->pending.items[b->pending_pos];
    b->pending.items[b->pending_pos++] = NULL;
    return 1;
}

#endif
