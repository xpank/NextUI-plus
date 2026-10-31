#include "vidaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// defines.h first: api.h uses BTN_ID_COUNT and the other platform
// enums without including the header that declares them, so including
// it on its own does not compile.
#include "defines.h"
#include "api.h"
#include "sdl.h"

#include "pcm.h"

// How much decoded audio the ring can hold. The decoder reads ahead to
// VIDA_HIGH_WATER_MS and the rest is slack, because a decoded AAC frame is
// 1024 samples and arrives whole -- the gate can only be checked between
// frames, never inside one.
#define VIDA_RING_MS 2000

// A fade this long is inaudible as a fade and completely removes the click.
// Shorter than about 20 ms stops working; much longer starts sounding like a
// deliberate effect on a two-second clip.
#define VIDA_FADE_MS 40

// What SDL asks for in one callback, in milliseconds rather than in frames.
//
// Frames was the obvious choice and was wrong. 1024 frames is 21 ms at 48 kHz
// and 93 ms at 11 kHz, and the period is both the granularity at which the ring
// is drained and the whole of the clock's latency term -- so a low-rate clip
// got a 139 ms correction where a 48 kHz one got 32 ms, and its ring emptied in
// jumps big enough to notice at the loop point. About 20 ms at any rate, which
// is short enough that the fade-out on close finishes promptly and long enough
// that the callback is not run absurdly often on a device whose main thread is
// busy converting video frames.
#define VIDA_PERIOD_MS 20
#define VIDA_PERIOD_MIN 256
#define VIDA_PERIOD_MAX 2048

static struct {
	SDL_AudioDeviceID device;
	bool open;
	int rate;

	int16_t *ring;      // interleaved stereo
	int capacity;       // frames
	int head;           // read position, frames
	int count;          // frames held

	// The master clock. Advanced by every frame the device is handed,
	// including silence emitted during an underrun -- that time really did
	// pass at the speaker, so the clock has to reflect it or the picture
	// freezes whenever audio starves.
	int64_t played;

	// The other half of counting silence as time. Audio the device did not
	// get is audio that must never be heard late, so the same number of frames
	// is dropped from the front of whatever arrives next. Without this the
	// stream would slip a little further behind the clock with every underrun.
	int64_t drop;

	int underruns;

	int period;         // frames per SDL callback, ~VIDA_PERIOD_MS at this rate
	int fade_frames;    // VIDA_FADE_MS at this rate
	int fade_in_left;   // frames of the fade-in still to apply
	bool closing;
	int close_gain;

	// Open and running, but between clips: the callback emits silence and the
	// clock does not move, because no clip is playing for it to measure. This
	// is what replaces pausing the device -- SDL stops writing to ALSA while a
	// device is paused, the hardware buffer runs out, and the resulting XRUN
	// stops the stream, which is most of the way to closing it.
	bool idle;

	// The demuxer has read the whole file, so a ring that empties is the clip
	// ending, not the decoder losing. See VIDA_endOfStream().
	bool draining;
} vida;

static bool vida_subsystem = false;

static int vida_ms_to_frames(int ms, int rate)
{
	return (int)(((int64_t)rate * ms) / 1000);
}

// SDL rounds `samples` up to a power of two, so it is chosen as one here and
// the arithmetic that depends on it stays exact.
static int vida_period_frames(int rate)
{
	int want = vida_ms_to_frames(VIDA_PERIOD_MS, rate);
	int frames = VIDA_PERIOD_MIN;
	while (frames < want && frames < VIDA_PERIOD_MAX) frames <<= 1;
	return frames;
}

// Gain at position `pos` of a `total`-frame fade, with inclusive endpoints:
// frame 0 is silent and frame total-1 is at full level.
static int vida_fade_gain(int pos, int total)
{
	if (total <= 1 || pos >= total - 1) return RG_PCM_GAIN_ONE;
	if (pos <= 0) return 0;
	return (int)(((int64_t)RG_PCM_GAIN_ONE * pos) / (total - 1));
}

static void vida_callback(void *userdata, Uint8 *stream, int len)
{
	(void)userdata;
	int16_t *out = (int16_t *)stream;
	int frames = len / (int)(2 * sizeof(int16_t));
	if (frames <= 0) return;

	if (vida.idle) {
		memset(out, 0, (size_t)frames * 2 * sizeof(int16_t));
		return;
	}

	int taken = vida.count < frames ? vida.count : frames;
	for (int i = 0; i < taken; i++) {
		int slot = (vida.head + i) % vida.capacity;
		out[2 * i] = vida.ring[2 * slot];
		out[2 * i + 1] = vida.ring[2 * slot + 1];
	}
	vida.head = (vida.head + taken) % vida.capacity;
	vida.count -= taken;

	if (taken < frames) {
		memset(out + 2 * taken, 0,
			(size_t)(frames - taken) * 2 * sizeof(int16_t));
		if (!vida.draining) {
			vida.drop += frames - taken;
			vida.underruns++;
		}
	}

	// Closing: ride the level down over VIDA_FADE_MS and stay at zero. This is
	// the one fade that cannot be applied to the ring in advance, because the
	// stop is asynchronous -- the selection moved, and these samples are
	// already queued.
	if (vida.closing) {
		int step = vida.fade_frames > 0
			? (int)(((int64_t)RG_PCM_GAIN_ONE * frames) / vida.fade_frames)
			: RG_PCM_GAIN_ONE;
		int to = vida.close_gain - step;
		if (to < 0) to = 0;
		rg_pcm_ramp(out, frames, vida.close_gain, to);
		vida.close_gain = to;
	}

	vida.played += frames;
}

