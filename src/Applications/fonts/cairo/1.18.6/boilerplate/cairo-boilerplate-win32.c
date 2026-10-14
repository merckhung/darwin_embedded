/* -*- Mode: c; c-basic-offset: 4; indent-tabs-mode: t; tab-width: 8; -*- */
/*
 * Copyright © 2004,2006 Red Hat, Inc.
 *
 * Permission to use, copy, modify, distribute, and sell this software
 * and its documentation for any purpose is hereby granted without
 * fee, provided that the above copyright notice appear in all copies
 * and that both that copyright notice and this permission notice
 * appear in supporting documentation, and that the name of
 * Red Hat, Inc. not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. Red Hat, Inc. makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
 * RED HAT, INC. DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS
 * SOFTWARE, INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND
 * FITNESS, IN NO EVENT SHALL RED HAT, INC. BE LIABLE FOR ANY SPECIAL,
 * INDIRECT OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER
 * RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION
 * OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR
 * IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 *
 * Author: Carl D. Worth <cworth@cworth.org>
 */

#include "cairo-boilerplate-private.h"

#include <cairo-win32.h>

#include <errno.h>
#include <limits.h>
#include <assert.h>

static const cairo_user_data_key_t win32_closure_key;

typedef struct _win32_target_closure {
    HBITMAP hbitmap;     /* Bitmap */
    void *data;          /* Bitmap data */
    HDC hdc;             /* Memory DC with bitmap selected */
} win32_target_closure_t;

static void
_cairo_boilerplate_win32_cleanup_ddb_surface (void *closure)
{
    win32_target_closure_t *win32tc = closure;

    if (win32tc != NULL)
    {
        if (win32tc->hdc != NULL) {
            /* https://devblogs.microsoft.com/oldnewthing/20100416-00/?p=14313 */
            SelectObject (win32tc->hdc, CreateBitmap (0, 0, 1, 1, NULL));

            if (!DeleteDC (win32tc->hdc)) {
                fprintf (stderr, "%s failed during cleanup\n", "DeleteDC");
            }
        }

        if (win32tc->hbitmap) {
            if (!DeleteObject (win32tc->hbitmap)) {
                fprintf (stderr, "%s failed during cleanup\n", "DeleteObject");
            }
        }

        free (win32tc);
    }
}

static cairo_surface_t *
_cairo_boilerplate_win32_create_ddb_surface (const char                *name,
                                             cairo_content_t            content,
                                             double                     width,
                                             double                     height,
                                             double                     max_width,
                                             double                     max_height,
                                             cairo_boilerplate_mode_t   mode,
                                             void                     **closure)
{
    win32_target_closure_t *win32tc = NULL;
    cairo_format_t format;
    cairo_surface_t *surface;
    cairo_status_t status;
    WORD pixel_bits;
    struct {
        BITMAPINFOHEADER header;
        RGBQUAD color_table[256];
    } bitmap_desc = {0};
    HBITMAP hbitmap_old;

    if (width < 1.0)
        width = 1.0;
    if (height < 1.0)
        height = 1.0;

    win32tc = calloc (1, sizeof (win32_target_closure_t));
    if (win32tc == NULL) {
        fprintf (stderr, "%s failed with errno %d\n", "calloc", errno);
        *closure = NULL;
        return NULL;
    }

    memset (win32tc, 0, sizeof (*win32tc));

    *closure = win32tc;

    switch (content) {
        case CAIRO_CONTENT_COLOR:
            format = CAIRO_FORMAT_RGB24;
            pixel_bits = 32;
            break;
        case CAIRO_CONTENT_COLOR_ALPHA:
            format = CAIRO_FORMAT_ARGB32;
            pixel_bits = 32;
            break;
        case CAIRO_CONTENT_ALPHA:
            format = CAIRO_FORMAT_A8;
            pixel_bits = 8;
            break;
        default:
            assert (0); /* not reached */
            format = CAIRO_FORMAT_INVALID;
            break;
    }

    if (pixel_bits <= 8) {
        assert (pixel_bits == 8);

        for (BYTE i = 0; i <= 255; i++) {
            bitmap_desc.color_table[i].rgbBlue = i;
            bitmap_desc.color_table[i].rgbGreen = i;
            bitmap_desc.color_table[i].rgbRed = i;
            bitmap_desc.color_table[i].rgbReserved = 0;
        }
    }

    assert (width < (double)LONG_MAX);
    assert (height < (double)LONG_MAX);

    bitmap_desc.header.biSize = sizeof (bitmap_desc.header);
    bitmap_desc.header.biWidth = (LONG)width;
    bitmap_desc.header.biHeight = -(LONG)height; /* a negative height tells GDI to
                                                    use a top-down coordinate system */
    bitmap_desc.header.biPlanes = 1;
    bitmap_desc.header.biBitCount = pixel_bits;
    bitmap_desc.header.biCompression = BI_RGB;

    /* From https://learn.microsoft.com/en-us/windows/win32/api/wingdi/nf-wingdi-createdibsection:
     * CreateDIBSection does not use the parameters biXPelsPerMeter or biYPelsPerMeter */

    win32tc->hbitmap = CreateDIBSection (NULL, (BITMAPINFO*)&bitmap_desc, DIB_RGB_COLORS, &win32tc->data, NULL, 0);
    if (win32tc->hbitmap == NULL) {
        fprintf (stderr, "%s failed with error code %u\n", "CreateDIBSection", (unsigned int) GetLastError ());
        return NULL;
    }

    win32tc->hdc = CreateCompatibleDC (NULL);
    if (win32tc->hdc == NULL) {
        fprintf (stderr, "%s failed\n", "CreateCompatibleDC");
        return NULL;
    }

    hbitmap_old = SelectObject (win32tc->hdc, win32tc->hbitmap);
    assert (hbitmap_old == CreateBitmap (0, 0, 1, 1, NULL));

    surface = cairo_win32_surface_create_with_format (win32tc->hdc, format);

    status = cairo_surface_status (surface);

    if (status != CAIRO_STATUS_SUCCESS) {
	fprintf (stderr,
		 "Failed to create the test surface: %s [%d].\n",
		 cairo_status_to_string (status), status);
        cairo_surface_destroy (surface);
        _cairo_boilerplate_win32_cleanup_ddb_surface (win32tc);
	return NULL;
    }

    status = cairo_surface_set_user_data (surface, &win32_closure_key, win32tc, NULL);

    if (status != CAIRO_STATUS_SUCCESS) {
	fprintf (stderr,
		 "Failed to set surface userdata: %s [%d].\n",
		 cairo_status_to_string (status), status);
        cairo_surface_destroy (surface);
	_cairo_boilerplate_win32_cleanup_ddb_surface (win32tc);
	return NULL;
    }

    return surface;
}

