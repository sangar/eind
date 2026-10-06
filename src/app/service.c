#include "service.h"

#include <errno.h>
#include <limits.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

extern char **environ;

#define LABEL "eind"

char *service_unit_path(Err *err) {
#if defined(__APPLE__)
    (void)err;
    return path_join(home_dir(), "Library/LaunchAgents/" LABEL ".plist");
#elif defined(__linux__)
    (void)err;
    const char *config_home = getenv("XDG_CONFIG_HOME");
    char *base = config_home && *config_home ? xstrdup(config_home) : path_join(home_dir(), ".config");
    char *path = path_join(base, "systemd/user/" LABEL ".service");
    free(base);
    return path;
#else
    err_set(err, "no user service manager known for this system; start `eind serve` from your session startup instead");
    return NULL;
#endif
}

/* run_command runs a service manager command and puts its output into the error when it fails. */
static bool run_command(char *const argv[], Err *err) {
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        err_set(err, "pipe: %s", strerror(errno));
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pipefd[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pipefd[0]);
    pid_t pid;
    int rc = posix_spawnp(&pid, argv[0], &actions, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&actions);
    close(pipefd[1]);
    StrBuf out = {0};
    char buf[4096];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof buf)) > 0) sb_append(&out, buf, (size_t)n);
    close(pipefd[0]);
    int status = 0;
    bool ok = rc == 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
    if (!ok) {
        StrBuf cmd = {0};
        for (size_t i = 0; argv[i]; i++) sb_printf(&cmd, "%s%s", i ? " " : "", argv[i]);
        while (out.len > 0 && (out.data[out.len - 1] == '\n' || out.data[out.len - 1] == ' ')) out.len--;
        err_set(err, "%s: %s%s", sb_cstr(&cmd), rc ? strerror(rc) : "failed: ", rc ? "" : sb_cstr(&out));
        sb_free(&cmd);
    }
    sb_free(&out);
    return ok;
}

static void xml_escape(StrBuf *sb, const char *s) {
    for (; *s; s++) {
        switch (*s) {
        case '&': sb_puts(sb, "&amp;"); break;
        case '<': sb_puts(sb, "&lt;"); break;
        case '>': sb_puts(sb, "&gt;"); break;
        case '"': sb_puts(sb, "&#34;"); break;
        case '\'': sb_puts(sb, "&#39;"); break;
        default: sb_putc(sb, *s);
        }
    }
}

#ifdef __APPLE__

static void launchd_target(char *buf, size_t cap) { snprintf(buf, cap, "gui/%d/" LABEL, (int)getuid()); }

/*
 * bootout unloads the agent if launchd has it and waits until it is gone:
 * launchctl returns as soon as the stop signal is sent, and a bootstrap while
 * the old registration lingers fails with an I/O error.
 */
static bool bootout(Err *err) {
    char target[64];
    launchd_target(target, sizeof target);
    char *argv[] = {"launchctl", "bootout", target, NULL};
    if (!run_command(argv, err)) return strstr(err->msg, "No such process") != NULL;
    char *print[] = {"launchctl", "print", target, NULL};
    for (int i = 0; i < 150; i++) {
        Err ignored;
        if (!run_command(print, &ignored)) return true;
        usleep(100 * 1000);
    }
    err_set(err, "launchctl bootout: the eind agent did not stop within 15 seconds");
    return false;
}

static void definition(StrBuf *sb, const char *executable) {
    char *log = path_join(home_dir(), "Library/Logs/" LABEL ".log");
    sb_puts(sb,
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
            "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
            "<plist version=\"1.0\"><dict>\n"
            "  <key>Label</key><string>" LABEL "</string>\n"
            "  <key>ProgramArguments</key><array><string>");
    xml_escape(sb, executable);
    sb_puts(sb, "</string><string>serve</string></array>\n"
                "  <key>RunAtLoad</key><true/>\n"
                "  <key>KeepAlive</key><true/>\n"
                "  <key>StandardOutPath</key><string>");
    xml_escape(sb, log);
    sb_puts(sb, "</string>\n  <key>StandardErrorPath</key><string>");
    xml_escape(sb, log);
    sb_puts(sb, "</string>\n</dict></plist>\n");
    free(log);
}

