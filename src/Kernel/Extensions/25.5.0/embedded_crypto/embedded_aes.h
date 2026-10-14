#pragma once
#include <corecrypto/ccmode.h>

extern const struct ccmode_cbc embedded_aes_cbc_encrypt;
extern const struct ccmode_cbc embedded_aes_cbc_decrypt;
void embedded_aes_selftest(void);
