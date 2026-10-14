#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <libdisplay-info/dmt.h>
#include <libdisplay-info/cta-vic.h>
#include <libdisplay-info/hdmi-vic.h>

#include "di-edid-decode.h"

static const char *
video_format_picture_aspect_ratio_name(enum di_cta_vic_video_format_picture_aspect_ratio ar)
{
	switch (ar) {
	case DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_4_3:
		return "  4:3  ";
	case DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_16_9:
		return " 16:9  ";
	case DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_64_27:
		return " 64:27 ";
	case DI_CTA_VIC_VIDEO_FORMAT_PICTURE_ASPECT_RATIO_256_135:
		return "256:135";
	}
	abort(); /* unreachable */
}

void
print_cta_vic_timing(struct di_cta_vic vic)
{
	const struct di_cta_vic_video_format *fmt;
	int32_t h_blank, v_blank, v_active;
	double refresh, h_freq_hz, pixel_clock_mhz, h_total, v_total;
	char buf[10];

	printf("    VIC %3" PRIu8, vic.code);

	fmt = di_cta_vic_video_format_from_vic(vic);
	if (fmt == NULL)
		return;

	v_active = fmt->v_active;
	if (fmt->interlaced)
		v_active /= 2;

	h_blank = fmt->h_front + fmt->h_sync + fmt->h_back;
	v_blank = fmt->v_front + fmt->v_sync + fmt->v_back;
	h_total = fmt->h_active + h_blank;

	v_total = v_active + v_blank;
	if (fmt->interlaced)
		v_total += 0.5;

	refresh = (double) fmt->pixel_clock_hz / (h_total * v_total);
	h_freq_hz = (double) fmt->pixel_clock_hz / h_total;
	pixel_clock_mhz = (double) fmt->pixel_clock_hz / (1000 * 1000);

	snprintf(buf, sizeof(buf), "%d%s",
		 fmt->v_active,
		 fmt->interlaced ? "i" : "");

	printf(":");
	printf(" %5dx%-5s", fmt->h_active, buf);
	printf(" %10.6f Hz", refresh);
	printf(" %s", video_format_picture_aspect_ratio_name(fmt->picture_aspect_ratio));
	printf(" %8.3f kHz %13.6f MHz", h_freq_hz / 1000, pixel_clock_mhz);
}


void
print_hdmi_vic_timing(const struct di_hdmi_vic vic)
{
	const struct di_hdmi_vic_video_format *fmt;
	int32_t h_blank, v_blank;
	double refresh, h_freq_hz, pixel_clock_mhz, h_total, v_total;
	int horiz_ratio, vert_ratio;

	printf("    HDMI VIC %" PRIu8, vic.code);

	fmt = di_hdmi_vic_video_format_from_vic(vic);
	if (fmt == NULL)
		return;

	compute_aspect_ratio(fmt->h_active, fmt->v_active, &horiz_ratio, &vert_ratio);

	h_blank = fmt->h_front + fmt->h_sync + fmt->h_back;
	v_blank = fmt->v_front + fmt->v_sync + fmt->v_back;
	h_total = fmt->h_active + h_blank;

	v_total = fmt->v_active + v_blank;

	refresh = (double) fmt->pixel_clock_hz / (h_total * v_total);
	h_freq_hz = (double) fmt->pixel_clock_hz / h_total;
	pixel_clock_mhz = (double) fmt->pixel_clock_hz / (1000 * 1000);

	printf(":");
	printf(" %5dx%-5d", fmt->h_active, fmt->v_active);
	printf(" %10.6f Hz", refresh);
	/* Not part of the spec, but edid-decode prints the aspect ratio. */
	printf(" %3u:%-3u", horiz_ratio, vert_ratio);
	printf(" %8.3f kHz %13.6f MHz", h_freq_hz / 1000, pixel_clock_mhz);
}

void
print_dmt_timing_code(struct di_dmt_code dmt_code)
{
	const struct di_dmt_timing *t;
	int hbl, vbl, horiz_total, vert_total, horiz_ratio, vert_ratio;
	double refresh, horiz_freq_hz, pixel_clock_mhz;

	printf("      ");

	t = di_dmt_timing_from_code(dmt_code);
	if (!t) {
		printf("DMT 0x%02x: Unknown\n", dmt_code.code);
		return;
	}

	compute_aspect_ratio(t->horiz_video, t->vert_video,
			     &horiz_ratio, &vert_ratio);

	hbl = t->horiz_blank - 2 * t->horiz_border;
	vbl = t->vert_blank - 2 * t->vert_border;
	horiz_total = t->horiz_video + hbl;
	vert_total = t->vert_video + vbl;
	refresh = (double) t->pixel_clock_hz / (horiz_total * vert_total);
	horiz_freq_hz = (double) t->pixel_clock_hz / horiz_total;
	pixel_clock_mhz = (double) t->pixel_clock_hz / (1000 * 1000);

	printf("DMT 0x%02x:", t->dmt_code.code);
	printf(" %5dx%-5d", t->horiz_video, t->vert_video);
	printf(" %10.6f Hz", refresh);
	printf(" %3u:%-3u", horiz_ratio, vert_ratio);
	printf(" %8.3f kHz %13.6f MHz", horiz_freq_hz / 1000, pixel_clock_mhz);
	if (t->reduced_blanking)
		printf(" (RB)");

	printf("\n");
}

void
print_cvt_timing(struct di_cvt_timing *t, struct di_cvt_options *options,
		 int hratio, int vratio, bool preferred, bool rb)
{
	double hbl, htotal;

	hbl = t->h_front_porch + t->h_sync + t->h_back_porch;
	htotal = t->total_active_pixels + hbl;

	printf("      CVT: %5dx%-5d", (int)options->h_pixels, (int)options->v_lines);
	printf(" %10.6f Hz", t->act_frame_rate);
	printf(" %3u:%-3u", hratio, vratio);
	printf(" %8.3f kHz %13.6f MHz", t->act_pixel_freq * 1000 / htotal,
	       (double) t->act_pixel_freq);

	if (preferred || rb) {
		printf(" (%s%s%s)", rb ? "RB" : "",
				    (preferred && rb) ? ", " : "",
				    preferred ? "preferred vertical rate" : "");
	}

	printf("\n");
}
