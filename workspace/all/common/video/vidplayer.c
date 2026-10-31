#include "vidplayer.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

// defines.h first: api.h uses BTN_ID_COUNT and the other platform
// enums without including the header that declares them, so including
// it on its own does not compile.
#include "defines.h"
#include "api.h"
#include "config.h"
#include "pcm.h"
#include "vidaudio.h"
#include "vidload.h"

// pcm.h mirrors the AV_SAMPLE_FMT_* numbering so that it -- and its test --
// can be built on the host without an ffmpeg header in sight. That only works
// while the two really do agree, so they are checked here, where both are
// visible. The same arrangement as RG_YUV_* against PLAT_YUV_* in
// generic_video.c.
_Static_assert(RG_PCM_U8 == AV_SAMPLE_FMT_U8, "pcm.h U8 disagrees with ffmpeg");
_Static_assert(RG_PCM_S16 == AV_SAMPLE_FMT_S16, "pcm.h S16 disagrees with ffmpeg");
_Static_assert(RG_PCM_S32 == AV_SAMPLE_FMT_S32, "pcm.h S32 disagrees with ffmpeg");
_Static_assert(RG_PCM_FLT == AV_SAMPLE_FMT_FLT, "pcm.h FLT disagrees with ffmpeg");
_Static_assert(RG_PCM_DBL == AV_SAMPLE_FMT_DBL, "pcm.h DBL disagrees with ffmpeg");
_Static_assert(RG_PCM_U8P == AV_SAMPLE_FMT_U8P, "pcm.h U8P disagrees with ffmpeg");
_Static_assert(RG_PCM_S16P == AV_SAMPLE_FMT_S16P, "pcm.h S16P disagrees with ffmpeg");
_Static_assert(RG_PCM_S32P == AV_SAMPLE_FMT_S32P, "pcm.h S32P disagrees with ffmpeg");
_Static_assert(RG_PCM_FLTP == AV_SAMPLE_FMT_FLTP, "pcm.h FLTP disagrees with ffmpeg");
_Static_assert(RG_PCM_DBLP == AV_SAMPLE_FMT_DBLP, "pcm.h DBLP disagrees with ffmpeg");

// There is no resolution limit, and no aspect-ratio expectation. Any shape and
// any size is accepted and fitted into the art panel: the draw path computes the
// destination rectangle from the clip's own aspect ratio -- the same arithmetic
// the cover image uses -- and the GPU scales the frame into it during
// RenderCopy, which costs nothing extra whatever the source size.
//
// This replaced two successive guesses at a size cap. The first, 640x480 as
// separate width and height limits, rejected the 720x480 that screenscraper
// actually produces, and so refused every GBA preview on the owner's card. The
// second raised it to 1280x720 worth of pixels, which fixed that case and would
// have gone on being a number chosen here rather than a limit the device
// actually has.
//
// The real constraint was never display; it is decode time, because this SoC
// has no video decoder and every frame costs CPU. That is not a property of a
// resolution -- it is a property of this device, this clip and this moment, and
// it is measurable. So the decoder measures it: it plays whatever it is given
// and adapts when it cannot keep up (see the lateness handling below), instead
// of refusing work it might well have managed.
//
// What remains is a memory sanity bound, which is a different claim. A frame is
// buffered in full, several are held as references, and H.264 permits a decoded
// picture buffer of up to sixteen. Beyond 4K that becomes hundreds of megabytes
// for a clip playing in a panel a few hundred pixels wide, which no scraper
// produces and nothing here should attempt.
#define VIDP_MAX_SOURCE_PIXELS (3840 * 2160)

// Above this, decode gets a second thread. Frame threading costs one frame of
// latency per thread, which is why the common case does not pay for it. The
// H700 has four A53 cores and NextUI is drawing on one of them.
#define VIDP_THREAD_PIXELS (640 * 480)

// How the decoder recognises that it is losing, and what it does about it.
//
// A frame that arrives after its presentation time is "late". Occasional
// lateness is normal -- a keyframe costs more than the frames around it. What
// matters is sustained lateness, which means decode is slower than playback and
// the clip would otherwise drift into slow motion, because a decoder that
// cannot keep up does not stutter, it falls behind and stays behind.
//
// The response is to drop non-reference frames: a real decode saving rather
// than a display trick, typically removing a third to a half of the work, and
// it costs smoothness rather than speed. Choppy is an acceptable preview; slow
// motion is not, because it reads as broken.
#define VIDP_LATE_MS 120
#define VIDP_LATE_FRAMES_BEFORE_DEGRADE 10

// Presentation is capped at 30 fps. Faster sources are decoded in full -- there
// is no way to skip a frame without knowing whether something later references
// it -- but only every other frame is published, so a 60 fps clip still plays
// at its true speed instead of at half speed, and the GPU upload happens half
// as often.
#define VIDP_MIN_FRAME_INTERVAL_MS 33

// A single stuck read must not wedge the worker forever.
#define VIDP_MAX_SLEEP_MS 1000

// How far ahead of the speaker the demuxer runs when a clip has sound.
//
// Read-ahead is not an optimisation here, it is the whole reason the audio
// path needs any structure at all. Packets in an MP4 are interleaved in
// decode order, so reaching the video packet for moment T means having read
// the audio for moment T and no further -- and the audio for moment T is
// exactly the audio the device wants *now*. Publishing that video frame when
// the audio clock reaches T therefore leaves the ring empty by construction,
// and the device underruns on every single frame.
//
// So the reader runs ahead until the ring holds VIDP_AUDIO_HIGH_WATER_MS, and
// the video packets it passes on the way are queued rather than decoded. The
// queue holds compressed packets, not decoded frames: a few kilobytes each
// instead of half a megabyte each.
#define VIDP_AUDIO_HIGH_WATER_MS 400
// Filled before the device is unpaused, so the first thing heard is not a
// stutter.
#define VIDP_AUDIO_PRIME_MS 250
// Deep enough for VIDP_AUDIO_HIGH_WATER_MS of video at any frame rate a
// scraper produces -- 400 ms is 12 packets at 30 fps, 24 at 60.
#define VIDP_VQ_SLOTS 64
// A wait for a frame's moment is taken in slices this long, with a read
// between them. A single uninterrupted sleep would be that many milliseconds
// in which the ring only drains.
#define VIDP_AUDIO_SLICE_MS 8

