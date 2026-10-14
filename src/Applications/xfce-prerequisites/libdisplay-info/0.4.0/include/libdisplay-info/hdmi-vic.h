#ifndef DI_HDMI_VIC_H
#define DI_HDMI_VIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Low-level API for HDMI Video Identification Codes (HDMI VIC).
 */

/**
 * A HDMI Video Identification Code (VIC).
 */
struct di_hdmi_vic {
	/* Video Identification Code (VIC) */
	uint8_t code;
};

/**
 * A HDMI video format, not to be confused with a CTA-861 video format.
 */
struct di_hdmi_vic_video_format {
	/* Video Identification Code (VIC) */
	struct di_hdmi_vic vic;
	/* Horizontal/vertical active pixels/lines */
	int32_t h_active, v_active;
	/* Horizontal/vertical front porch */
	int32_t h_front, v_front;
	/* Horizontal/vertical sync pulse */
	int32_t h_sync, v_sync;
	/* Horizontal/vertical back porch */
	int32_t h_back, v_back;
	/* Pixel clock in Hz */
	int64_t pixel_clock_hz;
};

/**
 * Get a HDMI video format from a HDMI VIC.
 *
 * Returns NULL if the HDMI VIC is unknown.
 */
const struct di_hdmi_vic_video_format *
di_hdmi_vic_video_format_from_vic(struct di_hdmi_vic vic);

/**
 * Get a HDMI VIC from a HDMI video format.
 *
 * Returns 0 if the video format is unknown.
 */
struct di_hdmi_vic
di_hdmi_vic_video_format_to_vic(const struct di_hdmi_vic_video_format *format);

#endif
