/*
 * Copyright © 2026 Kevin Ushey
 *
 * Permission is hereby granted, free of charge, to any person
 * obtaining a copy of this software and associated documentation
 * files (the "Software"), to deal in the Software without
 * restriction, including without limitation the rights to use, copy,
 * modify, merge, publish, distribute, sublicense, and/or sell copies
 * of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN
 * ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
 * CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "cairo-test.h"

/* Test case for:
 *
 *      https://gitlab.freedesktop.org/cairo/cairo/-/issues/976
 *
 * With ANTIALIAS_NONE, abutting rectangles built with cairo_rectangle()
 * could leave their shared pixel unpainted: the far edge was converted
 * to fixed point as fixed(x) + fixed(width), which can differ from the
 * abutting rectangle's fixed(x + width) by 1/256 and round to a
 * different pixel.
 *
 * The cell size below is chosen so that the first cell's far edge lands
 * exactly on a .5 fixed-point boundary (rounding down) while the next
 * cell's origin rounds up, which used to leave a one-pixel white cross
 * through the middle of the grid.
 */

static cairo_test_status_t
draw (cairo_t *cr, int width, int height)
{
    double x0 = 1.7;
    double size = 2.8024;
    double x1 = x0 + size;

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_paint (cr);

    cairo_set_antialias (cr, CAIRO_ANTIALIAS_NONE);
    cairo_set_source_rgb (cr, 0, 0, 0);

    /* this should draw a seamless 2x2 grid of cells */
    cairo_rectangle (cr, x0, x0, size, size);
    cairo_rectangle (cr, x1, x0, size, size);
    cairo_rectangle (cr, x0, x1, size, size);
    cairo_rectangle (cr, x1, x1, size, size);
    cairo_fill (cr);

    return CAIRO_TEST_SUCCESS;
}

CAIRO_TEST (bug_976,
	    "Bug 976 (abutting rectangles can leave their shared pixel unpainted with ANTIALIAS_NONE)",
	    "fill, antialias", /* keywords */
	    "target=raster", /* requirements */
	    10, 10,
	    NULL, draw)