// How long the audio device is kept after the last clip stops.
//
// Closing the PCM powers the codec's DAC, output mixer, line-out and speaker
// amplifier down, and opening it powers them back up; on this hardware that is
// audible. A selection change stops the clip immediately and asks for the next
// one only after the two-second rest timer, so anything shorter than that
// closes the device between every pair of ROMs and puts the click back. This is
// comfortably longer, so browsing -- including past ROMs that have no preview
// at all -- costs no power cycles, and a session that has stopped browsing
// gives the device back within a few seconds.
//
// Sleep and shutdown do not wait for it: both close the device outright.
#define VIDP_AUDIO_LINGER_MS 5000

static pthread_once_t vidp_thread_once = PTHREAD_ONCE_INIT;
static pthread_t vidp_thread;
static bool vidp_thread_started = false;

static pthread_mutex_t vidp_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vidp_cond = PTHREAD_COND_INITIALIZER;
// Signalled when the worker goes back to waiting for a request, which is the
// point at which it has released everything the previous clip held -- the
// audio device in particular. See VIDP_stopAndWait().
static pthread_cond_t vidp_idle_cond = PTHREAD_COND_INITIALIZER;
static bool vidp_worker_idle = true;

// Request slot. Latest wins; there is no queue on purpose.
static char vidp_request_path[1024];
static bool vidp_request_pending = false;
static bool vidp_shutdown = false;
static unsigned vidp_generation = 0;

// Published frame. Owned by the worker, read by the draw thread under the
// mutex. Two AVFrames are kept so the decoder can fill one while the other is
// on screen, and they are swapped rather than copied.
static AVFrame *vidp_published = NULL;
static bool vidp_has_frame = false;
static unsigned vidp_published_generation = 0;

// Every failure keeps the static image, and nothing here logs per frame or per
// attempt: browsing a folder of forty ROMs must not produce forty log lines,
// and a clip that loops for a minute must not produce eighteen hundred.
//
// The gate is per (clip, kind of message), not per session. It was per session,
// which is cheaper and was wrong: the second clip of a session that had
// anything to say said nothing at all, because the first had already spent the
// single line the session allowed. A full-range fixture that in fact played
// correctly therefore produced no line whatever, and read as a silent failure
// for as long as it took to prove otherwise -- exactly the case the logging
// existed to make legible.
//
// So each clip gets to say each thing once, and revisiting a clip stays quiet.
// The bound is the ring below: only the most recent VIDP_LOG_SLOTS clips are
// remembered, so a session long enough to evict one may repeat its line -- at
// worst once per rest, never once per frame.
typedef enum {
	VIDP_LOG_IDENTITY = 0,
	VIDP_LOG_OPEN_FAILURE,
	VIDP_LOG_UNSUPPORTED_CODEC,
	VIDP_LOG_OVERSIZE,
	VIDP_LOG_PIXEL_FORMAT,
	VIDP_LOG_DEGRADED,
	VIDP_LOG_AUDIO_CODEC,
	VIDP_LOG_AUDIO_FORMAT,
	VIDP_LOG_AUDIO_DEVICE,
	VIDP_LOG_AUDIO_UNDERRUN,
} VIDP_LogKind;

#define VIDP_LOG_SLOTS 32

static struct {
	uint64_t hash;
	uint32_t kinds;  // bit per VIDP_LogKind already said about this clip
} vidp_log_seen[VIDP_LOG_SLOTS];
static unsigned vidp_log_next = 0;

// Why the dlopen failed cannot change within a session, so that one stays
// once per session and is not keyed to a clip.
static bool vidp_logged_unavailable = false;

// What the first clip of the session was, so later clips can say whether they
// agree with it -- which is the difference between a uniformly exported pack
// and a mixed one, on the same line rather than in a second message.
static int vidp_first_format = -1;
static int vidp_first_range = -1;
static int vidp_first_space = -1;

static uint64_t vidp_path_hash(const char *path) {
	// FNV-1a. The path is only ever compared against itself, so the only
	// property needed is that different clips rarely collide.
	uint64_t h = 1469598103934665603ull;
	for (const unsigned char *p = (const unsigned char *)path; *p; p++) {
		h ^= *p;
		h *= 1099511628211ull;
	}
	return h;
}

// True the first time this clip has this kind of thing to say. Called only from
// the worker thread, which is the only thread that logs about a clip.
static bool vidp_first_time(const char *path, VIDP_LogKind kind) {
	const uint64_t hash = vidp_path_hash(path);
	const uint32_t bit = 1u << (unsigned)kind;

	for (unsigned i = 0; i < VIDP_LOG_SLOTS; i++) {
		if (vidp_log_seen[i].hash != hash) continue;
		if (vidp_log_seen[i].kinds & bit) return false;
		vidp_log_seen[i].kinds |= bit;
		return true;
	}

	// Not remembered: take the oldest slot. A hash collision costs a line that
	// is not printed, never a line that is wrong.
	vidp_log_seen[vidp_log_next].hash = hash;
	vidp_log_seen[vidp_log_next].kinds = bit;
	vidp_log_next = (vidp_log_next + 1) % VIDP_LOG_SLOTS;
	return true;
}

