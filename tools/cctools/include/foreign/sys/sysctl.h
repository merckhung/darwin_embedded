#ifndef _SYS_SYSCTL_H_
#define _SYS_SYSCTL_H_

#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CTL_KERN 1
#define KERN_OSTYPE 1
#define KERN_OSRELEASE 2
#define CTL_HW 6
#define HW_NCPU 3

int sysctl(int *, u_int, void *, size_t *, void *, size_t);
int sysctlbyname(const char *, void *, size_t *, void *, size_t);
int sysctlnametomib(const char *, int *, size_t *);

#ifdef __cplusplus
}
#endif

#endif
