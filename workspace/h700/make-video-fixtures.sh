#!/usr/bin/env bash
# Builds the video-preview test fixtures, ready to push onto the card.
#
# The originals were made by hand on the device and existed only there, which
# is why the audio work had to regenerate all of them from nothing: none of the
# shipped clips had an audio stream at all. This script is the fix for that --
# the fixtures are now a thing that can be rebuilt rather than a thing that was
# once produced.
#
# Each clip is chosen for a property of the code, not for looking nice:
#
#   1 Wide 720x480     the size screenscraper actually emits; limited range,
#                      BT.470BG, AAC 48 kHz stereo -- the ordinary case
#   2 Tall 480x720     taller than the panel, so the max_h clamp branch in
#                      videoPreviewTick is the one that runs
#   3 Fullrange        yuvj420p / AVCOL_RANGE_JPEG, over a black-to-white
#                      gradient so a range error clips visibly at both ends
#   4 VP9 unsupported  no decoder is built for VP9; the clip must degrade to
#                      the static image, and must not open an audio device
#                      either even though it has a perfectly good AAC track
#   5 No Preview       no file at all: nothing is armed and ffmpeg is never
#                      dlopen()ed
#   6 No Audio         H.264 with no audio stream, which is the silent path --
#                      wall-clock pacing, exactly as the video-only build did
#   7 Mono 44k1        AAC, one channel, 44100 Hz. Two things at once: the
#                      mono duplication in pcm.h, and a rate the hardware
#                      probably does not want, so SDL has to convert
#   8 MP3 44k1         the MP3 decoder and the mpegaudio parser, which are
#                      enabled in build.sh and which nothing else here reaches
#   9 Odd rate         AAC at 11025 Hz. The H700 codec takes both 48000 and
#                      44100 straight, so nothing above ever makes SDL build
#                      its converting stream -- and that stream is the whole
#                      reason no resampler is linked. A rate the hardware is
#                      unlikely to want is the only way to exercise it
#
# Every clip with sound carries a 1 kHz beep for the first 100 ms of each
# second and a white bar across the top of the picture for exactly the same
# 100 ms. That is the sync test that needs no instruments: the flash and the
# beep either land together or they do not.
#
# Usage:
#     bash workspace/h700/make-video-fixtures.sh [OUTDIR]
#     adb push "$OUTDIR"/. "/mnt/sdcard/Roms/Game Boy (GB)/"
#
# Needs a host ffmpeg with libx264, libvpx-vp9, aac and libmp3lame. Not the
# cross-built one -- these are built on the workstation and copied over.
set -euo pipefail

out=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/video-fixtures}
rm -rf "$out"
mkdir -p "$out/.videos" "$out/.media"

for tool in ffmpeg ffprobe; do
	command -v "$tool" >/dev/null || { echo "need $tool on PATH" >&2; exit 1; }
done

DUR=6
# 100 ms out of every second, in both the picture and the sound.
FLASH="drawbox=x=0:y=0:w=iw:h=ih/10:color=white@1.0:t=fill:enable='lt(mod(t,1),0.1)'"
BEEP="aevalsrc='0.6*sin(2*PI*1000*t)*lt(mod(t,1),0.1)'"

say() { printf '  %s\n' "$*"; }

# A stub ROM so the entry appears in the list. Six bytes is enough for NextUI to
# show it; nothing ever launches these.
stub() { printf 'STUBRM' >"$out/$1.gb"; }

# A cover image, so that "the video replaced the static art" is a visible
# statement rather than an inference from an empty panel.
cover() {
	ffmpeg -v error -y -f lavfi -i "color=c=0x203040:s=320x240,drawbox=x=0:y=0:w=iw:h=ih:color=0x50a0ff@1:t=8" \
		-frames:v 1 "$out/.media/$1.png"
}

echo "1 Wide 720x480 -- the ordinary case"
stub "1 Wide 720x480"; cover "1 Wide 720x480"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=720x480:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=48000:d=$DUR" \
	-vf "$FLASH" \
	-c:v libx264 -pix_fmt yuv420p -profile:v main -g 30 \
	-color_primaries bt470bg -color_trc gamma28 -colorspace bt470bg \
	-color_range tv \
	-c:a aac -ar 48000 -ac 2 -b:a 128k \
	-movflags +faststart "$out/.videos/1 Wide 720x480.mp4"

echo "2 Tall 480x720 -- exercises the max_h clamp"
stub "2 Tall 480x720"; cover "2 Tall 480x720"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=480x720:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=48000:d=$DUR" \
	-vf "$FLASH" \
	-c:v libx264 -pix_fmt yuv420p -g 30 \
	-c:a aac -ar 48000 -ac 2 -b:a 128k \
	-movflags +faststart "$out/.videos/2 Tall 480x720.mp4"