// An A/V drift line once a second, while this file exists.
//
// Whether picture and sound stay together over a long loop is not something
// anybody can assert from a couch, and it is the one property of this feature
// that a screenshot cannot show. The line carries wall time, the audio clock
// and the frame's own presentation time, so a run of a few minutes shows both
// whether the two stay together and whether the audio clock tracks real time
// at all -- a device consuming samples at the wrong rate shows up as the audio
// clock drifting away from wall time even while video follows it perfectly.
//
// A file rather than an environment variable, and the difference matters. The
// launcher inherits its environment from MinUI.pak/launch.sh, which is read
// once at boot; a variable therefore cannot be turned on for a device that is
// already running, and the device this was measured on could not be rebooted
// cheaply. A marker on tmpfs can be created over adb at any moment:
//
//     adb shell touch /tmp/nextui-video-av-log
//
// Checked once per clip, so it takes effect at the next preview and costs one
// access() per clip when it is not there.
#define VIDP_AV_LOG_MARKER "/tmp/nextui-video-av-log"

static int64_t vidp_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void vidp_sleep_ms(int64_t ms) {
	if (ms <= 0) return;
	if (ms > VIDP_MAX_SLEEP_MS) ms = VIDP_MAX_SLEEP_MS;
	struct timespec ts = {
		.tv_sec = ms / 1000,
		.tv_nsec = (ms % 1000) * 1000000,
	};
	nanosleep(&ts, NULL);
}

// What the three planes mean, according to the clip rather than to us.
//
// yuvj420p is the historical spelling of "full range"; modern ffmpeg reports
// the same thing through color_range, and files in the wild carry either. Both
// are honoured.
static VIDP_YuvMode vidp_yuv_mode(const AVFrame *frame) {
	if (frame->color_range == AVCOL_RANGE_JPEG ||
		frame->format == AV_PIX_FMT_YUVJ420P) {
		return VIDP_YUV_FULL;
	}
	if (frame->colorspace == AVCOL_SPC_BT709) return VIDP_YUV_BT709;
	return VIDP_YUV_BT601;
}

// True when the clip the worker is decoding is no longer the one wanted.
static bool vidp_superseded(unsigned generation) {
	pthread_mutex_lock(&vidp_mutex);
	bool stale = vidp_shutdown || vidp_request_pending ||
		generation != vidp_generation;
	pthread_mutex_unlock(&vidp_mutex);
	return stale;
}

// Hands the decoded frame to the presenter and gives the decoder back a frame
// to fill next. False means allocation failed and the clip must stop.
static bool vidp_publish(AVFrame **frame, unsigned generation) {
	const VID_Api *api = VID_api();
	pthread_mutex_lock(&vidp_mutex);
	// Swap rather than copy: the decoder continues into whichever frame the
	// presenter is not holding.
	AVFrame *previous = vidp_published;
	vidp_published = *frame;
	vidp_has_frame = true;
	vidp_published_generation = generation;
	pthread_mutex_unlock(&vidp_mutex);

	if (previous) {
		// Unreferenced outside the lock: the presenter can no longer reach it.
		api->av_frame_unref(previous);
		*frame = previous;
		return true;
	}
	// First publish of this clip, so there is nothing to recycle yet and the
	// decoder needs a fresh frame. Handing back the NULL that the swap produced
	// would pass NULL straight into avcodec_receive_frame.
	*frame = api->av_frame_alloc();
	return *frame != NULL;
}

static void vidp_clear_published(void) {
	pthread_mutex_lock(&vidp_mutex);
	vidp_has_frame = false;
	AVFrame *frame = vidp_published;
	vidp_published = NULL;
	pthread_mutex_unlock(&vidp_mutex);
	if (!frame) return;
	// Only reached with a frame in hand, which means ffmpeg is already loaded.
	// Asking for the API unconditionally would dlopen libavcodec on the way
	// through VIDP_stop(), defeating the whole point of loading it lazily.
	const VID_Api *api = VID_api();
	if (api) api->av_frame_free(&frame);
}


// --- the reader ------------------------------------------------------------
//
// One AVFormatContext, read by one thread, feeding two consumers. With no
// audio this is a thin wrapper that behaves exactly as the silent build did:
// read a packet, hand back the video ones, skip the rest. With audio it also
// runs ahead, decoding audio straight into the device's ring and parking the
// video packets it passes in a queue -- see VIDP_AUDIO_HIGH_WATER_MS for why
// running ahead is mandatory rather than merely nice.
typedef struct {
	const VID_Api *api;
	AVFormatContext *format;
	const char *path;
	int video_index;

	// -1 when the clip is silent, has no decodable audio, the device would not
	// open, or the owner turned preview audio off. Every one of those is the
	// same outcome: play the picture, say so once, carry on.
	int audio_index;
	AVCodecContext *audio_ctx;
	AVFrame *audio_frame;
	const char *audio_codec_name;
	int audio_channels;
	int high_water;   // frames
	int prime;        // frames

	int16_t *stage;      // interleaved stereo, between the decoder and the ring
	int stage_frames;

	AVPacket *queue[VIDP_VQ_SLOTS];
	AVPacket *scratch;   // the silent path's single packet
	int qhead;
	int qcount;

	bool eof;
	bool faded_out;
} VIDP_Reader;

static bool vidp_stage_reserve(VIDP_Reader *reader, int frames)
{
	if (reader->stage_frames >= frames) return true;
	int16_t *grown = (int16_t *)realloc(reader->stage,
		(size_t)frames * 2 * sizeof(int16_t));
	if (!grown) return false;
	reader->stage = grown;
	reader->stage_frames = frames;
	return true;
}

// One audio packet, all the way from compressed bytes to the device's ring.
static void vidp_audio_packet(VIDP_Reader *reader, AVPacket *packet)
{
	const VID_Api *api = reader->api;
	int sent = api->avcodec_send_packet(reader->audio_ctx, packet);
	if (sent < 0 && sent != AVERROR(EAGAIN)) return;

	for (;;) {
		int got = api->avcodec_receive_frame(reader->audio_ctx,
			reader->audio_frame);
		if (got < 0) break;

		AVFrame *frame = reader->audio_frame;
		int frames = frame->nb_samples;
		int channels = frame->ch_layout.nb_channels;
		if (channels <= 0) channels = reader->audio_channels;

		if (frames > 0 && rg_pcm_supported(frame->format) &&
			vidp_stage_reserve(reader, frames)) {
			// extended_data, not data: data holds only the first eight planes,
			// and reading a planar frame through it is correct right up until
			// it silently is not.
			rg_pcm_to_s16_stereo(
				(const uint8_t *const *)frame->extended_data,
				frame->format, channels, frames, reader->stage);
			VIDA_push(reader->stage, frames);
		} else if (frames > 0 && !rg_pcm_supported(frame->format) &&
			vidp_first_time(reader->path, VIDP_LOG_AUDIO_FORMAT)) {
			LOG_info("video preview: %s decodes audio as sample format %d, which pcm.h cannot convert; playing silently\n",
				reader->path, frame->format);
		}
		api->av_frame_unref(frame);
	}
}

