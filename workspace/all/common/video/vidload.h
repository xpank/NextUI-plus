// Runtime loader for the ffmpeg libraries used by video previews.
//
// The launcher never links ffmpeg. Every symbol below is resolved with dlsym
// the first time a preview is actually wanted, which is what makes the feature
// cost exactly zero before the first frame -- there is no DT_NEEDED entry for
// the dynamic loader to resolve at process start, and a device whose owner
// never rests on a ROM never maps libavcodec at all.
//
// That guarantee is checked mechanically, not trusted:
//   workspace/h700/check-video-preview.sh
//       asserts the built nextui.elf's DT_NEEDED contains no libav*, that no
//       -lav* reaches a link line, and that this symbol table and the decoder
//       that uses it cannot drift apart.
//
// The single VID_SYMBOLS list below is the reason that last check is possible:
// the struct and the dlsym calls are both generated from it, so the table
// cannot disagree with itself, and the test only has to compare it against the
// call sites.
//
// Headers are used normally even though the code is not linked. Struct layouts
// therefore have to match at run time, which dlsym cannot check -- a wrong
// libavcodec would not fail to resolve, it would read the wrong offsets. Hence
// the major-version check in VID_available(): the headers come from the same
// pinned tarball that workspace/h700/ffmpeg/build.sh builds the libraries from,
// and the loader refuses anything whose major version disagrees with what was
// compiled against.

#ifndef NEXTUI_VIDLOAD_H
#define NEXTUI_VIDLOAD_H

#include <stdbool.h>

#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/frame.h>

// lib, return type, symbol, parameter list.
//
// The lib field selects which handle the symbol is resolved from and must be
// one of FORMAT, CODEC or UTIL.
#define VID_SYMBOLS(X)                                                        \
	X(FORMAT, int, avformat_open_input,                                       \
		(AVFormatContext **ps, const char *url,                               \
		 const AVInputFormat *fmt, AVDictionary **options))                   \
	X(FORMAT, int, avformat_find_stream_info,                                 \
		(AVFormatContext *ic, AVDictionary **options))                        \
	X(FORMAT, void, avformat_close_input, (AVFormatContext **s))              \
	X(FORMAT, int, av_read_frame, (AVFormatContext *s, AVPacket *pkt))        \
	X(FORMAT, int, av_seek_frame,                                             \
		(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)) \
	X(FORMAT, unsigned, avformat_version, (void))                             \
	X(CODEC, const AVCodec *, avcodec_find_decoder, (enum AVCodecID id))      \
	X(CODEC, AVCodecContext *, avcodec_alloc_context3, (const AVCodec *codec))\
	X(CODEC, int, avcodec_parameters_to_context,                              \
		(AVCodecContext *codec, const AVCodecParameters *par))                \
	X(CODEC, int, avcodec_open2,                                              \
		(AVCodecContext *avctx, const AVCodec *codec, AVDictionary **options))\
	X(CODEC, void, avcodec_free_context, (AVCodecContext **avctx))            \
	X(CODEC, int, avcodec_send_packet,                                        \
		(AVCodecContext *avctx, const AVPacket *avpkt))                       \
	X(CODEC, int, avcodec_receive_frame,                                      \
		(AVCodecContext *avctx, AVFrame *frame))                              \
	X(CODEC, void, avcodec_flush_buffers, (AVCodecContext *avctx))            \
	X(CODEC, AVPacket *, av_packet_alloc, (void))                             \
	X(CODEC, void, av_packet_free, (AVPacket **pkt))                          \
	X(CODEC, void, av_packet_unref, (AVPacket *pkt))                          \
	X(CODEC, unsigned, avcodec_version, (void))                               \
	X(UTIL, AVFrame *, av_frame_alloc, (void))                                \
	X(UTIL, void, av_frame_free, (AVFrame **frame))                           \
	X(UTIL, void, av_frame_unref, (AVFrame *frame))                           \
	X(UTIL, void, av_log_set_level, (int level))                              \
	X(UTIL, int, av_strerror, (int errnum, char *errbuf, size_t errbuf_size)) \
	X(UTIL, unsigned, avutil_version, (void))

#define VID_DECLARE_MEMBER(lib, ret, name, params) ret (*name) params;
typedef struct VID_Api {
	VID_SYMBOLS(VID_DECLARE_MEMBER)
} VID_Api;
#undef VID_DECLARE_MEMBER

// True once the three libraries are open and every symbol above resolved.
// Safe to call from any thread and from more than one; the work happens once.
// The first call is the expensive one -- it is a dlopen of libavcodec -- so it
// belongs on the preview worker thread and never on the path to a frame.
bool VID_available(void);

// The API table, or NULL when VID_available() is false. Never partially filled:
// a single missing symbol fails the whole load.
const VID_Api *VID_api(void);

// Why the load failed, for the once-per-session log. "" when it succeeded or
// has not been attempted.
const char *VID_unavailableReason(void);

#endif
