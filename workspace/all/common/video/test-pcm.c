// Checks the sample conversion the video previews use.
//
// The expected values come from the definitions of the formats -- full scale is
// 32768, unsigned 8-bit is centred on 128, planar means one buffer per channel
// -- not from running this implementation and recording what it produced.
//
// Two of the cases below exist because of specific ways this can be wrong while
// looking right:
//
//   * planar and packed indexing are the same expression when there is one
//     channel, and differ only in which buffer is read when there are two. A
//     stereo case with the channels carrying *different* values is the only
//     thing that separates them, exactly as the U/V case does in
//     test-yuv2rgb.c.
//   * +1.0 scaled by 32768 is 32768, which does not fit in an int16 and wraps
//     to -32768 -- full-scale positive becoming full-scale negative. That is a
//     loud, obvious fault in the output and a silent one in the source, so it
//     is asserted rather than assumed.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pcm.h"

static int failures;

static void expect(const char *what, int got, int want)
{
	if (got != want) {
		printf("FAIL %-40s got %d, want %d\n", what, got, want);
		failures++;
		return;
	}
	printf("ok   %-40s %d\n", what, got);
}

static void expect_near(const char *what, int got, int want, int tolerance)
{
	if (abs(got - want) > tolerance) {
		printf("FAIL %-40s got %d, want %d +/-%d\n", what, got, want,
			tolerance);
		failures++;
		return;
	}
	printf("ok   %-40s %d\n", what, got);
}

