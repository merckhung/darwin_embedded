#pragma once
#include <corecrypto/ccdigest.h>

extern const struct ccdigest_info embedded_md5_di, embedded_sha1_di;
void embedded_legacy_digest_selftest(void);