// One packet, routed. 1 when a video packet was queued, 0 when something else
// was consumed, -1 at end of file.
static int vidp_read_one(VIDP_Reader *reader)
{
	const VID_Api *api = reader->api;
	if (reader->qcount >= VIDP_VQ_SLOTS) return 0;

	AVPacket *slot = reader->queue[(reader->qhead + reader->qcount) %
		VIDP_VQ_SLOTS];
	int read = api->av_read_frame(reader->format, slot);
	if (read < 0) {
		reader->eof = true;
		// The tail of the clip is the last thing heard before the loop point.
		// Ramping it here is exact -- the samples are still in the ring and
		// their position in it is known -- where a fade applied later, from
		// the callback, could only guess.
		if (!reader->faded_out) {
			VIDA_endOfStream();
			reader->faded_out = true;
		}
		return -1;
	}
	if (slot->stream_index == reader->video_index) {
		reader->qcount++;   // stays in the slot; nothing is copied
		return 1;
	}
	if (slot->stream_index == reader->audio_index) {
		vidp_audio_packet(reader, slot);
	}
	api->av_packet_unref(slot);
	return 0;
}

// Read ahead until the ring is topped up or the video queue is full.
static void vidp_pump(VIDP_Reader *reader)
{
	if (reader->audio_index < 0) return;

	while (!reader->eof && reader->qcount < VIDP_VQ_SLOTS &&
		VIDA_queuedFrames() < reader->high_water) {
		if (vidp_read_one(reader) < 0) break;
	}
}

// The next video packet, or a negative code when the clip is exhausted.
static int vidp_next_video(VIDP_Reader *reader, AVPacket **out)
{
	const VID_Api *api = reader->api;

	if (reader->audio_index >= 0) {
		vidp_pump(reader);
		// A full ring is not the end of the clip. When the read-ahead stops on
		// the water mark before any video packet has turned up -- an
		// audio-heavy stretch of the interleave, or a clip whose audio outlasts
		// its picture -- keep reading until one does, or this reports end of
		// file and the clip restarts in the middle.
		while (reader->qcount == 0 && !reader->eof) {
			if (vidp_read_one(reader) < 0) break;
		}
		if (reader->qcount > 0) {
			*out = reader->queue[reader->qhead];
			reader->qhead = (reader->qhead + 1) % VIDP_VQ_SLOTS;
			reader->qcount--;
			return 0;
		}
		return AVERROR_EOF;
	}

	// Silent path, unchanged: read until a video packet turns up.
	for (;;) {
		int read = api->av_read_frame(reader->format, reader->scratch);
		if (read < 0) return read;
		if (reader->scratch->stream_index == reader->video_index) {
			*out = reader->scratch;
			return 0;
		}
		api->av_packet_unref(reader->scratch);
	}
}

// Fill the ring before the device is unpaused.
static void vidp_audio_prime(VIDP_Reader *reader, unsigned generation)
{
	while (!vidp_superseded(generation) && !reader->eof &&
		VIDA_queuedFrames() < reader->prime) {
		int before = VIDA_queuedFrames();
		vidp_pump(reader);
		// The video queue filling first, or the clip's audio ending early,
		// both look like this. Neither is a reason to spin.
		if (VIDA_queuedFrames() <= before) break;
	}
	VIDA_start();
}

// Back to the top of the clip. The format context has already been seeked.
static void vidp_reader_rewind(VIDP_Reader *reader, unsigned generation)
{
	const VID_Api *api = reader->api;
	for (int i = 0; i < reader->qcount; i++) {
		api->av_packet_unref(reader->queue[(reader->qhead + i) %
			VIDP_VQ_SLOTS]);
	}
	reader->qhead = 0;
	reader->qcount = 0;
	reader->eof = false;
	reader->faded_out = false;
	if (reader->audio_index < 0) return;

	// Both decoders flushed and the ring dropped, so the new pass starts
	// aligned instead of playing the old pass's tail over the new pass's first
	// frame. VIDA_reset also re-arms the fade-in and zeroes the clock, which is
	// what the video side's first_pts reset below is measured against.
	api->avcodec_flush_buffers(reader->audio_ctx);
	VIDA_reset();
	vidp_audio_prime(reader, generation);
}

// Everything the reader owns. VIDA_idle comes first because it waits out its
// own fade, and doing that after the decoder is gone would be the same wait
// with the picture already stopped.
//
// Idle, not close. The clip is over but the list is not: closing here would
// power the codec's whole output chain down and up again on every selection
// change, which is audible. The worker gives the device up when nothing else
// is coming -- see vidp_worker() -- and the sleep and shutdown paths take it
// away outright.
static void vidp_reader_release(VIDP_Reader *reader)
{
	const VID_Api *api = reader->api;
	if (reader->audio_index >= 0) VIDA_idle();
	for (int i = 0; i < VIDP_VQ_SLOTS; i++) {
		if (reader->queue[i]) api->av_packet_free(&reader->queue[i]);
	}
	if (reader->scratch) api->av_packet_free(&reader->scratch);
	if (reader->audio_frame) api->av_frame_free(&reader->audio_frame);
	if (reader->audio_ctx) api->avcodec_free_context(&reader->audio_ctx);
	free(reader->stage);
	reader->stage = NULL;
	reader->stage_frames = 0;
	reader->audio_index = -1;
}

