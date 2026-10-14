/* Native Darwin configuration for the pinned OpenBSD asr sources.
 * No guest API is emulated here. Optional OpenBSD-only extensions are absent. */
#ifndef NATIVE_RESOLVER_CONFIG_H
#define NATIVE_RESOLVER_CONFIG_H
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "time_math.h"
#define SA_LEN(x) ((x)->sa_len)
#define SS_LEN(x) ((x)->ss_len)
#include <libkern/OSByteOrder.h>
#include <machine/endian.h>
#define _BYTE_ORDER BYTE_ORDER
#define _LITTLE_ENDIAN LITTLE_ENDIAN
#define _BIG_ENDIAN BIG_ENDIAN
#define htobe16(x) OSSwapHostToBigInt16(x)
#define be16toh(x) OSSwapBigToHostInt16(x)
#define htobe32(x) OSSwapHostToBigInt32(x)
#define be32toh(x) OSSwapBigToHostInt32(x)
#define HAVE_STRUCT_SOCKADDR_IN_SIN_LEN 1
#define HAVE_STRUCT_SOCKADDR_IN6_SIN6_LEN 1
#define HAVE_STRUCT_SOCKADDR_SA_LEN 1
#define HAVE_ISSETUGID 1
#define HAVE_GETSERVBYNAME_R_4_ARGS 1
#define HAVE_GETSERVBYPORT_R_4_ARGS 1
#define HAVE_ENDSERVENT_R 1
#define HAVE_STRUCT_SERVENT_DATA 1
/* Darwin has AI_CANONNAME, but no OpenBSD AI_FQDN extension. The portable
 * upstream alias makes AI_CANONNAME self-conflict; disable only that extension. */
#define AI_FQDN 0
#define DEF_WEAK(x)
#define DEF_STRONG(x)
#define _PATH_SERVICES "/etc/services"
/* Exact OpenBSD private service-parser state, copied from pinned netdb.h.
 * It is not exposed as a Darwin public struct or installed in the SDK. */
struct servent_data {
    void *fp;
    char **aliases;
    int maxaliases;
    int stayopen;
    char *line;
};
#define setservent_r native_resolver_setservent_r
#define endservent_r native_resolver_endservent_r
#define getservent_r native_resolver_getservent_r
#define getservbyname_r native_resolver_getservbyname_r
#define getservbyport_r native_resolver_getservbyport_r
void setservent_r(int, struct servent_data *);
void endservent_r(struct servent_data *);
int getservent_r(struct servent *, struct servent_data *);
int getservbyname_r(const char *, const char *, struct servent *, struct servent_data *);
int getservbyport_r(int, const char *, struct servent *, struct servent_data *);
long long strtonum(const char *, long long, long long, const char **);
void *reallocarray(void *, size_t, size_t);
void *recallocarray(void *, size_t, size_t, size_t);
void explicit_bzero(void *, size_t);
#endif
