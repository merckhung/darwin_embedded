#ifndef DI_DMT_H
#define DI_DMT_H

/**
 * libdisplay-info's low-level API for VESA Display Monitor Timing (DMT).
 *
 * The library implements VESA DMT version 1.0 revision 13.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * A DMT Timing Code.
 */
struct di_dmt_code {
	/* Timing Code */
	uint8_t code;
};

/**
 * A DMT timing.
 */
struct di_dmt_timing {
	/* DMT Timing Code */
	struct di_dmt_code dmt_code;
	/* EDID standard timing 2-byte code, zero if unset */
	uint16_t edid_std_id;
	/* CVT 3-byte code, zero if unset */
	uint32_t cvt_id;
	/* Addressable pixels */
	int32_t horiz_video, vert_video;
	/* Field Refresh Rate in Hz */
	float refresh_rate_hz;
	/* Pixel clock in Hz */
	int32_t pixel_clock_hz;
	/* Horizontal/Vertical Blanking in pixels/lines */
	int32_t horiz_blank, vert_blank;
	/* Horizontal/Vertical Front Porch in pixels/lines */
	int32_t horiz_front_porch, vert_front_porch;
	/* Horizontal/Vertical Sync Pulse Width in pixels/lines */
	int32_t horiz_sync_pulse, vert_sync_pulse;
	/* Horizontal/Vertical Border in pixels/lines */
	int32_t horiz_border, vert_border;
	/* Whether the timing has reduced blanking */
	bool reduced_blanking;
};

/**
 * Get a VESA Display Monitor Timing (DMT) from a DMT Timing Code.
 *
 * Returns NULL if the DMT Timing Code is unknown.
 */
const struct di_dmt_timing *
di_dmt_timing_from_code(struct di_dmt_code dmt_code);

/**
 * Get a DMT Timing Code from a VESA Display Monitor Timing (DMT).
 *
 * Returns a code of 0 if the DMT Timing Code is unknown.
 */
struct di_dmt_code
di_dmt_timing_to_code(const struct di_dmt_timing *timing);

/* See <libdisplay-info/edid.h> */
struct di_edid_standard_timing;

/**
 * Get a VESA Display Monitor Timing (DMT), if any.
 *
 * NULL is returned if the standard timing doesn't have a DMT.
 */
const struct di_dmt_timing *
di_dmt_timing_from_edid_standard_timing(const struct di_edid_standard_timing *t);

#endif
