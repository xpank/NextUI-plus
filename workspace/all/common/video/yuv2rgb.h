// Planar YUV 4:2:0 to ARGB8888, on the CPU.
//
// This exists as a header of static inlines rather than as part of
// generic_video.c so that the arithmetic can be compiled and checked on the
// build host, where there is no SDL and no cross toolchain.
// workspace/h700/check-video-preview.sh does exactly that, against values
// derived independently of this code.
//
// On the device this was ported from it replaced handing the three planes to
// SDL as an IYUV texture and letting the fragment shader convert them, which is
// cheaper but produced badly shifted colours there while leaving greyscale
// clips correct -- the signature of the chroma channels being exchanged
// somewhere below the source that could be read. The H700 has a different GPU
// stack and may well not have that bug, but re-testing that is a curiosity,
// not this port's job. See generic_video.c for the full account.
//
// Verified against ffmpeg's own BT.601 output on a real 720x480 frame: mean
// absolute error under 2 per channel, worst 3, no pixel off by more than 8.
// The residual is chroma upsampling -- ffmpeg interpolates, this replicates.

#ifndef NEXTUI_YUV2RGB_H
#define NEXTUI_YUV2RGB_H

#include <stdint.h>
#include <stddef.h>

// Must match the PLAT_YUV_* values in api.h, which is asserted in
// generic_video.c.
#define RG_YUV_BT601 0
#define RG_YUV_BT709 1
#define RG_YUV_FULL 2

// 8.8 fixed point, applied as:
//
//   R = (yscale*(Y - yoffset) + rv*(V-128)              + 128) >> 8
//   G = (yscale*(Y - yoffset) + gu*(U-128) + gv*(V-128) + 128) >> 8
//   B = (yscale*(Y - yoffset) + bu*(U-128)              + 128) >> 8
//
// The limited-range rows scale by 298/256 because the signal occupies 16..235
// rather than 0..255. The full-range row does not scale at all, which is the
// whole difference between them.
struct rg_yuv_coefficients {
	int yscale;
	int yoffset;
	int rv;
	int gu;
	int gv;
	int bu;
};

static inline struct rg_yuv_coefficients rg_yuv_for(int mode)
{
	switch (mode) {
	case RG_YUV_FULL:
		return (struct rg_yuv_coefficients){ 256, 0, 359, -88, -183, 454 };
	case RG_YUV_BT709:
		return (struct rg_yuv_coefficients){ 298, 16, 459, -55, -136, 541 };
	default:
		return (struct rg_yuv_coefficients){ 298, 16, 409, -100, -208, 516 };
	}
}

static inline uint8_t rg_clamp_u8(int value)
{
	if (value < 0) return 0;
	if (value > 255) return 255;
	return (uint8_t)value;
}

static inline uint32_t rg_yuv_pixel_to_argb(
	const struct rg_yuv_coefficients *c, int luma_sample, int cb_sample,
	int cr_sample)
{
	int luma = c->yscale * (luma_sample - c->yoffset);
	int cb = cb_sample - 128;
	int cr = cr_sample - 128;
	uint8_t r = rg_clamp_u8((luma + c->rv * cr + 128) >> 8);
	uint8_t g = rg_clamp_u8((luma + c->gu * cb + c->gv * cr + 128) >> 8);
	uint8_t b = rg_clamp_u8((luma + c->bu * cb + 128) >> 8);
	return 0xFF000000u | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b;
}

// Converts a whole frame into a caller-supplied ARGB8888 buffer.
//
// Chroma is replicated rather than interpolated: at the size this draws into,
// the difference is not visible, and interpolation would double the work in the
// inner loop of the only part of this feature that costs CPU per frame.
static inline void rg_yuv420_to_argb(
	const uint8_t *y, int ystride,
	const uint8_t *u, int ustride,
	const uint8_t *v, int vstride,
	int width, int height, int mode,
	void *pixels, int pitch)
{
	const struct rg_yuv_coefficients c = rg_yuv_for(mode);

	for (int row = 0; row < height; row++) {
		const uint8_t *yrow = y + (size_t)row * ystride;
		// 4:2:0 -- one chroma sample per two rows and two columns.
		const uint8_t *urow = u + (size_t)(row >> 1) * ustride;
		const uint8_t *vrow = v + (size_t)(row >> 1) * vstride;
		uint32_t *out = (uint32_t *)((uint8_t *)pixels + (size_t)row * pitch);

		for (int col = 0; col < width; col++) {
			out[col] = rg_yuv_pixel_to_argb(&c, yrow[col],
				urow[col >> 1], vrow[col >> 1]);
		}
	}
}

#endif