static bool start(const char *unit, Err *err) {
    if (!bootout(err)) return false;
    char domain[32];
    snprintf(domain, sizeof domain, "gui/%d", (int)getuid());
    char *argv[] = {"launchctl", "bootstrap", domain, (char *)unit, NULL};
    return run_command(argv, err);
}

static bool stop(Err *err) { return bootout(err); }

static bool after_remove(Err *err) {
    (void)err;
    return true;
}

#else

static void definition(StrBuf *sb, const char *executable) {
    (void)xml_escape;
    sb_printf(sb,
              "[Unit]\n"
              "Description=eind file index daemon\n"
              "\n"
              "[Service]\n"
              "ExecStart=\"%s\" serve\n"
              "Restart=on-failure\n"
              "RestartSec=5\n"
              "\n"
              "[Install]\n"
              "WantedBy=default.target\n",
              executable);
}

static bool start(const char *unit, Err *err) {
    (void)unit;
    char *reload[] = {"systemctl", "--user", "daemon-reload", NULL};
    char *enable[] = {"systemctl", "--user", "enable", "--now", LABEL ".service", NULL};
    return run_command(reload, err) && run_command(enable, err);
}

static bool stop(Err *err) {
    char *argv[] = {"systemctl", "--user", "disable", "--now", LABEL ".service", NULL};
    return run_command(argv, err);
}

static bool after_remove(Err *err) {
    char *argv[] = {"systemctl", "--user", "daemon-reload", NULL};
    return run_command(argv, err);
}

#endif

bool service_enable(const char *executable, Err *err) {
    char *unit = service_unit_path(err);
    if (!unit) return false;
    StrBuf text = {0};
    definition(&text, executable);
    bool ok = write_file_atomic(unit, text.data, text.len, 0644, err) && start(unit, err);
    sb_free(&text);
    free(unit);
    return ok;
}

bool service_disable(Err *err) {
    char *unit = service_unit_path(err);
    if (!unit) return false;
    struct stat st;
    bool ok = false;
    if (stat(unit, &st) != 0) {
        err_set(err, "no service installed at %s", unit);
    } else if (stop(err)) {
        if (unlink(unit) != 0) {
            err_set(err, "%s: %s", unit, strerror(errno));
        } else {
            ok = after_remove(err);
        }
    }
    free(unit);
    return ok;
}

bool service_installed(void) {
    Err err;
    char *unit = service_unit_path(&err);
    struct stat st;
    bool ok = unit && stat(unit, &st) == 0;
    free(unit);
    return ok;
}

static char *running_executable(void) {
    char buf[PATH_MAX];
#ifdef __APPLE__
    uint32_t size = sizeof buf;
    if (_NSGetExecutablePath(buf, &size) != 0) return NULL;
#else
    ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n < 0) return NULL;
    buf[n] = '\0';
#endif
    char resolved[PATH_MAX];
    return xstrdup(realpath(buf, resolved) ? resolved : buf);
}

static char *find_on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return NULL;
    char *copy = xstrdup(path), *save = NULL;
    char *found = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir && !found; dir = strtok_r(NULL, ":", &save)) {
        char *candidate = path_join(*dir ? dir : ".", name);
        if (access(candidate, X_OK) == 0) {
            found = candidate;
        } else {
            free(candidate);
        }
    }
    free(copy);
    return found;
}

char *service_executable_path(Err *err) {
    char *exe = running_executable();
    if (!exe) {
        err_set(err, "cannot find the running executable");
        return NULL;
    }
    char *on_path = find_on_path(LABEL);
    struct stat a, b;
    if (on_path && stat(on_path, &a) == 0 && stat(exe, &b) == 0 && a.st_dev == b.st_dev && a.st_ino == b.st_ino) {
        free(exe);
        return on_path;
    }
    free(on_path);
    return exe;
}
