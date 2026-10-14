/* Native ASR record queries; errors belong to the individual query, rather
 * than the process-global legacy h_errno variable. No BIND state is exposed. */
#ifndef NATIVE_RESOLVER_RECORDS_H
#define NATIVE_RESOLVER_RECORDS_H
#include <sys/types.h>
int native_resolver_query(const char *, int, int, unsigned char *, int, int *);
int native_resolver_expand(const unsigned char *, const unsigned char *,
                           const unsigned char *, char *, int);
#endif