// Opens the clip's audio, if it has any and the owner wants it.
//
// Every failure below returns false, which means "play the video silently".
// Audio must never take the video down with it: a preview with no sound is a
// preview, and a preview that refuses to start because of its soundtrack is a
// bug report.
static bool vidp_audio_open(VIDP_Reader *reader)
{
	const VID_Api *api = reader->api;
	AVCodecContext *ctx = NULL;
	AVFrame *frame = NULL;
	int index = -1;

	for (unsigned i = 0; i < reader->format->nb_streams; i++) {
		if (reader->format->streams[i]->codecpar->codec_type ==
			AVMEDIA_TYPE_AUDIO) {
			index = (int)i;
			break;
		}
	}
	// No audio stream at all. Not a failure and not worth its own line -- the
	// identity line already says audio=none.
	if (index < 0) return false;

	AVCodecParameters *params = reader->format->streams[index]->codecpar;
	const AVCodec *codec = api->avcodec_find_decoder(params->codec_id);
	if (!codec) {
		if (vidp_first_time(reader->path, VIDP_LOG_AUDIO_CODEC)) {
			LOG_info("video preview: %s has no decoder for audio codec %d; only AAC and MP3 are built in\n",
				reader->path, (int)params->codec_id);
		}
		return false;
	}

	ctx = api->avcodec_alloc_context3(codec);
	if (!ctx) return false;
	if (api->avcodec_parameters_to_context(ctx, params) < 0) goto fail;
	// One thread. Audio decode is a rounding error next to H.264, and a second
	// thread here would only add latency to a path whose whole job is to stay
	// ahead of the speaker.
	ctx->thread_count = 1;
	if (api->avcodec_open2(ctx, codec, NULL) < 0) {
		if (vidp_first_time(reader->path, VIDP_LOG_AUDIO_CODEC)) {
			LOG_info("video preview: %s could not open its %s audio decoder; playing silently\n",
				reader->path, codec->name);
		}
		goto fail;
	}
	// Checked here rather than on the first frame, because avcodec_open2 has
	// already settled what the decoder will emit and this is where there is
	// still a cheap way out.
	if (!rg_pcm_supported(ctx->sample_fmt) || ctx->sample_rate <= 0 ||
		ctx->ch_layout.nb_channels <= 0) {
		if (vidp_first_time(reader->path, VIDP_LOG_AUDIO_FORMAT)) {
			LOG_info("video preview: %s decodes audio as sample format %d at %d Hz on %d channels, which pcm.h cannot convert; playing silently\n",
				reader->path, ctx->sample_fmt, ctx->sample_rate,
				ctx->ch_layout.nb_channels);
		}
		goto fail;
	}

	frame = api->av_frame_alloc();
	if (!frame) goto fail;

	char reason[128] = "";
	if (!VIDA_open(ctx->sample_rate, reason, sizeof(reason))) {
		if (vidp_first_time(reader->path, VIDP_LOG_AUDIO_DEVICE)) {
			LOG_info("video preview: %s cannot have sound (%s); playing silently\n",
				reader->path, reason);
		}
		api->av_frame_free(&frame);
		goto fail;
	}

	for (int i = 0; i < VIDP_VQ_SLOTS; i++) {
		reader->queue[i] = api->av_packet_alloc();
		if (!reader->queue[i]) {
			VIDA_idle();
			api->av_frame_free(&frame);
			goto fail;
		}
	}

	reader->audio_index = index;
	reader->audio_ctx = ctx;
	reader->audio_frame = frame;
	reader->audio_codec_name = codec->name;
	reader->audio_channels = ctx->ch_layout.nb_channels;
	reader->high_water = (int)(((int64_t)ctx->sample_rate *
		VIDP_AUDIO_HIGH_WATER_MS) / 1000);
	reader->prime = (int)(((int64_t)ctx->sample_rate *
		VIDP_AUDIO_PRIME_MS) / 1000);
	return true;

fail:
	if (ctx) api->avcodec_free_context(&ctx);
	return false;
}

