// Decoded audio samples to interleaved stereo S16, on the CPU.
//
// The counterpart of yuv2rgb.h, and it exists for the same reason: a header of
// static inlines can be compiled and checked on the build host, where there is
// no SDL and no cross toolchain. workspace/h700/check-video-preview.sh does
// exactly that.
//
// This is what libswresample would otherwise be for. It is not worth a fourth
// shared library: ffmpeg's AAC decoder emits planar float, MP3 emits planar
// float or planar S16, and turning either into the interleaved S16 that SDL
// wants is a scale, a clamp and an interleave. Rate conversion -- the other
// thing swresample does -- is done by SDL itself, which builds a converting
// stream when the device cannot take the rate the clip is in.
//
// Nothing here scales for volume. Volume and mute on this device are applied at
// the ALSA mixer by SetVolume()/PLAT_overrideMute(), below SDL, so preview audio
// follows the system volume and the mute switch without being told to.

#ifndef NEXTUI_PCM_H
#define NEXTUI_PCM_H

#include <math.h>
#include <stddef.h>
#include <stdint.h>

// Must match the AV_SAMPLE_FMT_* values, which vidplayer.c asserts. Mirroring
// them rather than translating is what lets this file -- and its test -- be
// built without an ffmpeg header in sight.
#define RG_PCM_U8 0
#define RG_PCM_S16 1
#define RG_PCM_S32 2
#define RG_PCM_FLT 3
#define RG_PCM_DBL 4
#define RG_PCM_U8P 5
#define RG_PCM_S16P 6
#define RG_PCM_S32P 7
#define RG_PCM_FLTP 8
#define RG_PCM_DBLP 9

// Gain is fixed point so a fade costs a multiply and a shift per sample rather
// than a float multiply, and so the ramp is exactly reproducible in the test.
#define RG_PCM_GAIN_ONE 4096

static inline int rg_pcm_planar(int format)
{
	return format >= RG_PCM_U8P;
}

static inline int rg_pcm_supported(int format)
{
	return format >= RG_PCM_U8 && format <= RG_PCM_DBLP;
}

static inline int16_t rg_pcm_clamp_s16(int32_t value)
{
	if (value < -32768) return -32768;
	if (value > 32767) return 32767;
	return (int16_t)value;
}

// Full scale is 32768, not 32767: -1.0 must reach -32768 exactly, which is the
// convention every decoder and every resampler uses. The asymmetry is real and
// is why the clamp above exists -- +1.0 would otherwise wrap to -32768, which
// is not a quiet error, it is a loud one.
static inline int16_t rg_pcm_from_float(double value)
{
	return rg_pcm_clamp_s16((int32_t)lrint(value * 32768.0));
}

// One sample, whatever it was decoded as.
//
// A switch per sample rather than a loop per format: at 48 kHz stereo this runs
// 96,000 times a second, which is well under a tenth of one percent of a core,
// and the alternative is ten near-identical loops where a transposed index in
// any one of them would be invisible.
static inline int16_t rg_pcm_sample(const uint8_t *const *data, int format,
	int channels, int frame, int channel)
{
	size_t index;
	const uint8_t *plane;

	if (rg_pcm_planar(format)) {
		plane = data[channel];
		index = (size_t)frame;
	} else {
		plane = data[0];
		index = (size_t)frame * (size_t)channels + (size_t)channel;
	}
	if (!plane) return 0;

	switch (format) {
	case RG_PCM_U8:
	case RG_PCM_U8P:
		// Unsigned 8-bit is centred on 128, so it is shifted before scaling.
		return (int16_t)((int32_t)(plane[index] - 128) << 8);
	case RG_PCM_S16:
	case RG_PCM_S16P:
		return ((const int16_t *)plane)[index];
	case RG_PCM_S32:
	case RG_PCM_S32P:
		return (int16_t)(((const int32_t *)plane)[index] >> 16);
	case RG_PCM_FLT:
	case RG_PCM_FLTP:
		return rg_pcm_from_float(((const float *)plane)[index]);
	case RG_PCM_DBL:
	case RG_PCM_DBLP:
		return rg_pcm_from_float(((const double *)plane)[index]);
	default:
		return 0;
	}
}

// A whole decoded frame into interleaved stereo S16.
//
// Channel handling is deliberately blunt. Mono is duplicated to both sides;
// anything with two or more channels contributes its first two and the rest are
// dropped. A real downmix needs per-layout coefficients and a centre/LFE
// policy, which is a great deal of machinery for a case no scraper produces --
// and dropping the surrounds is a quiet, correct-sounding result, where getting
// the coefficients wrong is not.
static inline void rg_pcm_to_s16_stereo(const uint8_t *const *data, int format,
	int channels, int frames, int16_t *out)
{
	if (frames <= 0 || channels <= 0) return;

	if (channels == 1) {
		for (int i = 0; i < frames; i++) {
			int16_t mono = rg_pcm_sample(data, format, 1, i, 0);
			out[2 * i] = mono;
			out[2 * i + 1] = mono;
		}
		return;
	}
	for (int i = 0; i < frames; i++) {
		out[2 * i] = rg_pcm_sample(data, format, channels, i, 0);
		out[2 * i + 1] = rg_pcm_sample(data, format, channels, i, 1);
	}
}

// A constant gain over `frames` interleaved stereo frames. Used to carry a fade
// that has not finished across a buffer boundary.
static inline void rg_pcm_gain(int16_t *stereo, int frames, int gain)
{
	if (frames <= 0 || gain == RG_PCM_GAIN_ONE) return;
	for (int i = 0; i < 2 * frames; i++) {
		stereo[i] = (int16_t)(((int32_t)stereo[i] * gain) / RG_PCM_GAIN_ONE);
	}
}

// A linear gain ramp across `frames` interleaved stereo frames.
//
// Without this, every start, stop and loop point cuts a PCM stream mid-waveform
// and the step to or from zero is audible as a click -- on a small speaker,
// often louder than the clip itself.
//
// The endpoints are inclusive: frame 0 is at `from` and the last frame is at
// `to`. A fade-out therefore really reaches silence rather than stopping one
// step short of it, which is the step that clicks. Chunks of a longer fade join
// by having the caller compute each chunk's own endpoints from its position.
static inline void rg_pcm_ramp(int16_t *stereo, int frames, int from, int to)
{
	if (frames <= 0) return;
	if (frames == 1) {
		rg_pcm_gain(stereo, 1, to);
		return;
	}
	for (int i = 0; i < frames; i++) {
		int gain = from + (int)(((int64_t)(to - from) * i) / (frames - 1));
		stereo[2 * i] = (int16_t)(((int32_t)stereo[2 * i] * gain) /
			RG_PCM_GAIN_ONE);
		stereo[2 * i + 1] = (int16_t)(((int32_t)stereo[2 * i + 1] * gain) /
			RG_PCM_GAIN_ONE);
	}
}

#endif
