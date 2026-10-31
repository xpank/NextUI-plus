// The video-preview decoder: one background thread, one clip at a time.
//
// Shape follows nextui.c's thumbLoadThread -- a worker, a mutex, a condition
// variable, and a flag the draw path reads -- with one deliberate difference.
// Thumbnails use a bounded queue because several can be worth loading at once.
// Only one preview is ever visible, so this holds a single request slot where
// the latest submission wins. A queue here would only preserve requests that
// are already stale by the time they run.
//
// Nothing in this file runs before the first frame. The worker thread is
// created by the first VIDP_request() call, and VID_available() -- the dlopen
// -- happens on that thread, so a device whose owner never rests on a ROM never
// creates the thread and never maps libavcodec.

#ifndef NEXTUI_VIDPLAYER_H
#define NEXTUI_VIDPLAYER_H

#include <stdbool.h>
#include <stdint.h>

// How the three planes are meant to be interpreted.
//
// Y'CbCr is not one encoding. The same bytes mean different colours depending
// on whether the range is limited (16-235) or full (0-255), and on which matrix
// the encoder used. Guessing wrong does not corrupt the picture, it tints it --
// which is why it is easy to ship and easy to misread as a decoder bug.
//
// Taken from the clip's own metadata; never assumed.
typedef enum {
	VIDP_YUV_BT601,  // limited range, standard definition -- the usual case
	VIDP_YUV_BT709,  // limited range, high definition
	VIDP_YUV_FULL,   // full range, what yuvj420p and AVCOL_RANGE_JPEG carry
} VIDP_YuvMode;

// A decoded frame, referenced in place. The planes point into the decoder's own
// AVFrame: there is no copy anywhere between libavcodec and the GPU upload,
// which is why the caller has to hold the lock while it reads them.
typedef struct VIDP_Frame {
	const uint8_t *y;
	const uint8_t *u;
	const uint8_t *v;
	int ystride;
	int ustride;
	int vstride;
	int width;
	int height;
	VIDP_YuvMode yuv;
	// Bumped for every newly opened clip. The presenter keeps one streaming
	// texture per clip and recreates it when this changes, which is also how it
	// learns the dimensions changed.
	unsigned generation;
} VIDP_Frame;

// Ask for a clip. Returns immediately; decoding happens on the worker. Replaces
// any clip currently playing or opening. Safe to call on every selection
// change.
void VIDP_request(const char *path);

// Stop playing and release the clip. Idempotent, and safe to call when nothing
// is playing.
void VIDP_stop(void);

// Take the current frame, holding the decoder off until VIDP_unlockFrame().
// Returns false -- without locking -- when no frame is ready, which is the
// normal case for the first fraction of a second after a request and the
// permanent case when the clip could not be opened.
//
// The caller must do as little as possible between these two calls: the decoder
// blocks for exactly that long. Uploading to a texture is the intended amount
// of work.
bool VIDP_lockFrame(VIDP_Frame *out);
void VIDP_unlockFrame(void);

// Stop, and do not return until the worker has actually let go of the clip and
// of the audio device with it.
//
// The asynchronous VIDP_stop() above is right everywhere the app keeps running
// -- the selection moved, the list was left -- because a few tens of
// milliseconds of teardown on a background thread are invisible. It is wrong on
// the way into suspend-to-RAM: PWR_enterSleep() deliberately closes NextUI's
// own audio device first, because a PCM left open across suspend comes back in
// a state SDL takes about ten seconds to close, and a preview's device has to
// be closed for the same reason and before the same moment.
void VIDP_stopAndWait(void);

// Stop the worker and join it. Called from the launcher's shutdown path.
void VIDP_shutdown(void);

#endif