// Everything that belongs to one clip rather than to the device. Called with
// the audio lock held, or before the device exists.
static void vida_clear_clip(void)
{
	vida.head = 0;
	vida.count = 0;
	vida.played = 0;
	vida.drop = 0;
	vida.underruns = 0;
	vida.draining = false;
	vida.closing = false;
	vida.close_gain = RG_PCM_GAIN_ONE;
	vida.fade_in_left = vida.fade_frames;
}

// The ramp down, and the wait for it to reach the speaker. Shared by the two
// ways a clip can end: giving the device up, and holding on to it.
static void vida_fade_out(void)
{
	SDL_LockAudioDevice(vida.device);
	vida.closing = true;
	SDL_UnlockAudioDevice(vida.device);
	// Long enough for the ramp to be generated and for the buffer it landed in
	// to reach the speaker. Without it the stream is cut mid-waveform and the
	// stop clicks as loudly as the start would have.
	SDL_Delay(VIDA_FADE_MS + 20);
}

bool VIDA_open(int rate, char *reason, size_t reason_size)
{
	if (rate <= 0) {
		snprintf(reason, reason_size, "sample rate is %d", rate);
		return false;
	}

	// The device we already have, if it will do. Reusing it is the whole point
	// of VIDA_idle(): the ALSA stream never stops, so the codec is never
	// power-cycled between two clips. A clip at a different rate cannot reuse
	// it -- the rate is fixed at open -- and pays the cycle, which is the one
	// navigation left that is audible.
	if (vida.open) {
		if (vida.rate == rate) {
			SDL_LockAudioDevice(vida.device);
			vida.idle = true;
			vida_clear_clip();
			SDL_UnlockAudioDevice(vida.device);
			return true;
		}
		VIDA_close();
	}
	memset(&vida, 0, sizeof(vida));

	// The audio subsystem is initialised once and never torn down. Quitting it
	// would unload SDL's ALSA backend -- and the libasound it dlopen()ed --
	// only to load it again for the next clip; the device itself is what has to
	// be released between clips, and that is what VIDA_close does.
	if (!vida_subsystem) {
		if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
			snprintf(reason, reason_size, "SDL audio init failed: %s",
				SDL_GetError());
			return false;
		}
		vida_subsystem = true;
		LOG_info("video preview: audio driver is %s\n",
			SDL_GetCurrentAudioDriver() ? SDL_GetCurrentAudioDriver() : "none");
	}

	SDL_AudioSpec want;
	SDL_AudioSpec have;
	SDL_zero(want);
	SDL_zero(have);
	want.freq = rate;
	want.format = AUDIO_S16SYS;
	want.channels = 2;
	vida.period = vida_period_frames(rate);
	want.samples = (Uint16)vida.period;
	want.callback = vida_callback;

	// Zero allowed changes on purpose. SDL then guarantees the callback is fed
	// exactly this format and rate, and builds its own converting stream when
	// the hardware wants something else. Allowing changes would push that
	// conversion back onto us and cost a resampler.
	vida.device = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (vida.device == 0) {
		snprintf(reason, reason_size, "SDL_OpenAudioDevice: %s", SDL_GetError());
		return false;
	}

	vida.rate = rate;
	vida.capacity = vida_ms_to_frames(VIDA_RING_MS, rate);
	if (vida.capacity < vida.period * 4) {
		vida.capacity = vida.period * 4;
	}
	vida.ring = (int16_t *)malloc((size_t)vida.capacity * 2 * sizeof(int16_t));
	if (!vida.ring) {
		SDL_CloseAudioDevice(vida.device);
		vida.device = 0;
		snprintf(reason, reason_size, "out of memory for a %d-frame ring",
			vida.capacity);
		return false;
	}

	vida.fade_frames = vida_ms_to_frames(VIDA_FADE_MS, rate);
	if (vida.fade_frames < 1) vida.fade_frames = 1;
	vida.idle = true;
	vida_clear_clip();
	vida.open = true;

	// Running from here until VIDA_close(), on silence until VIDA_start().
	// Opening paused and unpausing once the ring is primed reads better, but
	// it leaves the PCM stopped, and starting it is exactly the power-up this
	// file exists to stop doing on every navigation.
	SDL_PauseAudioDevice(vida.device, 0);
	return true;
}

bool VIDA_isOpen(void)
{
	return vida.open;
}

int VIDA_capacityFrames(void)
{
	return vida.open ? vida.capacity : 0;
}

int VIDA_queuedFrames(void)
{
	if (!vida.open) return 0;
	SDL_LockAudioDevice(vida.device);
	int count = vida.count;
	SDL_UnlockAudioDevice(vida.device);
	return count;
}

