// Checks the YUV to RGB conversion the video previews use.
//
// The expected values here come from the standard definitions -- SMPTE 75%
// colour bars and the BT.601/BT.709/JPEG matrices -- not from running this
// implementation and recording what it produced. A test written the other way
// round would have happily confirmed a channel swap.
//
// On the device this was ported from, previews spent several hardware rounds
// with badly shifted colour while greyscale clips stayed correct, which is the
// signature of the chroma channels being exchanged. The swap case below is that
// symptom turned into an assertion, so a swapped implementation cannot pass.

#include <stdio.h>
#include <stdlib.h>

#include "yuv2rgb.h"

static int failures;

static void expect(const char *what, int mode, int y, int u, int v,
	int want_r, int want_g, int want_b, int tolerance)
{
	struct rg_yuv_coefficients c = rg_yuv_for(mode);
	uint32_t argb = rg_yuv_pixel_to_argb(&c, y, u, v);
	int r = (argb >> 16) & 0xff;
	int g = (argb >> 8) & 0xff;
	int b = argb & 0xff;
	int alpha = (argb >> 24) & 0xff;

	if (alpha != 0xff) {
		printf("FAIL %-34s alpha is %d, expected 255\n", what, alpha);
		failures++;
		return;
	}
	if (abs(r - want_r) > tolerance || abs(g - want_g) > tolerance ||
		abs(b - want_b) > tolerance) {
		printf("FAIL %-34s got (%3d,%3d,%3d) want (%3d,%3d,%3d) +/-%d\n",
			what, r, g, b, want_r, want_g, want_b, tolerance);
		failures++;
		return;
	}
	printf("ok   %-34s (%3d,%3d,%3d)\n", what, r, g, b);
}

