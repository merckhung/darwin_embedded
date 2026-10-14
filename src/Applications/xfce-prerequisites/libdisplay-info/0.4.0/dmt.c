#include <float.h>
#include <math.h>

#include <libdisplay-info/dmt.h>
#include <libdisplay-info/edid.h>

extern const struct di_dmt_timing _di_dmt_timings[];
extern const size_t _di_dmt_timings_len;

const struct di_dmt_timing *
di_dmt_timing_from_code(struct di_dmt_code dmt_code)
{
	size_t i;

	for (i = 0; i < _di_dmt_timings_len; i++) {
		const struct di_dmt_timing *t = &_di_dmt_timings[i];

		if (t->dmt_code.code == dmt_code.code)
			return t;
	}

	return NULL;
}

struct di_dmt_code
di_dmt_timing_to_code(const struct di_dmt_timing *timing)
{
	size_t i;

	for (i = 0; i < _di_dmt_timings_len; i++) {
		const struct di_dmt_timing *t = &_di_dmt_timings[i];

		if (t->horiz_video == timing->horiz_video &&
		    t->vert_video == timing->vert_video &&
		    fabs(t->refresh_rate_hz - timing->refresh_rate_hz) <= FLT_EPSILON &&
		    t->pixel_clock_hz == timing->pixel_clock_hz &&
		    t->horiz_blank == timing->horiz_blank &&
		    t->vert_blank == timing->vert_blank &&
		    t->horiz_front_porch == timing->horiz_front_porch &&
		    t->vert_front_porch == timing->vert_front_porch &&
		    t->horiz_sync_pulse == timing->horiz_sync_pulse &&
		    t->vert_sync_pulse == timing->vert_sync_pulse &&
		    t->horiz_border == timing->horiz_border &&
		    t->vert_border == timing->vert_border &&
		    t->reduced_blanking == timing->reduced_blanking)
			return t->dmt_code;
	}

	return (struct di_dmt_code) { .code = 0 };
}

const struct di_dmt_timing *
di_dmt_timing_from_edid_standard_timing(const struct di_edid_standard_timing *t)
{
	int32_t vert_video;
	size_t i;
	const struct di_dmt_timing *dmt;

	vert_video = di_edid_standard_timing_get_vert_video(t);

	for (i = 0; i < _di_dmt_timings_len; i++) {
		dmt = &_di_dmt_timings[i];

		if (dmt->horiz_video == t->horiz_video
		    && dmt->vert_video == vert_video
		    && dmt->refresh_rate_hz == (float)t->refresh_rate_hz
		    && dmt->edid_std_id != 0) {
			return dmt;
		}
	}

	return 0;
}