echo "3 Fullrange 640x480 -- yuvj420p, every frame distinct"
stub "3 Fullrange 640x480"; cover "3 Fullrange 640x480"
# The gradient is what makes a range error visible: read as limited range, its
# dark end clips to black and its bright end to white. testsrc2 over the top
# keeps every frame distinct, so a preview that has frozen cannot be mistaken
# for one that is playing -- which cost real diagnosis time when the original
# fixture was 180 copies of one still.
ffmpeg -v error -y \
	-f lavfi -i "gradients=size=640x480:c0=black:c1=white:x0=0:y0=0:x1=640:y1=0:d=$DUR:r=30" \
	-f lavfi -i "testsrc2=size=640x120:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=48000:d=$DUR" \
	-filter_complex "[0:v][1:v]overlay=x=0:y=360[v];[v]$FLASH[vo]" \
	-map "[vo]" -map 2:a \
	-c:v libx264 -pix_fmt yuvj420p -color_range pc -g 30 \
	-c:a aac -ar 48000 -ac 2 -b:a 128k \
	-movflags +faststart "$out/.videos/3 Fullrange 640x480.mp4"

echo "4 VP9 unsupported -- must degrade to the static image"
stub "4 VP9 unsupported"; cover "4 VP9 unsupported"
# With sound on purpose. The video codec check comes first, so this also
# proves an unopenable clip never reaches the audio device.
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=640x480:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=48000:d=$DUR" \
	-c:v libvpx-vp9 -pix_fmt yuv420p -b:v 300k -speed 8 \
	-c:a aac -ar 48000 -ac 2 -b:a 128k \
	"$out/.videos/4 VP9 unsupported.mp4"

echo "5 No Preview -- nothing to arm"
stub "5 No Preview"; cover "5 No Preview"

echo "6 No Audio 640x480 -- the silent path, unchanged"
stub "6 No Audio 640x480"; cover "6 No Audio 640x480"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=640x480:rate=30:duration=$DUR" \
	-an -c:v libx264 -pix_fmt yuv420p -g 30 \
	-movflags +faststart "$out/.videos/6 No Audio 640x480.mp4"

echo "7 Mono 44k1 480x360 -- mono, and a rate the hardware may refuse"
stub "7 Mono 44k1 480x360"; cover "7 Mono 44k1 480x360"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=480x360:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=44100:d=$DUR" \
	-vf "$FLASH" \
	-c:v libx264 -pix_fmt yuv420p -g 30 \
	-c:a aac -ar 44100 -ac 1 -b:a 64k \
	-movflags +faststart "$out/.videos/7 Mono 44k1 480x360.mp4"

echo "8 MP3 44k1 320x240 -- the other decoder build.sh enables"
stub "8 MP3 44k1 320x240"; cover "8 MP3 44k1 320x240"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=320x240:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=44100:d=$DUR" \
	-vf "$FLASH" \
	-c:v libx264 -pix_fmt yuv420p -g 30 \
	-c:a libmp3lame -ar 44100 -ac 2 -b:a 128k \
	-movflags +faststart "$out/.videos/8 MP3 44k1 320x240.mp4"

echo "9 Odd rate 11025 320x240 -- makes SDL convert, or proves it did not have to"
stub "9 Odd rate 11025 320x240"; cover "9 Odd rate 11025 320x240"
ffmpeg -v error -y \
	-f lavfi -i "testsrc2=size=320x240:rate=30:duration=$DUR" \
	-f lavfi -i "$BEEP:s=11025:d=$DUR" \
	-vf "$FLASH" \
	-c:v libx264 -pix_fmt yuv420p -g 30 \
	-c:a aac -ar 11025 -ac 1 -b:a 32k \
	-movflags +faststart "$out/.videos/9 Odd rate 11025 320x240.mp4"

echo
echo "streams:"
for f in "$out/.videos/"*.mp4; do
	printf '  %-32s ' "$(basename "$f")"
	ffprobe -v error -show_entries stream=index,codec_name,codec_type,width,height,sample_rate,channels,pix_fmt,color_range \
		-of csv=p=0 "$f" | tr '\n' ' '
	printf '\n'
done

# The 3 Fullrange clip is the one that was once 180 copies of a single still,
# which was indistinguishable from a preview that had failed. Assert the
# property rather than trusting the recipe.
distinct=$(ffmpeg -v error -i "$out/.videos/3 Fullrange 640x480.mp4" -f framemd5 - 2>/dev/null \
	| grep -v '^#' | awk '{print $NF}' | sort -u | wc -l | tr -d ' ')
echo
echo "3 Fullrange has $distinct distinct frames (must be well above 1)"
[ "$distinct" -gt 100 ] || { echo "FAIL: 3 Fullrange is not animating" >&2; exit 1; }

echo
echo "fixtures in $out"
echo "push with:  adb push \"$out/.\" \"/mnt/sdcard/Roms/Game Boy (GB)/\""