// Decode one clip to its end, then loop, until something supersedes it.
static void vidp_play(const char *path, unsigned generation) {
	const VID_Api *api = VID_api();
	AVFormatContext *format = NULL;
	AVCodecContext *codec_ctx = NULL;
	AVFrame *frame = NULL;
	int stream_index = -1;
	VIDP_Reader reader;
	memset(&reader, 0, sizeof(reader));
	reader.api = api;
	reader.path = path;
	reader.audio_index = -1;

	int opened = api->avformat_open_input(&format, path, NULL, NULL);
	if (opened < 0) {
		// Missing files are the common case -- most ROMs have no preview --
		// so this is not an error, and it is logged once. The decoded message
		// is what separates "no such file" from "this MP4 is truncated", which
		// are the same opaque negative integer without it.
		if (vidp_first_time(path, VIDP_LOG_OPEN_FAILURE)) {
			char reason[128];
			if (api->av_strerror(opened, reason, sizeof(reason)) < 0) {
				snprintf(reason, sizeof(reason), "error %d", opened);
			}
			LOG_info("video preview: cannot open %s: %s\n", path, reason);
		}
		return;
	}
	if (api->avformat_find_stream_info(format, NULL) < 0) {
		if (vidp_first_time(path, VIDP_LOG_OPEN_FAILURE)) {
			LOG_info("video preview: no stream info in %s\n", path);
		}
		goto done;
	}

	for (unsigned i = 0; i < format->nb_streams; i++) {
		if (format->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
			stream_index = (int)i;
			break;
		}
	}
	if (stream_index < 0) goto done;

	AVCodecParameters *params = format->streams[stream_index]->codecpar;
	long pixels = (long)params->width * params->height;
	if (params->width <= 0 || params->height <= 0 ||
		pixels > VIDP_MAX_SOURCE_PIXELS) {
		if (vidp_first_time(path, VIDP_LOG_OVERSIZE)) {
			// The path is in the message because the size alone does not say
			// which file to re-encode.
			LOG_info("video preview: %s is %dx%d (%ld pixels), beyond the %d-pixel memory bound; keeping the static image\n",
				path, params->width, params->height, pixels,
				VIDP_MAX_SOURCE_PIXELS);
		}
		goto done;
	}

	// Only H.264 is built into this ffmpeg (see workspace/h700/ffmpeg/build.sh).
	// A VP9 preview
	// resolves no decoder, which is a supported outcome rather than a bug.
	const AVCodec *codec = api->avcodec_find_decoder(params->codec_id);
	if (!codec) {
		if (vidp_first_time(path, VIDP_LOG_UNSUPPORTED_CODEC)) {
			// Named, because "some clip in this folder is VP9" is not a thing
			// anyone can act on.
			LOG_info("video preview: %s has no decoder for codec %d; only H.264 is built in\n",
				path, (int)params->codec_id);
		}
		goto done;
	}

	codec_ctx = api->avcodec_alloc_context3(codec);
	if (!codec_ctx) goto done;
	if (api->avcodec_parameters_to_context(codec_ctx, params) < 0) goto done;
	// One thread for the sizes packs usually ship, two above that. The device
	// has eight cores but the launcher is drawing on one of them, and frame
	// threading adds a frame of latency per thread between the request and the
	// first picture -- which is the part the owner actually perceives.
	codec_ctx->thread_count = pixels > VIDP_THREAD_PIXELS ? 2 : 1;
	// AV_CODEC_FLAG2_FAST is deliberately NOT set. Its single effect in this
	// decoder (h264_slice.c) is to stop deblocking across slice boundaries when
	// more than one slice context is active -- which is exactly the case above
	// the threading threshold. That trades visible edge artefacts for a saving
	// nobody here has measured, which is the wrong way round.
	if (api->avcodec_open2(codec_ctx, codec, NULL) < 0) goto done;

	reader.format = format;
	reader.video_index = stream_index;
	reader.scratch = api->av_packet_alloc();
	frame = api->av_frame_alloc();
	if (!reader.scratch || !frame) goto done;

	// Sound, if the clip has any and the owner wants it. Everything about the
	// video below is unchanged whether this succeeds or not; the only thing it
	// decides is which clock paces the picture.
	bool audio_on = CFG_getShowGameVideoAudio() && vidp_audio_open(&reader);
	const bool av_log = access(VIDP_AV_LOG_MARKER, F_OK) == 0;
	char audio_desc[64];
	if (audio_on) {
		snprintf(audio_desc, sizeof(audio_desc), "%s %dHz %dch",
			reader.audio_codec_name, reader.audio_ctx->sample_rate,
			reader.audio_channels);
	} else {
		snprintf(audio_desc, sizeof(audio_desc), "%s",
			CFG_getShowGameVideoAudio() ? "none" : "off");
	}
	if (audio_on) vidp_audio_prime(&reader, generation);

	AVRational time_base = format->streams[stream_index]->time_base;
	int64_t wall_start = vidp_now_ms();
	int64_t first_pts = AV_NOPTS_VALUE;
	int64_t last_published_ms = -VIDP_MIN_FRAME_INTERVAL_MS;
	int64_t last_av_log_ms = -1000;
	// How many frames in a row arrived after their presentation time. Reset by
	// any frame that arrives on time, so a single expensive keyframe does not
	// count as losing.
	int late_streak = 0;
	bool degraded = false;

	while (!vidp_superseded(generation)) {
		AVPacket *packet = NULL;
		int read = vidp_next_video(&reader, &packet);
		if (read < 0) {
			// End of clip: rewind and keep going. Looping is the chosen
			// behaviour: a clip only runs while the selection rests on it, so
			// the cost is bounded by the owner's attention.
			api->av_seek_frame(format, stream_index, 0, AVSEEK_FLAG_BACKWARD);
			api->avcodec_flush_buffers(codec_ctx);
			// Seeking on the video stream repositions the whole file, so the
			// audio stream follows; what it does not do is empty the ring or
			// the packet queue, which is what this is for.
			vidp_reader_rewind(&reader, generation);
			wall_start = vidp_now_ms();
			first_pts = AV_NOPTS_VALUE;
			continue;
		}

		int sent = api->avcodec_send_packet(codec_ctx, packet);
		api->av_packet_unref(packet);
		if (sent < 0 && sent != AVERROR(EAGAIN)) continue;

		while (!vidp_superseded(generation)) {
			int got = api->avcodec_receive_frame(codec_ctx, frame);
			if (got == AVERROR(EAGAIN) || got == AVERROR_EOF) break;
			if (got < 0) break;

			// No swscale in this build, so the decoder's own layout has to be
			// something SDL can take as a planar YUV texture. H.264 is
			// effectively always one of these two.
			if ((frame->format != AV_PIX_FMT_YUV420P &&
				 frame->format != AV_PIX_FMT_YUVJ420P) ||
				frame->linesize[0] <= 0 || frame->linesize[1] <= 0 ||
				frame->linesize[2] <= 0) {
				if (vidp_first_time(path, VIDP_LOG_PIXEL_FORMAT)) {
					// A negative stride means a flipped frame, which the upload
					// would read backwards off the start of the buffer.
					LOG_info("video preview: %s has pixel format %d and strides %d/%d/%d, which this cannot upload; no converter is built in\n",
						path, frame->format, frame->linesize[0],
						frame->linesize[1], frame->linesize[2]);
				}
				api->av_frame_unref(frame);
				goto done;
			}

			// One identifying line per clip. This is the line that says a
			// preview opened at all and how its planes are being read, so it
			// is also the line whose absence means something went wrong
			// earlier -- which only works if every clip gets one.
			if (vidp_first_time(path, VIDP_LOG_IDENTITY)) {
				const bool differs = vidp_first_format >= 0 &&
					(frame->format != vidp_first_format ||
					 frame->color_range != vidp_first_range ||
					 frame->colorspace != vidp_first_space);
				if (vidp_first_format < 0) {
					vidp_first_format = frame->format;
					vidp_first_range = frame->color_range;
					vidp_first_space = frame->colorspace;
				}
				LOG_info("video preview: %s is %dx%d format=%d range=%d colourspace=%d audio=%s%s\n",
					path, frame->width, frame->height, frame->format,
					frame->color_range, frame->colorspace, audio_desc,
					differs ? " -- differs from the first clip this session" : "");
			}

			int64_t pts = frame->best_effort_timestamp;
			if (pts == AV_NOPTS_VALUE) pts = frame->pts;
			// This frame's own moment, relative to the start of the pass.
			// Declared out here so the diagnostic below can name it; a clip
			// with no timestamps at all leaves it at zero, which is also what
			// the pacing below would have made of it.
			int64_t offset_ms = 0;
			if (pts != AV_NOPTS_VALUE) {
				if (first_pts == AV_NOPTS_VALUE) first_pts = pts;
				offset_ms =
					(int64_t)((pts - first_pts) * av_q2d(time_base) * 1000.0);
				// Two clocks, chosen once when the clip opened.
				//
				// With sound, the master is what the device has actually
				// played. Pacing the picture against wall time instead would
				// let the two drift apart over a loop even though they started
				// together, because nothing would ever correct the difference
				// between "33 ms elapsed" and "33 ms of audio came out".
				//
				// Without sound this is the wall-clock expression the silent
				// build shipped with, rearranged only by moving wall_start to
				// the other side.
				int64_t elapsed = audio_on ? VIDA_clockMs()
					: vidp_now_ms() - wall_start;
				int64_t behind = elapsed - offset_ms;
				if (behind <= 0) {
					// Ahead of the frame's moment: wait for it. This is the
					// normal path and the one that keeps playback at real speed.
					if (audio_on) {
						// Sliced, with a read between the slices. An
						// uninterrupted sleep here is that many milliseconds
						// in which the ring only drains, and the ring is the
						// only thing between a busy main thread and a gap in
						// the sound.
						int64_t remaining = -behind;
						while (remaining > 0 && !vidp_superseded(generation)) {
							vidp_sleep_ms(remaining < VIDP_AUDIO_SLICE_MS
								? remaining : VIDP_AUDIO_SLICE_MS);
							vidp_pump(&reader);
							remaining = offset_ms - VIDA_clockMs();
						}
					} else {
						vidp_sleep_ms(-behind);
					}
					late_streak = 0;
				} else if (behind > VIDP_LATE_MS) {
					late_streak++;
				} else {
					late_streak = 0;
				}
			}

			// Losing, and not yet doing anything about it: stop decoding frames
			// nothing else refers to. Roughly a third to a half of the work in a
			// typical encode, and the only lever here that reduces decoding
			// rather than display.
			//
			// This is the whole response. There is deliberately no "give up and
			// go back to the cover image" step: it would need the draw path to
			// learn that the decoder had stopped, and until it did the panel
			// would sit on a frozen frame -- a worse outcome than the choppy
			// playback it was meant to avoid. A clip only runs while the
			// selection rests on it, so the cost is bounded by attention, and
			// CFG_getShowGameVideo turns the feature off entirely.
			if (!degraded && late_streak >= VIDP_LATE_FRAMES_BEFORE_DEGRADE) {
				degraded = true;
				codec_ctx->skip_frame = AVDISCARD_NONREF;
				if (vidp_first_time(path, VIDP_LOG_DEGRADED)) {
					LOG_info("video preview: %s (%dx%d) decodes slower than it plays; dropping non-reference frames\n",
						path, params->width, params->height);
				}
			}

			// Said once per clip, on the first gap. A count that keeps rising
			// is the symptom the CPU note in VIDEO-PREVIEW-AUDIO.md section 8
			// exists for; a single one at the start of a clip is not.
			if (audio_on && VIDA_underruns() > 0 &&
				vidp_first_time(path, VIDP_LOG_AUDIO_UNDERRUN)) {
				LOG_info("video preview: %s ran the audio device dry; decode is not staying ahead of playback\n",
					path);
			}

			int64_t now = vidp_now_ms();
			if (audio_on && av_log && now - last_av_log_ms >= 1000) {
				last_av_log_ms = now;
				int64_t audio_ms = VIDA_clockMs();
				LOG_info("video preview av: wall=%lldms audio=%lldms video=%lldms drift=%lldms ring=%dms underruns=%d\n",
					(long long)(now - wall_start), (long long)audio_ms,
					(long long)offset_ms, (long long)(offset_ms - audio_ms),
					(int)(((int64_t)VIDA_queuedFrames() * 1000) /
						reader.audio_ctx->sample_rate),
					VIDA_underruns());
			}
			if (now - last_published_ms >= VIDP_MIN_FRAME_INTERVAL_MS) {
				last_published_ms = now;
				if (!vidp_publish(&frame, generation)) goto done;
			} else {
				api->av_frame_unref(frame);
			}
		}
	}

done:
	vidp_reader_release(&reader);
	if (frame) api->av_frame_free(&frame);
	if (codec_ctx) api->avcodec_free_context(&codec_ctx);
	if (format) api->avformat_close_input(&format);
}

