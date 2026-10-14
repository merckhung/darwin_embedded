#ifndef _SYS_SYSLIMITS_H_
#define _SYS_SYSLIMITS_H_
#warning "Using foreign syslimits.h"

#include <linux/limits.h>

#ifndef ARG_MAX
#define ARG_MAX 131072
#endif

#ifndef CHILD_MAX
#define CHILD_MAX 40
#endif

#ifndef GID_MAX
#define GID_MAX 2147483647U
#endif

#ifndef LINK_MAX
#define LINK_MAX 32767
#endif

#ifndef MAX_CANON
#define MAX_CANON 255
#endif

#ifndef MAX_INPUT
#define MAX_INPUT 255
#endif

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

#ifndef NGROUPS_MAX
#define NGROUPS_MAX 16
#endif

#ifndef OPEN_MAX
#define OPEN_MAX 10240
#endif

#ifndef PATH_MAX
#define PATH_MAX 1024
#endif

#ifndef PIPE_BUF
#define PIPE_BUF 512
#endif

#ifndef MB_LEN_MAX
#define MB_LEN_MAX 16
#endif
#warning MB_LEN_MAX
#if MB_LEN_MAX == 16
#warning MB_LEN_MAX is 16
#else
#warning MB_LEN_MAX is NOT 16
#endif

#endif
