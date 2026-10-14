#ifndef _COPYFILE_H_
#define _COPYFILE_H_

#include <stdint.h>
#include <sys/sendfile.h>
#include <errno.h>
#include <string.h>

#define COPYFILE_DATA 1

static inline int fcopyfile(int srcfd, int dstfd, void *state, uint32_t flags) {
    (void)state;
    (void)flags;
    struct stat st;
    if (fstat(srcfd, &st) < 0) return -1;
    off_t offset = 0;
    while (offset < st.st_size) {
        ssize_t ret = sendfile(dstfd, srcfd, &offset, st.st_size - offset);
        if (ret < 0) return -1;
    }
    return 0;
}

static inline void *setmode(const char *mode_str) {
    // Dummy implementation: allocate a string and parse it roughly
    char *m = strdup(mode_str);
    return m;
}

static inline mode_t getmode(const void *set, mode_t mode) {
    if (!set) return mode;
    // Just treat it as octal
    return strtol((const char *)set, NULL, 8);
}

#endif /* _COPYFILE_H_ */