int VIDA_underruns(void)
{
	return vida.underruns;
}

int VIDA_push(const int16_t *stereo, int frames)
{
	if (!vida.open || !stereo || frames <= 0) return 0;

	SDL_LockAudioDevice(vida.device);

	// Underrun repayment: the device already played silence where these frames
	// belonged, so they are dropped rather than played late.
	if (vida.drop > 0) {
		int skip = vida.drop > frames ? frames : (int)vida.drop;
		vida.drop -= skip;
		stereo += (size_t)skip * 2;
		frames -= skip;
		if (frames <= 0) {
			SDL_UnlockAudioDevice(vida.device);
			return 0;
		}
	}

	int room = vida.capacity - vida.count;
	int take = frames < room ? frames : room;
	for (int i = 0; i < take; i++) {
		int slot = (vida.head + vida.count + i) % vida.capacity;
		vida.ring[2 * slot] = stereo[2 * i];
		vida.ring[2 * slot + 1] = stereo[2 * i + 1];
	}

	// Fade in the opening frames of the clip, and of every pass after a loop.
	// Applied here rather than in the callback because here the position in the
	// fade is exactly known.
	if (vida.fade_in_left > 0 && take > 0) {
		int done = vida.fade_frames - vida.fade_in_left;
		int n = vida.fade_in_left < take ? vida.fade_in_left : take;
		int start = (vida.head + vida.count) % vida.capacity;
		for (int i = 0; i < n; i++) {
			int slot = (start + i) % vida.capacity;
			rg_pcm_gain(&vida.ring[2 * slot], 1,
				vida_fade_gain(done + i, vida.fade_frames));
		}
		vida.fade_in_left -= n;
	}

	vida.count += take;
	SDL_UnlockAudioDevice(vida.device);
	return take;
}

void VIDA_endOfStream(void)
{
	if (!vida.open) return;
	SDL_LockAudioDevice(vida.device);
	vida.draining = true;
	int n = vida.count < vida.fade_frames ? vida.count : vida.fade_frames;
	// The last n frames in the ring, ramped to silence -- frame by frame
	// because the ring wraps and a fade that stopped at the wrap would leave
	// exactly the discontinuity it was there to remove.
	int first = vida.count - n;
	for (int i = 0; i < n; i++) {
		int slot = (vida.head + first + i) % vida.capacity;
		rg_pcm_gain(&vida.ring[2 * slot], 1,
			vida_fade_gain(n - 1 - i, n));
	}
	SDL_UnlockAudioDevice(vida.device);
}

void VIDA_start(void)
{
	if (!vida.open) return;
	SDL_LockAudioDevice(vida.device);
	vida.idle = false;
	SDL_UnlockAudioDevice(vida.device);
}

void VIDA_reset(void)
{
	if (!vida.open) return;
	SDL_LockAudioDevice(vida.device);
	// Silence rather than SDL_PauseAudioDevice: a paused device stops being
	// written to, the hardware buffer empties, and ALSA reports an XRUN --
	// which is what happened at every loop point before this change, stopping
	// the stream the loop was trying to continue.
	vida.idle = true;
	int keep = vida.underruns;
	vida_clear_clip();
	// The dry-ring diagnostic is per clip, and a loop is the same clip.
	vida.underruns = keep;
	SDL_UnlockAudioDevice(vida.device);
}

int64_t VIDA_clockMs(void)
{
	if (!vida.open) return 0;
	SDL_LockAudioDevice(vida.device);
	int64_t played = vida.played;
	SDL_UnlockAudioDevice(vida.device);

	// What the callback has handed over is not yet what has been heard, and
	// the clock counts a block as played the moment it is generated. Measured
	// on the device, /proc/asound/card0/pcm0p/sub0 reports period_size 1024
	// and buffer_size 2048 for a 1024-frame request -- SDL's ALSA backend asks
	// for two periods of the size given -- so a block written into a full
	// buffer starts playing one period later and finishes two periods later.
	// Its midpoint, which is what a listener hears it at, is one and a half
	// periods out.
	//
	// Subtracting that puts the clock at the speaker rather than at the
	// driver, which is where the picture has to match.
	played -= vida.period * 3 / 2;
	if (played < 0) played = 0;
	return (played * 1000) / vida.rate;
}

void VIDA_close(void)
{
	if (!vida.open) return;

	// An idle device has nothing to fade and nothing in flight, so it closes
	// immediately -- which is the common case, because a clip that ends while
	// the list is still up has already been through VIDA_idle().
	if (!vida.idle) vida_fade_out();

	SDL_PauseAudioDevice(vida.device, 1);
	SDL_CloseAudioDevice(vida.device);
	vida.device = 0;
	vida.open = false;
	free(vida.ring);
	vida.ring = NULL;
}

void VIDA_idle(void)
{
	if (!vida.open || vida.idle) return;
	vida_fade_out();
	SDL_LockAudioDevice(vida.device);
	vida.idle = true;
	vida_clear_clip();
	SDL_UnlockAudioDevice(vida.device);
}
