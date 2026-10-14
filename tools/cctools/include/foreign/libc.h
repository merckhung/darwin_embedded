#ifndef _LIBC_H
#define _LIBC_H

#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <stdarg.h>
#include <string.h>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/param.h>

static inline void *reallocf(void *ptr, size_t size) {
    void *new_ptr = realloc(ptr, size);
    if (!new_ptr && size > 0) {
        free(ptr);
    }
    return new_ptr;
}

#endif
