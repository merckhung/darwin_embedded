#ifndef _COMMON_CRYPTO_COMMON_DIGEST_H_
#define _COMMON_CRYPTO_COMMON_DIGEST_H_

#include <openssl/md5.h>
#include <openssl/sha.h>
#include <string.h>

#define CC_MD5_DIGEST_LENGTH MD5_DIGEST_LENGTH
typedef MD5_CTX CC_MD5_CTX;
static inline int CC_MD5_Init(CC_MD5_CTX *c) { return MD5_Init(c); }
static inline int CC_MD5_Update(CC_MD5_CTX *c, const void *data, size_t len) { return MD5_Update(c, data, len); }
static inline int CC_MD5_Final(unsigned char *md, CC_MD5_CTX *c) { return MD5_Final(md, c); }
static inline unsigned char *CC_MD5(const void *data, size_t len, unsigned char *md) { return MD5((const unsigned char *)data, len, md); }

#endif
