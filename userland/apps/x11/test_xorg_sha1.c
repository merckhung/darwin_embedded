/* Known SHA1 vectors for the actual GLib-backed Xorg glyph-cache backend.
 * This is a host algorithm check, not native guest or desktop evidence. */
#include "xsha1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int vector(const char *text, const char *expected)
{
    void *context = x_sha1_init();
    unsigned char digest[20];
    char hexadecimal[41];
    if (!context) return 1;
    size_t length = strlen(text);
    for (size_t index = 0; index < length; ++index)
        if (!x_sha1_update(context, (void *)(text + index), 1)) return 2;
    if (!x_sha1_final(context, digest)) return 3;
    for (unsigned index = 0; index < 20; ++index) sprintf(hexadecimal + index * 2, "%02x", digest[index]);
    return strcmp(hexadecimal, expected) != 0;
}

int main(void)
{
    if (vector("", "da39a3ee5e6b4b0d3255bfef95601890afd80709") ||
        vector("abc", "a9993e364706816aba3e25717850c26c9cd0d89d") ||
        vector("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", "84983e441c3bd26ebaae4aa1f95129e5e54670f1")) return 1;
    void *context = x_sha1_init();
    if (!context || x_sha1_update(context, NULL, 1) || x_sha1_update(context, "a", -1) || x_sha1_update(NULL, "a", 1)) return 2;
    char block[1000]; memset(block, 'a', sizeof(block));
    for (unsigned count = 0; count < 1000; ++count) if (!x_sha1_update(context, block, sizeof(block))) return 3;
    unsigned char digest[20]; char hexadecimal[41];
    if (!x_sha1_final(context, digest)) return 4;
    for (unsigned index = 0; index < 20; ++index) sprintf(hexadecimal + index * 2, "%02x", digest[index]);
    if (strcmp(hexadecimal, "34aa973cd4c4daa4f61eeb2bdbad27316534016f")) return 5;
    puts("Host Xorg SHA1 vectors: PASS");
    return 0;
}
