#ifndef CONSOLE_H
#define CONSOLE_H

#include "common.h"

VOID log_info(CONST CHAR16 *fmt, ...);
VOID log_error(CONST CHAR16 *fmt, ...);
VOID log_screen_quiet(VOID);

#endif