static cairo_surface_t *
_cairo_boilerplate_win32_create_dib_surface (const char		       *name,
					     cairo_content_t		content,
					     double 			width,
					     double 			height,
					     double 			max_width,
					     double 			max_height,
					     cairo_boilerplate_mode_t   mode,
					     void		      **closure)
{
    cairo_format_t format;

    format = cairo_boilerplate_format_from_content (content);

    *closure = NULL;

    return cairo_win32_surface_create_with_dib (format, width, height);
}

static const cairo_boilerplate_target_t targets[] = {
    {
	"win32-DIB", "win32", NULL, NULL,
	CAIRO_SURFACE_TYPE_WIN32, CAIRO_CONTENT_COLOR, 0,
	"cairo_win32_surface_create_with_dib",
	_cairo_boilerplate_win32_create_dib_surface,
	cairo_surface_create_similar,
	NULL,
	NULL,
	_cairo_boilerplate_get_image_surface,
	cairo_surface_write_to_png,
	NULL,
	NULL,
	NULL,
	TRUE, FALSE, FALSE
    },
    /* Testing the win32 surface for ARGB32 DIBs isn't interesting,
     * since it just chains to the image backend
     */
    {
        "win32-DIB", "win32", NULL, NULL,
	CAIRO_SURFACE_TYPE_WIN32, CAIRO_CONTENT_COLOR_ALPHA, 0,
	"cairo_win32_surface_create_with_dib",
	_cairo_boilerplate_win32_create_dib_surface,
	cairo_surface_create_similar,
	NULL,
	NULL,
	_cairo_boilerplate_get_image_surface,
	cairo_surface_write_to_png,
	NULL,
	NULL,
	NULL,
	FALSE, FALSE, FALSE
    },
    {
	"win32-DDB", "win32", NULL, NULL,
	CAIRO_SURFACE_TYPE_WIN32, CAIRO_CONTENT_COLOR, 1,
	"cairo_win32_surface_create",
	_cairo_boilerplate_win32_create_ddb_surface,
	cairo_surface_create_similar,
	NULL,
	NULL,
	_cairo_boilerplate_get_image_surface,
	cairo_surface_write_to_png,
	_cairo_boilerplate_win32_cleanup_ddb_surface,
	NULL,
	NULL,
	FALSE, FALSE, FALSE
    },
    {
	"win32-DDB", "win32", NULL, NULL,
	CAIRO_SURFACE_TYPE_WIN32, CAIRO_CONTENT_COLOR_ALPHA, 1,
	"cairo_win32_surface_create_with_format",
	_cairo_boilerplate_win32_create_ddb_surface,
	cairo_surface_create_similar,
	NULL,
	NULL,
	_cairo_boilerplate_get_image_surface,
	cairo_surface_write_to_png,
	_cairo_boilerplate_win32_cleanup_ddb_surface,
	NULL,
	NULL,
	FALSE, FALSE, FALSE
    },
};
CAIRO_BOILERPLATE (win32, targets)
