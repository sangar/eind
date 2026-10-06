#ifndef EIND_SERVICE_H
#define EIND_SERVICE_H

#include <stdbool.h>

#include "../core/util.h"

/* `eind serve` as a per-user login service: a launchd agent on macOS, a systemd user unit on Linux. */

/* service_unit_path returns the definition's path, or NULL where no service manager is known. */
char *service_unit_path(Err *err);
bool service_enable(const char *executable, Err *err);
bool service_disable(Err *err);
bool service_installed(void);
/*
 * service_executable_path is the eind found on PATH when it is this same
 * binary, because that survives upgrades that move the real file, otherwise
 * the running executable.
 */
char *service_executable_path(Err *err);

#endif
