#include "vidload.h"

#include <dlfcn.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

// defines.h first: api.h uses BTN_ID_COUNT and the other platform
// enums without including the header that declares them, so including
// it on its own does not compile.
#include "defines.h"
#include "api.h"

// Sonames are derived from the headers this was compiled against rather than
// written out, so a version bump that moves libavcodec from .so.60 to .so.61
// follows automatically instead of turning into a runtime failure nobody sees
// until a preview is attempted. Verified against the libraries shipped in
// .system/h700/lib: libavformat.so.60, libavcodec.so.60, libavutil.so.58.
//
// The card is FAT, which cannot hold symlinks, so those files are the real
// libraries rather than links to a fully-versioned name. dlopen by soname is
// what makes that work without any extra plumbing.
#define VID_STRINGIFY_(x) #x
#define VID_STRINGIFY(x) VID_STRINGIFY_(x)

#define VID_LIB_FORMAT "libavformat.so." VID_STRINGIFY(LIBAVFORMAT_VERSION_MAJOR)
#define VID_LIB_CODEC "libavcodec.so." VID_STRINGIFY(LIBAVCODEC_VERSION_MAJOR)
#define VID_LIB_UTIL "libavutil.so." VID_STRINGIFY(LIBAVUTIL_VERSION_MAJOR)

static pthread_once_t vid_once = PTHREAD_ONCE_INIT;
static VID_Api vid_api;
static bool vid_ok = false;
static char vid_reason[192] = "";

static int64_t vid_now_ms(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool vid_resolve(void *handle, const char *name, void **slot) {
	// dlerror() is cleared first because a symbol may legitimately resolve to
	// NULL; the error state is the only reliable signal.
	dlerror();
	void *symbol = dlsym(handle, name);
	if (dlerror() != NULL) return false;
	*slot = symbol;
	return true;
}

// A resolved symbol proves the library is present; it does not prove the
// library matches the headers whose struct offsets this code was compiled
// against. Nothing would fail loudly in that case -- AVFrame->data would simply
// be read at the wrong offset. The major version is the ABI promise, so it is
// checked before any struct is touched.
static bool vid_abi_matches(unsigned runtime, unsigned compiled,
	const char *lib) {
	if ((runtime >> 16) == (compiled >> 16)) return true;
	snprintf(vid_reason, sizeof(vid_reason),
		"%s ABI is %u.x but this was built against %u.x", lib,
		runtime >> 16, compiled >> 16);
	return false;
}

static void vid_load(void) {
	int64_t started = vid_now_ms();

	void *handle_FORMAT = NULL;
	void *handle_CODEC = NULL;
	void *handle_UTIL = NULL;
	const char *missing = NULL;

	// RTLD_NOW so an incomplete library fails here rather than at the first
	// call from the decode loop, where the failure would be a crash in a
	// worker thread instead of a message.
	handle_UTIL = dlopen(VID_LIB_UTIL, RTLD_NOW | RTLD_LOCAL);
	handle_CODEC = dlopen(VID_LIB_CODEC, RTLD_NOW | RTLD_LOCAL);
	handle_FORMAT = dlopen(VID_LIB_FORMAT, RTLD_NOW | RTLD_LOCAL);
	if (!handle_UTIL || !handle_CODEC || !handle_FORMAT) {
		// Read once: dlerror() clears the error state, so calling it twice in
		// one expression reports "(null)" for every real failure.
		const char *error = dlerror();
		snprintf(vid_reason, sizeof(vid_reason), "dlopen failed: %s",
			error ? error : "unknown error");
		goto fail;
	}

#define VID_RESOLVE_MEMBER(lib, ret, name, params)                            \
	if (!vid_resolve(handle_##lib, #name, (void **)&vid_api.name)) {          \
		missing = #name;                                                      \
		goto fail;                                                            \
	}
	VID_SYMBOLS(VID_RESOLVE_MEMBER)
#undef VID_RESOLVE_MEMBER

	if (!vid_abi_matches(vid_api.avutil_version(), LIBAVUTIL_VERSION_INT,
			"libavutil") ||
		!vid_abi_matches(vid_api.avcodec_version(), LIBAVCODEC_VERSION_INT,
			"libavcodec") ||
		!vid_abi_matches(vid_api.avformat_version(), LIBAVFORMAT_VERSION_INT,
			"libavformat")) {
		goto fail;
	}

	// ffmpeg is chatty on stderr by default and the launcher's stderr is the
	// boot log. Errors only; the decoder reports its own failures with context
	// this log line would not have.
	vid_api.av_log_set_level(AV_LOG_ERROR);

	vid_ok = true;
	// The marker a boot that never browses must not contain. See
	// VIDEO-PREVIEW-PORT.md section 4: its absence is the evidence that nothing
	// on the path to the first frame touched ffmpeg.
	LOG_info("nextui-video-dlopen=ok elapsed_ms=%lld avformat=%u.%u avcodec=%u.%u avutil=%u.%u\n",
		(long long)(vid_now_ms() - started),
		vid_api.avformat_version() >> 16,
		(vid_api.avformat_version() >> 8) & 0xff,
		vid_api.avcodec_version() >> 16,
		(vid_api.avcodec_version() >> 8) & 0xff,
		vid_api.avutil_version() >> 16,
		(vid_api.avutil_version() >> 8) & 0xff);
	return;

fail:
	if (missing) {
		snprintf(vid_reason, sizeof(vid_reason),
			"symbol not found: %s", missing);
	} else if (vid_reason[0] == '\0') {
		snprintf(vid_reason, sizeof(vid_reason), "unknown load failure");
	}
	// Handles are deliberately not closed. A partially resolved table is never
	// used (vid_ok stays false), and dlclose on a library another thread may be
	// inside is a worse failure than a few megabytes of address space that the
	// process holds until it exits anyway.
	vid_ok = false;
	LOG_info("nextui-video-dlopen=failed reason=\"%s\" elapsed_ms=%lld\n",
		vid_reason, (long long)(vid_now_ms() - started));
}

bool VID_available(void) {
	pthread_once(&vid_once, vid_load);
	return vid_ok;
}

const VID_Api *VID_api(void) {
	return VID_available() ? &vid_api : NULL;
}

const char *VID_unavailableReason(void) {
	return vid_reason;
}
