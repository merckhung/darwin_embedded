#ifndef _FOREIGN_STDLIB_H
#define _FOREIGN_STDLIB_H

#include_next <stdlib.h>

#ifdef __linux__
#include <errno.h>
#ifndef program_invocation_short_name
extern char *program_invocation_short_name;
#endif
static inline const char *getprogname(void) {
    return program_invocation_short_name;
}
#endif

#endif