int main(void)
{
	// --- scale and clamp --------------------------------------------------
	expect("float 0.0", rg_pcm_from_float(0.0), 0);
	expect("float -1.0 is full scale negative", rg_pcm_from_float(-1.0),
		-32768);
	// The wrap case. 1.0 * 32768 is one past the top of the range.
	expect("float +1.0 clamps, does not wrap", rg_pcm_from_float(1.0), 32767);
	expect("float +1.5 clamps", rg_pcm_from_float(1.5), 32767);
	expect("float -1.5 clamps", rg_pcm_from_float(-1.5), -32768);
	expect("float 0.5", rg_pcm_from_float(0.5), 16384);
	expect("float -0.5", rg_pcm_from_float(-0.5), -16384);

	expect("clamp above range", rg_pcm_clamp_s16(40000), 32767);
	expect("clamp below range", rg_pcm_clamp_s16(-40000), -32768);
	expect("clamp inside range", rg_pcm_clamp_s16(1234), 1234);

	// --- format classification --------------------------------------------
	if (rg_pcm_planar(RG_PCM_FLTP) && rg_pcm_planar(RG_PCM_S16P) &&
		!rg_pcm_planar(RG_PCM_FLT) && !rg_pcm_planar(RG_PCM_S16)) {
		printf("ok   %-40s\n", "planar formats are recognised");
	} else {
		printf("FAIL %-40s\n", "planar formats are recognised");
		failures++;
	}

	// --- planar float, which is what the AAC decoder emits -----------------
	{
		float left[3] = { 0.0f, 0.5f, -1.0f };
		float right[3] = { 1.0f, -0.25f, 0.0f };
		const uint8_t *planes[2] = { (const uint8_t *)left,
			(const uint8_t *)right };
		int16_t out[6];
		memset(out, 0x5a, sizeof(out));

		rg_pcm_to_s16_stereo(planes, RG_PCM_FLTP, 2, 3, out);
		expect("fltp frame 0 left", out[0], 0);
		expect("fltp frame 0 right", out[1], 32767);
		expect("fltp frame 1 left", out[2], 16384);
		expect("fltp frame 1 right", out[3], -8192);
		expect("fltp frame 2 left", out[4], -32768);
		expect("fltp frame 2 right", out[5], 0);
	}

	// --- packed vs planar are genuinely different code paths ---------------
	//
	// The same six samples, laid out both ways, must produce the same output.
	// If the indexing were confused the packed case would read left[0],
	// left[1] as if they were frames 0 and 1 of the left channel.
	{
		float packed[6] = { 0.25f, -0.25f, 0.5f, -0.5f, 0.75f, -0.75f };
		float left[3] = { 0.25f, 0.5f, 0.75f };
		float right[3] = { -0.25f, -0.5f, -0.75f };
		const uint8_t *packed_planes[2] = { (const uint8_t *)packed, NULL };
		const uint8_t *planar_planes[2] = { (const uint8_t *)left,
			(const uint8_t *)right };
		int16_t a[6], b[6];

		rg_pcm_to_s16_stereo(packed_planes, RG_PCM_FLT, 2, 3, a);
		rg_pcm_to_s16_stereo(planar_planes, RG_PCM_FLTP, 2, 3, b);
		if (memcmp(a, b, sizeof(a)) != 0) {
			printf("FAIL %-40s packed %d,%d,%d,%d,%d,%d planar %d,%d,%d,%d,%d,%d\n",
				"packed and planar agree", a[0], a[1], a[2], a[3], a[4], a[5],
				b[0], b[1], b[2], b[3], b[4], b[5]);
			failures++;
		} else {
			printf("ok   %-40s\n", "packed and planar agree");
		}
		// Control: the two channels must actually differ, or the case above
		// cannot tell a channel transposition from a correct conversion.
		if (a[0] == a[1]) {
			printf("FAIL %-40s the control is degenerate\n",
				"left and right are distinct");
			failures++;
		} else {
			printf("ok   %-40s %d vs %d\n", "left and right are distinct",
				a[0], a[1]);
		}
	}

	// --- the other formats a decoder in this build can produce -------------
	{
		int16_t s16[4] = { 0, 32767, -32768, 1000 };
		const uint8_t *planes[2] = { (const uint8_t *)s16, NULL };
		int16_t out[4];
		rg_pcm_to_s16_stereo(planes, RG_PCM_S16, 2, 2, out);
		expect("s16 passes through unchanged", out[1], 32767);
		expect("s16 second frame", out[2], -32768);
	}
	{
		int32_t s32[2] = { 1 << 30, -(1 << 30) };
		const uint8_t *planes[2] = { (const uint8_t *)s32, NULL };
		int16_t out[2];
		rg_pcm_to_s16_stereo(planes, RG_PCM_S32, 2, 1, out);
		expect("s32 narrows to the top 16 bits", out[0], 16384);
		expect("s32 negative narrows", out[1], -16384);
	}
	{
		uint8_t u8[2] = { 128, 255 };
		const uint8_t *planes[2] = { u8, NULL };
		int16_t out[2];
		rg_pcm_to_s16_stereo(planes, RG_PCM_U8, 2, 1, out);
		expect("u8 midpoint is silence", out[0], 0);
		expect_near("u8 maximum is near full scale", out[1], 32512, 256);
	}
	{
		double dbl[2] = { 0.5, -0.5 };
		const uint8_t *planes[2] = { (const uint8_t *)dbl, NULL };
		int16_t out[2];
		rg_pcm_to_s16_stereo(planes, RG_PCM_DBL, 2, 1, out);
		expect("dbl left", out[0], 16384);
		expect("dbl right", out[1], -16384);
	}

	// --- mono goes to both sides ------------------------------------------
	{
		float mono[2] = { 0.5f, -0.5f };
		const uint8_t *planes[1] = { (const uint8_t *)mono };
		int16_t out[4];
		rg_pcm_to_s16_stereo(planes, RG_PCM_FLTP, 1, 2, out);
		if (out[0] == out[1] && out[2] == out[3] && out[0] == 16384 &&
			out[2] == -16384) {
			printf("ok   %-40s\n", "mono is duplicated to both sides");
		} else {
			printf("FAIL %-40s got %d,%d,%d,%d\n",
				"mono is duplicated to both sides", out[0], out[1], out[2],
				out[3]);
			failures++;
		}
	}

	// --- more channels than two -------------------------------------------
	//
	// The first two are kept and the rest dropped. Reading past the second
	// channel would be a stride error, which this catches because the third
	// channel is full scale and must not appear anywhere in the output.
	{
		float packed[6] = { 0.25f, 0.5f, 1.0f, 0.25f, 0.5f, 1.0f };
		const uint8_t *planes[1] = { (const uint8_t *)packed };
		int16_t out[4];
		rg_pcm_to_s16_stereo(planes, RG_PCM_FLT, 3, 2, out);
		if (out[0] == 8192 && out[1] == 16384 && out[2] == 8192 &&
			out[3] == 16384) {
			printf("ok   %-40s\n", "5.1-style extra channels are dropped");
		} else {
			printf("FAIL %-40s got %d,%d,%d,%d\n",
				"5.1-style extra channels are dropped", out[0], out[1], out[2],
				out[3]);
			failures++;
		}
	}

	// --- the fade ----------------------------------------------------------
	{
		int16_t buffer[8];
		for (int i = 0; i < 8; i++) buffer[i] = 1000;
		rg_pcm_ramp(buffer, 4, 0, RG_PCM_GAIN_ONE);
		expect("fade in starts at silence", buffer[0], 0);
		expect("fade in ends at full level", buffer[6], 1000);
		if (buffer[2] < buffer[4] && buffer[4] < buffer[6]) {
			printf("ok   %-40s %d %d %d %d\n", "fade in rises monotonically",
				buffer[0], buffer[2], buffer[4], buffer[6]);
		} else {
			printf("FAIL %-40s %d %d %d %d\n", "fade in rises monotonically",
				buffer[0], buffer[2], buffer[4], buffer[6]);
			failures++;
		}
		// Both channels get the same gain, or the image shifts as it fades.
		if (buffer[4] != buffer[5]) {
			printf("FAIL %-40s left %d right %d\n",
				"fade applies equally to both channels", buffer[4], buffer[5]);
			failures++;
		} else {
			printf("ok   %-40s\n", "fade applies equally to both channels");
		}
	}
	{
		int16_t buffer[8];
		for (int i = 0; i < 8; i++) buffer[i] = -1000;
		rg_pcm_ramp(buffer, 4, RG_PCM_GAIN_ONE, 0);
		expect("fade out starts at full level", buffer[0], -1000);
		// The last frame of a fade-out has to be silence. One step short of it
		// is exactly the click the fade exists to remove.
		expect("fade out ends at silence", buffer[6], 0);
	}
	{
		int16_t buffer[4] = { 1000, -1000, 1000, -1000 };
		rg_pcm_gain(buffer, 2, RG_PCM_GAIN_ONE / 2);
		expect("constant half gain", buffer[0], 500);
		expect("constant half gain, negative", buffer[1], -500);
	}
	{
		int16_t buffer[4] = { 1234, -4321, 999, -999 };
		int16_t before[4];
		memcpy(before, buffer, sizeof(buffer));
		rg_pcm_gain(buffer, 2, RG_PCM_GAIN_ONE);
		if (memcmp(before, buffer, sizeof(buffer)) != 0) {
			printf("FAIL %-40s unity gain changed the samples\n",
				"unity gain is a no-op");
			failures++;
		} else {
			printf("ok   %-40s\n", "unity gain is a no-op");
		}
	}

	// --- degenerate inputs must not read anything --------------------------
	{
		int16_t out[2] = { 0x1234, 0x5678 };
		rg_pcm_to_s16_stereo(NULL, RG_PCM_FLTP, 0, 0, out);
		rg_pcm_ramp(out, 0, 0, RG_PCM_GAIN_ONE);
		rg_pcm_gain(out, 0, 0);
		if (out[0] == 0x1234 && out[1] == 0x5678) {
			printf("ok   %-40s\n", "zero-length calls touch nothing");
		} else {
			printf("FAIL %-40s\n", "zero-length calls touch nothing");
			failures++;
		}
	}

	if (failures) {
		printf("\n%d PCM conversion test(s) failed.\n", failures);
		return 1;
	}
	printf("\nPCM conversion tests passed.\n");
	return 0;
}
