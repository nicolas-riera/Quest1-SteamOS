// Copyright 2026, Quest1-SteamOS contributors
// SPDX-License-Identifier: BSL-1.0
/*!
 * @file
 * @brief  Quest 1 per-panel lens-to-panel alignment (decenter), as Meta's OS applies it. Header only, included by
 *         Monado's quest1 driver and MDP warp and by driver_quest1 (SteamVR), which each evaluate the lens model.
 *
 * Meta's compositor (libvrruntimeservice.so, CompositorVR, docs/quest1-lens.md) reads, read only,
 * /persist/calibration/display/{left,right}/{left,right}_screen_offset.json. Only when "source" is "AUTO" does it use
 * "decenter" ([x, y] in panel pixels), truncated toward zero to whole pixels; "roll" is not read. It moves the eye's
 * output viewport by that many pixels: x as read for the left eye and negated for the right eye, y as read for both
 * (GL window frame: x right, y up, the frame of the lens model's panel points). So the image is shifted by
 * (sx, sy) pixels, and with the lens model `x = (panel point + lensToScreen) / metersPerTan` this means
 * lensToScreen - shift * pixel pitch. The files are per unit data: nothing from them goes in the repository.
 */
#pragma once
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef Q1_DECENTER_DIR
#define Q1_DECENTER_DIR "/persist/calibration/display"
#endif
#define Q1_DECENTER_PIXEL_M (0.0594f / 1440.0f) //!< panel pixel pitch, 41.25 um

//! One side's file: the truncated decenter in pixels as Meta reads it (0, 0 when the file is missing, malformed or
//! its source is not AUTO, where Meta keeps zero as well).
static inline void
q1_decenter_read_file(const char *side, int out[2])
{
	out[0] = out[1] = 0;
	char path[128], buf[1024];
	snprintf(path, sizeof(path), Q1_DECENTER_DIR "/%s/%s_screen_offset.json", side, side);
	FILE *f = fopen(path, "r"); // read only, never written
	if (f == NULL) {
		return;
	}
	size_t n = fread(buf, 1, sizeof(buf) - 1, f);
	fclose(f);
	buf[n] = '\0';
	// "source": "AUTO" (the quoted key, so that "auto_decenter" or "decenter_mm" do not match)
	const char *p = strstr(buf, "\"source\"");
	if (p == NULL || (p = strchr(p + 8, ':')) == NULL) {
		return;
	}
	while (*++p == ' ') {
	}
	if (strncmp(p, "\"AUTO\"", 6) != 0) {
		return;
	}
	p = strstr(buf, "\"decenter\"");
	if (p == NULL || (p = strchr(p + 10, '[')) == NULL) {
		return;
	}
	char *end;
	double x = strtod(p + 1, &end);
	if (end == p + 1 || (end = strchr(end, ',')) == NULL) {
		return;
	}
	char *end2;
	double y = strtod(end + 1, &end2);
	if (end2 == end + 1) {
		return;
	}
	out[0] = (int)x; // truncation toward zero, as fcvtzs in Meta's reader
	out[1] = (int)y;
}

/*!
 * Meta's image shift of one eye in lens-model metres (x right, y up, the frame of the panel points): subtract it from
 * lensToScreen. view 0 is the left eye. Read once, then cached; zero without calibration.
 */
static inline void
q1_decenter_shift_m(int view, float out[2])
{
	static int shift[2][2];
	static bool read;
	if (!read) {
		int l[2], r[2];
		q1_decenter_read_file("left", l);
		q1_decenter_read_file("right", r);
		shift[0][0] = l[0];
		shift[0][1] = l[1];
		shift[1][0] = -r[0]; // Meta negates the right eye's horizontal value
		shift[1][1] = r[1];
		read = true;
	}
	out[0] = (float)shift[view != 0][0] * Q1_DECENTER_PIXEL_M;
	out[1] = (float)shift[view != 0][1] * Q1_DECENTER_PIXEL_M;
}
