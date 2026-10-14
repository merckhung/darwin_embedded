/*
 * Copyright (c) 2023,2026 Brian Tarricone <brian@tarricone.org>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301 USA
 */

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include "window-icon-utils.h"

// gdk-pixbuf's BMP writer does not write with the header type that supports
// an alpha channel, so we have to do it ourselves here.
static guchar *
window_icon_argb_to_bmp(const guint32 *image_data,
                        gint width,
                        gint height,
                        gboolean is_premultiplied,
                        gsize *bmp_len) {
    guint image_data_len;
    guchar *data;
    const guint32 header_bytes = 108;
    const guint32 pixel_data_start = 14 + header_bytes;
    guint32 data_size;
    guchar *cp;
    const guint32 *lp;

    g_return_val_if_fail(image_data != NULL, NULL);
    g_return_val_if_fail(width > 0 && height > 0, NULL);
    g_return_val_if_fail(bmp_len != NULL, NULL);

    image_data_len = width * 4 * height;
    data_size = pixel_data_start + image_data_len;

#define PACK_U16(off, val) \
    G_STMT_START { \
        data[(off)] = (val) & 0xff; \
        data[(off) + 1] = ((val) >> 8) & 0xff; \
    } \
    G_STMT_END
#define PACK_U32(off, val) \
    G_STMT_START { \
        data[(off)] = (val) & 0xff; \
        data[(off) + 1] = ((val) >> 8) & 0xff; \
        data[(off) + 2] = ((val) >> 16) & 0xff; \
        data[(off) + 3] = ((val) >> 24) & 0xff; \
    } \
    G_STMT_END

    data = g_malloc(data_size);
    memset(data, 0, pixel_data_start);
    // BMP header
    data[0] = 'B';
    data[1] = 'M';
    PACK_U32(2, data_size);
    PACK_U32(10, pixel_data_start);
    // DIB header (BITMAPV4HEADER)
    PACK_U32(14, header_bytes);
    PACK_U32(18, width);
    PACK_U32(22, -height);  // negative for top-to-bottom data
    PACK_U16(26, 1);  // number of color planes
    PACK_U16(28, 32);  // bpp
    PACK_U16(30, 3);  // BI_BITFIELDS
    PACK_U32(34, data_size);
    PACK_U32(54, 0x000000ff);  // red mask
    PACK_U32(58, 0x0000ff00);  // green mask
    PACK_U32(62, 0x00ff0000);  // blue mask
    PACK_U32(66, 0xff000000);  // alpha mask
    // image data
    for (cp = data + pixel_data_start, lp = image_data; cp < data + data_size; cp += 4, ++lp) {
        guint argb = *lp;

        gulong a = (argb >> 24) & 0xff;
        gulong r = (argb >> 16) & 0xff;
        gulong g = (argb >> 8) & 0xff;
        gulong b = argb & 0xff;

        if (is_premultiplied && a != 255) {
            if (a == 0) {
                cp[0] = 0;
                cp[1] = 0;
                cp[2] = 0;
                cp[3] = 0;
            } else {
                cp[0] = MIN((r * 255 + a / 2) / a, 255);
                cp[1] = MIN((g * 255 + a / 2) / a, 255);
                cp[2] = MIN((b * 255 + a / 2) / a, 255);
                cp[3] = a;
            }
        } else {
            cp[0] = r;
            cp[1] = g;
            cp[2] = b;
            cp[3] = a;
        }
    }

    *bmp_len = data_size;
    return data;

#undef PACK_U16
#undef PACK_U32
}

WindowIcon *
_window_icon_new(const guint32 *raw_argb32, gint width, gint height, gboolean is_premultiplied) {
    gsize bmp_len = 0;
    guchar *bmp_data = window_icon_argb_to_bmp(raw_argb32, width, height, is_premultiplied, &bmp_len);

    if (bmp_data != NULL) {
        WindowIcon *window_icon = g_new0(WindowIcon, 1);
        window_icon->width = width;
        window_icon->height = height;
        window_icon->bmp = bmp_data;
        window_icon->bmp_len = bmp_len;
        return window_icon;
    } else {
        return NULL;
    }
}

gint
_window_icon_compare(gconstpointer a,
                     gconstpointer b) {
    const WindowIcon *wa = a;
    const WindowIcon *wb = b;

    if (wa == NULL && wb == NULL) {
        return 0;
    } else if (wa == NULL) {
        return -1;
    } else if (wb == NULL) {
        return 1;
    } else {
        return MAX(wa->width, wa->height) - MAX(wb->width, wb->height);
    }
}

void
_window_icon_free(WindowIcon *window_icon) {
    g_free(window_icon->bmp);
    g_free(window_icon);
}
