#!/bin/sh
# Trimmed ffmpeg for the game-art video previews.
#
# These libraries are NEVER linked into nextui.elf. They are dlopen()ed by
# workspace/all/common/video/vidload.c the first time a preview is actually
# wanted, which is what makes the feature cost exactly zero before the first
# frame: there is no DT_NEEDED entry for ld.so to resolve at process start, and
# a device whose owner never rests on a ROM with a .videos/ clip never maps
# libavcodec at all.
#
# Only the headers are used at compile time. The nextui makefile adds
#   -I<this dir>/output/include
# and nothing is ever added to a link line.
#
# The video half of this configuration mirrors the reference implementation's
# Buildroot defconfig (rg-rotate-os configs/rg_rotate_m5_1_defconfig:106-131);
# the audio half has no reference, because that device had no working sound.
#   decoder  h264, aac, mp3  -- what scrapers emit
#   demuxer  mov             -- covers mov, mp4, m4a, 3gp
#   parser   h264, aac, mpegaudio
#   bsf      extract_extradata, h264_mp4toannexb, null
#            libavformat genuinely needs extract_extradata for H.264 in mp4;
#            omitting it is a silent "no frames" rather than a build error.
#   protocol file
# and disables everything else.  --disable-autodetect is the option that
# matters most: without it the build picks up zlib, lzma and iconv from the
# toolchain sysroot and libavcodec grows a dependency list it does not need.
#
# GPL stays off.  The H.264 *decoder* is LGPL-2.1; only encoders such as x264
# need GPL, and no encoder is built here.  LGPL also suits dynamic loading.
#
# Audio: AAC and MP3 decoders plus their parsers.  AAC is what every scraper
# pack carries in its MP4s; MP3 costs a few kilobytes and covers the rest.  The
# aac and mpegaudio parsers are enabled because a decoder without its parser is
# a silent "no audio", not a build error.
#
# swresample stays OFF.  Planar float -> interleaved S16 is a clamp, a scale and
# an interleave, written in workspace/all/common/video/pcm.h and unit tested;
# and rate conversion, when the device wants a rate the clip does not have, is
# done by SDL itself (SDL_OpenAudioDevice with allowed_changes=0 builds the
# converting stream internally).  Neither libswresample nor libsamplerate is
# needed, and libsamplerate in particular would have to be *linked*, which would
# put an entry in nextui.elf's DT_NEEDED and cost boot time on every boot.
#
# swscale is absent too.  Colour conversion is done on the CPU in
# workspace/all/common/video/yuv2rgb.h, straight into the streaming texture.
set -eu

: "${CROSS_COMPILE:?CROSS_COMPILE is required}"

FFMPEG_VERSION=6.1.2
# Pinned from the tarball fetched over TLS from ffmpeg.org.  Same trust model
# as workspace/h700/curl/build.sh, which pins curl, openssl and musl the same
# way.  ffmpeg.org publishes a detached GPG signature but no checksum file.
FFMPEG_SHA256=3b624649725ecdc565c903ca6643d41f33bd49239922e45c9b1442c63dca4e38

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname "$0")" && pwd)
ARCHIVE="$SCRIPT_DIR/ffmpeg-$FFMPEG_VERSION.tar.xz"
SOURCE_DIR="$SCRIPT_DIR/ffmpeg-$FFMPEG_VERSION"
OUTPUT_DIR="$SCRIPT_DIR/output"
# The stamp carries a hash of this script, not just the tarball identity.  Its
# earlier form keyed only on version and checksum, so editing the configure
# options below left a stale build in place and the next `make` silently shipped
# libraries that did not match the source it was told to build.  That is exactly
# how the AAC decoders would have failed to appear.
BUILD_HASH=$(sha256sum "$0" | cut -c1-16)
STAMP="$OUTPUT_DIR/.ffmpeg-$FFMPEG_VERSION-$FFMPEG_SHA256-$BUILD_HASH"

# Already built: this runs from `make early`, so it must be cheap on a rebuild.
if [ -f "$STAMP" ] && [ -f "$OUTPUT_DIR/include/libavcodec/avcodec.h" ]; then
	exit 0
fi

mkdir -p "$OUTPUT_DIR"
rm -f "$OUTPUT_DIR"/.ffmpeg-*
if [ ! -f "$ARCHIVE" ]; then
	wget -q "https://ffmpeg.org/releases/ffmpeg-$FFMPEG_VERSION.tar.xz" -O "$ARCHIVE.tmp"
	mv "$ARCHIVE.tmp" "$ARCHIVE"
fi
if ! echo "$FFMPEG_SHA256  $ARCHIVE" | sha256sum -c -; then
	rm -f "$ARCHIVE"
	exit 1
fi

rm -rf "$SOURCE_DIR"
tar -xJf "$ARCHIVE" -C "$SCRIPT_DIR"
cd "$SOURCE_DIR"

# -mcpu=cortex-a53 matches the H700's four A53 cores.  The project's own
# -Ofast -flto is deliberately NOT reused: this is third-party C that expects
# strict IEEE semantics, and LTO across a shared library gains nothing here.
./configure \
	--prefix="$OUTPUT_DIR" \
	--enable-cross-compile \
	--cross-prefix="$CROSS_COMPILE" \
	--arch=aarch64 \
	--target-os=linux \
	--enable-shared \
	--disable-static \
	--enable-pic \
	--disable-everything \
	--enable-decoder=h264 \
	--enable-decoder=aac \
	--enable-decoder=mp3 \
	--enable-demuxer=mov \
	--enable-parser=h264 \
	--enable-parser=aac \
	--enable-parser=mpegaudio \
	--enable-bsf=extract_extradata \
	--enable-bsf=h264_mp4toannexb \
	--enable-bsf=null \
	--enable-protocol=file \
	--disable-programs \
	--disable-doc \
	--disable-autodetect \
	--disable-avdevice \
	--disable-avfilter \
	--disable-swscale \
	--disable-swresample \
	--disable-postproc \
	--disable-network \
	--disable-hwaccels \
	--disable-libdrm \
	--disable-debug \
	--extra-cflags="-O3 -mcpu=cortex-a53"

make -j"$(nproc)"
make install

# Stripped: these ship on the card and nothing debugs them there.
"${CROSS_COMPILE}strip" --strip-unneeded "$OUTPUT_DIR"/lib/libav*.so.*.*.* 2>/dev/null || true
find "$OUTPUT_DIR/lib" -name 'libav*.so*' -type f -exec "${CROSS_COMPILE}strip" --strip-unneeded {} + 2>/dev/null || true

# Static archives and pkg-config files are build-host clutter; nothing links
# these and shipping them would only invite someone to try.
rm -rf "$OUTPUT_DIR/lib/pkgconfig" "$OUTPUT_DIR/share"

touch "$STAMP"
