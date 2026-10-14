/*
 * Copyright © 2026
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

/* Verify that transforming recording-pattern bounds during analysis does not
 * produce wrapped fixed-point boxes with empty extents.
 */

static cairo_test_status_t
preamble (cairo_test_context_t *ctx)
{
    cairo_rectangle_t extents = { 0, 0, 1, 1 };
    cairo_surface_t *source_surface;
    cairo_surface_t *recording_surface;
    cairo_pattern_t *pattern;
    cairo_t *cr;
    cairo_matrix_t matrix;
    cairo_status_t status;
    double x, y, width, height;

    source_surface = cairo_recording_surface_create (CAIRO_CONTENT_COLOR_ALPHA, &extents);
    cr = cairo_create (source_surface);
    cairo_rectangle (cr, 0, 0, 1, 1);
    cairo_fill (cr);
    status = cairo_status (cr);
    cairo_destroy (cr);
    if (status) {
	cairo_surface_destroy (source_surface);
	return cairo_test_status_from_status (ctx, status);
    }

    recording_surface = cairo_recording_surface_create (CAIRO_CONTENT_COLOR_ALPHA, NULL);
    cr = cairo_create (recording_surface);
    pattern = cairo_pattern_create_for_surface (source_surface);
    cairo_surface_destroy (source_surface);

    /* This invertible matrix causes transformed bbox coordinates to exceed
     * the fixed-point range and exercises clamping in
     * _cairo_matrix_transform_bounding_box_fixed().
     */
    cairo_matrix_init (&matrix, -70000.0, 50000.0, -100000.0,
		       100000.0, -100000000.0, -50000000.0);
    cairo_pattern_set_matrix (pattern, &matrix);
    status = cairo_pattern_status (pattern);
    if (!status) {
	cairo_set_source (cr, pattern);
	cairo_paint (cr);
	status = cairo_status (cr);
    }

    cairo_pattern_destroy (pattern);
    cairo_destroy (cr);
    if (status) {
	cairo_surface_destroy (recording_surface);
	return cairo_test_status_from_status (ctx, status);
    }

    cairo_recording_surface_ink_extents (recording_surface, &x, &y, &width, &height);
    status = cairo_surface_status (recording_surface);
    cairo_surface_destroy (recording_surface);
    if (status)
	return cairo_test_status_from_status (ctx, status);

    if (width <= 0 || height <= 0) {
	cairo_test_log (ctx,
			"ink extents should be non-empty, got (%g, %g, %g, %g)\n",
			x, y, width, height);
	return CAIRO_TEST_FAILURE;
    }

    return CAIRO_TEST_SUCCESS;
}

CAIRO_TEST (recording_ink_extents_overflow,
	    "Test cairo_recording_surface_ink_extents() with overflow-prone recording pattern matrix",
	    "recording,extents,transform", /* keywords */
	    NULL, /* requirements */
	    0, 0,
	    preamble, NULL)
