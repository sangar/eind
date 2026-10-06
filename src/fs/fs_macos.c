#ifdef __APPLE__

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/attr.h>
#include <sys/vnode.h>
#include <unistd.h>

#include "../core/queue.h"
#include "fs.h"
#include "watcher.h"

/*
 * getattrlistbulk returns names and metadata for many entries per call,
 * where readdir needs one more stat per entry. Attributes come packed in
 * bit order, except the error, which follows the returned-attribute set.
 */
bool fs_read_dir(int dirfd, DirSkipFn skip, DirEntryFn add, void *ctx) {
    struct attrlist attrs = {
        .bitmapcount = ATTR_BIT_MAP_COUNT,
        .commonattr = ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE |
                      ATTR_CMN_CRTIME | ATTR_CMN_MODTIME,
        .fileattr = ATTR_FILE_DATALENGTH,
    };
    char buf[64 * 1024] __attribute__((aligned(8)));
    for (;;) {
        int count = getattrlistbulk(dirfd, &attrs, buf, sizeof buf, 0);
        if (count < 0) return errno == ENOTSUP || errno == EINVAL ? fs_read_dir_portable(dirfd, skip, add, ctx) : false;
        if (count == 0) return true;
        char *entry = buf;
        for (int i = 0; i < count; i++) {
            char *p = entry;
            uint32_t length;
            memcpy(&length, p, sizeof length);
            entry += length;
            p += sizeof length;
            attribute_set_t returned;
            memcpy(&returned, p, sizeof returned);
            p += sizeof returned;
            if (returned.commonattr & ATTR_CMN_ERROR) {
                uint32_t error;
                memcpy(&error, p, sizeof error);
                p += sizeof error;
                if (error) continue;
            }
            attrreference_t name_ref;
            memcpy(&name_ref, p, sizeof name_ref);
            const char *name = p + name_ref.attr_dataoffset;
            p += sizeof name_ref;
            if (skip(ctx, name)) continue;
            fsobj_type_t type;
            memcpy(&type, p, sizeof type);
            p += sizeof type;
            struct timespec created, modified;
            memcpy(&created, p, sizeof created);
            p += sizeof created;
            memcpy(&modified, p, sizeof modified);
            p += sizeof modified;
            struct stat st = {0};
            st.st_mode = type == VDIR ? S_IFDIR : type == VLNK ? S_IFLNK : S_IFREG;
            st.st_mtime = modified.tv_sec;
            st.st_birthtimespec = created;
            if (returned.fileattr & ATTR_FILE_DATALENGTH) memcpy(&st.st_size, p, sizeof st.st_size);
            add(ctx, name, &st);
        }
    }
}

/* FSEvents watches whole trees and delivers on a dispatch queue; paths are handed over through a Queue. */
struct WatchBackend {
    FSEventStreamRef stream;
    dispatch_queue_t dispatch;
    Queue events;
};

static void on_events(ConstFSEventStreamRef stream, void *info, size_t count, void *paths,
                      const FSEventStreamEventFlags flags[], const FSEventStreamEventId ids[]) {
    (void)stream;
    (void)flags;
    (void)ids;
    WatchBackend *b = info;
    char **list = paths;
    for (size_t i = 0; i < count; i++) queue_push(&b->events, xstrdup(list[i]));
}

WatchBackend *backend_open(const StrList *roots, Err *err) {
    WatchBackend *b = xcalloc(1, sizeof *b);
    queue_init(&b->events);
    CFMutableArrayRef paths = CFArrayCreateMutable(NULL, (CFIndex)roots->len, &kCFTypeArrayCallBacks);
    for (size_t i = 0; i < roots->len; i++) {
        CFStringRef s = CFStringCreateWithCString(NULL, roots->items[i], kCFStringEncodingUTF8);
        CFArrayAppendValue(paths, s);
        CFRelease(s);
    }
    FSEventStreamContext ctx = {.info = b};
    b->stream = FSEventStreamCreate(NULL, on_events, &ctx, paths, kFSEventStreamEventIdSinceNow, 0.1,
                                    kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
                                        kFSEventStreamCreateFlagWatchRoot);
    CFRelease(paths);
    if (!b->stream) {
        err_set(err, "FSEvents: cannot create event stream");
        queue_destroy(&b->events);
        free(b);
        return NULL;
    }
    b->dispatch = dispatch_queue_create("eind.fsevents", DISPATCH_QUEUE_SERIAL);
    FSEventStreamSetDispatchQueue(b->stream, b->dispatch);
    if (!FSEventStreamStart(b->stream)) {
        err_set(err, "FSEvents: cannot start event stream");
        FSEventStreamInvalidate(b->stream);
        FSEventStreamRelease(b->stream);
        dispatch_release(b->dispatch);
        queue_destroy(&b->events);
        free(b);
        return NULL;
    }
    return b;
}

/* Running an empty task on the serial queue waits out a callback still in flight. */
static void drain_nothing(void *ctx) { (void)ctx; }

void backend_close(WatchBackend *b) {
    FSEventStreamStop(b->stream);
    FSEventStreamInvalidate(b->stream);
    FSEventStreamRelease(b->stream);
    dispatch_sync_f(b->dispatch, NULL, drain_nothing);
    dispatch_release(b->dispatch);
    void *path;
    while (queue_pop_timeout(&b->events, 0, &path)) free(path);
    queue_destroy(&b->events);
    free(b);
}

bool backend_needs_dirs(void) { return false; }

void backend_add_dir(WatchBackend *b, const char *path) {
    (void)b;
    (void)path;
}

int backend_next(WatchBackend *b, int timeout_ms, char **path, Err *err) {
    (void)err;
    void *item;
    if (!queue_pop_timeout(&b->events, timeout_ms, &item)) return 0;
    *path = item;
    return 1;
}

#endif