static void *vidp_worker(void *unused) {
	(void)unused;
	for (;;) {
		pthread_mutex_lock(&vidp_mutex);
		vidp_worker_idle = true;
		pthread_cond_broadcast(&vidp_idle_cond);
		// The device is held across the gap between two clips and given back
		// when the gap turns out not to be a gap. The wait is timed only while
		// there is something to give back, so a frontend that has never played
		// a preview parks on an untimed wait exactly as it always did.
		bool holding = VIDA_isOpen();
		// One deadline for the whole wait, not one per wakeup: recomputing it
		// would let a run of spurious wakeups postpone the close forever.
		struct timespec deadline;
		if (holding) {
			clock_gettime(CLOCK_REALTIME, &deadline);
			deadline.tv_nsec += (long)(VIDP_AUDIO_LINGER_MS % 1000) * 1000000L;
			deadline.tv_sec += VIDP_AUDIO_LINGER_MS / 1000 +
				deadline.tv_nsec / 1000000000L;
			deadline.tv_nsec %= 1000000000L;
		}
		while (!vidp_request_pending && !vidp_shutdown) {
			if (!holding) {
				pthread_cond_wait(&vidp_cond, &vidp_mutex);
				continue;
			}
			if (pthread_cond_timedwait(&vidp_cond, &vidp_mutex, &deadline) !=
				ETIMEDOUT) {
				continue;
			}
			if (vidp_request_pending || vidp_shutdown) continue;
			// Not idle while the close runs, so VIDP_stopAndWait() cannot
			// return in the middle of it and close the same device again.
			vidp_worker_idle = false;
			pthread_mutex_unlock(&vidp_mutex);
			VIDA_close();
			pthread_mutex_lock(&vidp_mutex);
			holding = false;
			vidp_worker_idle = true;
			pthread_cond_broadcast(&vidp_idle_cond);
		}
		if (vidp_shutdown) {
			pthread_mutex_unlock(&vidp_mutex);
			break;
		}
		char path[sizeof(vidp_request_path)];
		snprintf(path, sizeof(path), "%s", vidp_request_path);
		unsigned generation = vidp_generation;
		vidp_request_pending = false;
		vidp_worker_idle = false;
		pthread_mutex_unlock(&vidp_mutex);

		// Whatever was on screen belongs to the previous clip.
		vidp_clear_published();

		if (path[0] == '\0') continue;

		// The dlopen happens here, on this thread, the first time a preview is
		// genuinely wanted -- never on the path to the first frame.
		if (!VID_available()) {
			if (!vidp_logged_unavailable) {
				vidp_logged_unavailable = true;
				LOG_info("video preview: unavailable (%s); static art only\n",
					VID_unavailableReason());
			}
			continue;
		}

		vidp_play(path, generation);
		vidp_clear_published();
	}
	vidp_clear_published();
	VIDA_close();
	return NULL;
}

