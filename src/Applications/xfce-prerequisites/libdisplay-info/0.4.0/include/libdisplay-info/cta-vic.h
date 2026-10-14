#ifndef DI_CTA_VIC_H
#define DI_CTA_VIC_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/**
 * Low-level API for CTA Video Identification Codes (CTA VIC).
 */

/**
 * A CTA Video Identification Code (VIC).
 */
struct di_cta_vic {
	/* Video Identification Code (VIC) */
	uint8_t code;
};

/**
 * CTA video format picture aspect ratio.
 */
enum di_cta_vic_video_format_picture_aspect_ratio {
	DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_4_3, /* 4:3 */
	DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_16_9, /* 16:9 */
	DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_64_27, /* 64:27 */
	DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_256_135, /* 256:135 */
};

/**
 * CTA video format sync pulse polarity.
 */
enum di_cta_vic_video_format_sync_polarity {
	DI_CTA_VIC_VIDEO_FORMAT_SYNC_NEGATIVE, /* Negative */
	DI_CTA_VIC_VIDEO_FORMAT_SYNC_POSITIVE, /* Positive */
};

/**
 * A CTA-861 video format, defined in section 4.
 */
struct di_cta_vic_video_format {
	/* Video Identification Code (VIC) */
	struct di_cta_vic vic;
	/* Horizontal/vertical active pixels/lines */
	int32_t h_active, v_active;
	/* Horizontal/vertical front porch */
	int32_t h_front, v_front;
	/* Horizontal/vertical sync pulse */
	int32_t h_sync, v_sync;
	/* Horizontal/vertical back porch */
	int32_t h_back, v_back;
	/* Horizontal/vertical sync pulse polarity */
	enum di_cta_vic_video_format_sync_polarity h_sync_polarity, v_sync_polarity;
	/* Pixel clock in Hz */
	int64_t pixel_clock_hz;
	/* Whether this timing is interlaced */
	bool interlaced;
	/* Picture aspect ratio */
	enum di_cta_vic_video_format_picture_aspect_ratio picture_aspect_ratio;
};

/**
 * Get a CTA-861 video format from a CTA VIC.
 *
 * Returns NULL if the CTA VIC is unknown.
 */
const struct di_cta_vic_video_format *
di_cta_vic_video_format_from_vic(struct di_cta_vic vic);

/**
 * Get a CTA VIC from a CTA-861 video format.
 *
 * Returns 0 if the video format is unknown.
 */
struct di_cta_vic
di_cta_vic_video_format_to_vic(const struct di_cta_vic_video_format *format);

#endif
