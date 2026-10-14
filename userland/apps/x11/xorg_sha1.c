/* Native Xorg glyph-cache hashing through the existing genuine GLib SHA1. */
#include <glib.h>
#include "xsha1.h"

void *x_sha1_init(void)
{
    return g_checksum_new(G_CHECKSUM_SHA1);
}

int x_sha1_update(void *context, void *data, int size)
{
    if (!context || size < 0 || (!data && size)) return 0;
    g_checksum_update(context, data, size);
    return 1;
}

int x_sha1_final(void *context, unsigned char result[20])
{
    if (!context || !result) return 0;
    gsize length = 20;
    g_checksum_get_digest(context, result, &length);
    g_checksum_free(context);
    return length == 20;
}