int main(void)
{
	// Limited range: the signal lives in 16..235, so those are black and white.
	expect("bt601 black", RG_YUV_BT601, 16, 128, 128, 0, 0, 0, 1);
	expect("bt601 white", RG_YUV_BT601, 235, 128, 128, 255, 255, 255, 1);
	expect("bt601 mid grey", RG_YUV_BT601, 126, 128, 128, 128, 128, 128, 2);

	// SMPTE 75% colour bars, BT.601. Each primary should come back at about
	// 75% of full scale (191) with the other two at zero. These Y'CbCr triples
	// are the standard bar values.
	expect("bt601 75% red", RG_YUV_BT601, 65, 100, 212, 191, 0, 0, 3);
	expect("bt601 75% green", RG_YUV_BT601, 112, 72, 58, 0, 191, 0, 3);
	expect("bt601 75% blue", RG_YUV_BT601, 35, 212, 114, 0, 0, 191, 3);

	// Full range: black and white are the ends of the byte, and a saturated
	// red reaches full scale rather than 75%.
	expect("full range black", RG_YUV_FULL, 0, 128, 128, 0, 0, 0, 1);
	expect("full range white", RG_YUV_FULL, 255, 128, 128, 255, 255, 255, 1);
	expect("full range red", RG_YUV_FULL, 76, 85, 255, 254, 0, 0, 3);

	// A matrix only ever touches chroma, so every matrix agrees on grey. This
	// is why a wrong matrix cannot explain colour shifting while greyscale
	// clips stay correct -- and why the search had to look elsewhere.
	{
		struct rg_yuv_coefficients bt601 = rg_yuv_for(RG_YUV_BT601);
		struct rg_yuv_coefficients bt709 = rg_yuv_for(RG_YUV_BT709);
		int differed = 0;
		for (int y = 16; y <= 235; y += 8) {
			if (rg_yuv_pixel_to_argb(&bt601, y, 128, 128) !=
				rg_yuv_pixel_to_argb(&bt709, y, 128, 128)) {
				differed = 1;
			}
		}
		if (differed) {
			printf("FAIL %-34s matrices disagree on neutral chroma\n",
				"grey is matrix independent");
			failures++;
		} else {
			printf("ok   %-34s\n", "grey is matrix independent");
		}
	}

	// ...but they must differ on saturated colour, or the mode is being
	// ignored and the plumbing that carries it is decorative.
	{
		struct rg_yuv_coefficients bt601 = rg_yuv_for(RG_YUV_BT601);
		struct rg_yuv_coefficients bt709 = rg_yuv_for(RG_YUV_BT709);
		if (rg_yuv_pixel_to_argb(&bt601, 112, 72, 58) ==
			rg_yuv_pixel_to_argb(&bt709, 112, 72, 58)) {
			printf("FAIL %-34s BT.601 and BT.709 give identical colour\n",
				"matrices differ on colour");
			failures++;
		} else {
			printf("ok   %-34s\n", "matrices differ on colour");
		}
	}

	// The bug this whole path exists to avoid. Feeding U where V belongs must
	// visibly change the result; if it does not, the two are being conflated
	// somewhere and a swapped implementation would pass everything above.
	{
		struct rg_yuv_coefficients c = rg_yuv_for(RG_YUV_BT601);
		uint32_t upright = rg_yuv_pixel_to_argb(&c, 65, 100, 212);
		uint32_t swapped = rg_yuv_pixel_to_argb(&c, 65, 212, 100);
		int r1 = (upright >> 16) & 0xff, b1 = upright & 0xff;
		int r2 = (swapped >> 16) & 0xff, b2 = swapped & 0xff;
		if (abs(r1 - r2) < 64 || abs(b1 - b2) < 64) {
			printf("FAIL %-34s swapping U and V barely changed the pixel\n",
				"chroma channels are distinct");
			failures++;
		} else {
			printf("ok   %-34s red %d->%d blue %d->%d when swapped\n",
				"chroma channels are distinct", r1, r2, b1, b2);
		}
	}

	// Whole-frame path: a 2x2 block must fill all four pixels from the single
	// shared chroma sample, and must respect the destination pitch.
	{
		uint8_t y[4] = { 235, 235, 16, 16 };
		uint8_t u[1] = { 128 };
		uint8_t v[1] = { 128 };
		uint32_t out[8];
		for (int i = 0; i < 8; i++) out[i] = 0xDEADBEEF;
		rg_yuv420_to_argb(y, 2, u, 1, v, 1, 2, 2, RG_YUV_BT601, out,
			4 * sizeof(uint32_t));
		int ok = out[0] == 0xFFFFFFFFu && out[1] == 0xFFFFFFFFu &&
			out[4] == 0xFF000000u && out[5] == 0xFF000000u &&
			out[2] == 0xDEADBEEF && out[3] == 0xDEADBEEF;
		if (!ok) {
			printf("FAIL %-34s 2x2 block or pitch handling is wrong\n",
				"frame conversion");
			failures++;
		} else {
			printf("ok   %-34s\n", "frame conversion");
		}
	}

	// The same bug one level up. The 2x2 case above uses neutral chroma, so
	// exchanging the U and V *planes* in the frame loop leaves its output
	// identical and it passes -- while every colour clip on the device comes
	// out with red and blue swapped. Saturated chroma with the two planes
	// carrying different values is what makes a transposition visible, and
	// comparing against the per-pixel function ties the two together so the
	// loop cannot quietly disagree with the arithmetic already checked above.
	{
		uint8_t y[4] = { 65, 65, 65, 65 };
		uint8_t u[1] = { 100 };
		uint8_t v[1] = { 212 };   // BT.601 75% red: U and V differ sharply
		uint32_t out[4];
		rg_yuv420_to_argb(y, 2, u, 1, v, 1, 2, 2, RG_YUV_BT601, out,
			2 * sizeof(uint32_t));

		struct rg_yuv_coefficients c = rg_yuv_for(RG_YUV_BT601);
		uint32_t want = rg_yuv_pixel_to_argb(&c, 65, 100, 212);
		uint32_t swapped = rg_yuv_pixel_to_argb(&c, 65, 212, 100);

		if (want == swapped) {
			printf("FAIL %-34s the control is degenerate\n",
				"frame loop keeps U and V apart");
			failures++;
		} else if (out[0] != want || out[1] != want ||
			out[2] != want || out[3] != want) {
			printf("FAIL %-34s got %08X want %08X (swapped would be %08X)\n",
				"frame loop keeps U and V apart", out[0], want, swapped);
			failures++;
		} else {
			printf("ok   %-34s %08X, not the swapped %08X\n",
				"frame loop keeps U and V apart", out[0], swapped);
		}
	}

	if (failures) {
		printf("\n%d YUV conversion test(s) failed.\n", failures);
		return 1;
	}
	printf("\nYUV conversion tests passed.\n");
	return 0;
}