static void vidp_start_thread(void) {
	// pthread_create reports through its return value, not errno.
	int rc = pthread_create(&vidp_thread, NULL, vidp_worker, NULL);
	if (rc != 0) {
		LOG_error("video preview: cannot start the decode thread: %s\n",
			strerror(rc));
		return;
	}
	vidp_thread_started = true;
}

void VIDP_request(const char *path) {
	if (!path || path[0] == '\0') {
		VIDP_stop();
		return;
	}
	pthread_once(&vidp_thread_once, vidp_start_thread);
	if (!vidp_thread_started) return;

	pthread_mutex_lock(&vidp_mutex);
	snprintf(vidp_request_path, sizeof(vidp_request_path), "%s", path);
	vidp_request_pending = true;
	vidp_generation++;
	vidp_has_frame = false;
	pthread_cond_signal(&vidp_cond);
	pthread_mutex_unlock(&vidp_mutex);
}

void VIDP_stop(void) {
	if (!vidp_thread_started) return;
	pthread_mutex_lock(&vidp_mutex);
	vidp_request_path[0] = '\0';
	vidp_request_pending = true;
	vidp_generation++;
	vidp_has_frame = false;
	pthread_cond_signal(&vidp_cond);
	pthread_mutex_unlock(&vidp_mutex);
}

bool VIDP_lockFrame(VIDP_Frame *out) {
	if (!vidp_thread_started || !out) return false;
	pthread_mutex_lock(&vidp_mutex);
	if (!vidp_has_frame || !vidp_published) {
		pthread_mutex_unlock(&vidp_mutex);
		return false;
	}
	out->y = vidp_published->data[0];
	out->u = vidp_published->data[1];
	out->v = vidp_published->data[2];
	out->ystride = vidp_published->linesize[0];
	out->ustride = vidp_published->linesize[1];
	out->vstride = vidp_published->linesize[2];
	out->width = vidp_published->width;
	out->height = vidp_published->height;
	out->yuv = vidp_yuv_mode(vidp_published);
	out->generation = vidp_published_generation;
	// Returns holding the lock. The decoder waits until VIDP_unlockFrame().
	return true;
}

void VIDP_unlockFrame(void) {
	pthread_mutex_unlock(&vidp_mutex);
}

void VIDP_stopAndWait(void) {
	if (!vidp_thread_started) return;
	VIDP_stop();

	// Sleep is why this exists. PWR_enterSleep() closes NextUI's own audio
	// device before suspending, with a comment saying a PCM left open across
	// suspend-to-RAM comes back in a state SDL takes about ten seconds to
	// close -- freezing the UI on wake. That device is minarch's and is never
	// open here, but a preview's device is, and it would be left open in
	// exactly the same way. An asynchronous VIDP_stop() would not do: the main
	// thread would suspend the machine while the worker was still closing.
	pthread_mutex_lock(&vidp_mutex);
	struct timespec deadline;
	clock_gettime(CLOCK_REALTIME, &deadline);
	deadline.tv_sec += 1;
	// Bounded, because failing to sleep is worse than sleeping with the device
	// open. A worker wedged for a second has a problem this cannot fix.
	while (!vidp_worker_idle && !vidp_shutdown) {
		if (pthread_cond_timedwait(&vidp_idle_cond, &vidp_mutex, &deadline) ==
			ETIMEDOUT) {
			LOG_info("video preview: worker did not go idle before sleep\n");
			break;
		}
	}
	pthread_mutex_unlock(&vidp_mutex);

	// The worker is parked, so nothing else touches the device and closing it
	// from here is safe. It has to be closed rather than left idle: a PCM held
	// across suspend-to-RAM is exactly what PWR_enterSleep() closes its own
	// device to avoid, and the linger above would not have expired yet.
	VIDA_close();
}

void VIDP_shutdown(void) {
	if (!vidp_thread_started) return;
	pthread_mutex_lock(&vidp_mutex);
	vidp_shutdown = true;
	pthread_cond_signal(&vidp_cond);
	pthread_mutex_unlock(&vidp_mutex);
	pthread_join(vidp_thread, NULL);
	vidp_thread_started = false;
}
