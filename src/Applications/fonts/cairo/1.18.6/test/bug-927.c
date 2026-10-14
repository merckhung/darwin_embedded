/*
 * Copyright © 2026 Uli Schlachter
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

static cairo_test_status_t
draw (cairo_t *cr, int width, int height)
{
    double scale = 3;
    cairo_matrix_t matrix = {
	1 / scale, 0,
	0, scale,
	0, -100
    };

    cairo_set_line_width (cr, 20);
    cairo_set_line_join (cr, CAIRO_LINE_JOIN_ROUND);

    cairo_set_source_rgb (cr, 0, 0, 0);
    cairo_paint (cr);

    cairo_set_source_rgb (cr, 1, 1, 1);
    cairo_set_matrix (cr, &matrix);
    cairo_move_to (cr, -50, -50);
    cairo_line_to (cr, 50, 50);
    cairo_line_to (cr, 150, -50);
    cairo_stroke (cr);

    return CAIRO_TEST_SUCCESS;
}

CAIRO_TEST (bug_927,
	    "Bug 927 (_cairo_matrix_has_unity_scale incorrectly returning true)",
	    "matrix", /* keywords */
	    NULL, /* requirements */
	    30, 100,
	    NULL, draw)
