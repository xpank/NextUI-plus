#!/usr/bin/env bash
# Checks for the game-art video previews (see VIDEO-PREVIEW-PORT.md).
#
# The single most important thing here is section 4's boot-time contract: the
# shipped nextui.elf must not name any ffmpeg library in DT_NEEDED, because an
# entry there is resolved by ld.so on EVERY boot, including the overwhelming
# majority that never show a preview. This device's boot is I/O bound -- roughly
# 1,370 ms of it is nextui.elf paging libraries in -- so a library that is never
# opened costs exactly zero, and one that is always mapped costs on every cycle.
#
# The DT_NEEDED test is anchored on libSDL2 being present. Without that anchor,
# an unparsed or empty list would satisfy "contains no libav*" while proving
# nothing at all.
#
# Runs on the macOS host: the ELF is parsed directly rather than shelled out to
# a readelf that only exists inside the toolchain container.
set -euo pipefail

root=${1:-$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)}
video="$root/workspace/all/common/video"
vidaudio="$root/workspace/all/common/video/vidaudio.c"
ffmpeg_build="$root/workspace/h700/ffmpeg/build.sh"
nextui_c="$root/workspace/all/nextui/nextui.c"
nextui_mk="$root/workspace/all/nextui/makefile"
generic_video="$root/workspace/all/common/generic_video.c"
api_c="$root/workspace/all/common/api.c"
elf="${NEXTUI_ELF:-$root/workspace/all/nextui/build/h700/nextui.elf}"
tmp=$(mktemp -d); trap 'rm -rf -- "$tmp"' EXIT
fail=0
note() { printf 'ok   %s\n' "$1"; }
bad()  { printf 'FAIL %s\n' "$1" >&2; fail=1; }

# Several checks below search for code patterns. The comments in these files
# describe exactly the things being searched for -- "adding -lavcodec would...",
# "HDMI runs at 1280x720 instead of 720x480" -- so a grep over the raw text
# matches the explanation and reports a fault that is not there. Strip comments
# first and search the code itself.
strip_c() { python3 -c '
import re,sys
s=open(sys.argv[1]).read()
s=re.sub(r"/\*.*?\*/","",s,flags=re.S)
s=re.sub(r"//[^\n]*","",s)
sys.stdout.write(s)' "$1"; }
strip_hash() { sed "s/#.*//" "$1"; }

for f in vidload.c vidload.h vidplayer.c vidplayer.h yuv2rgb.h test-yuv2rgb.c \
	vidaudio.c vidaudio.h pcm.h test-pcm.c; do
	[[ -f "$video/$f" ]] || { echo "ERROR: missing $video/$f" >&2; exit 1; }
done

# --- 1. the colour conversion is correct ----------------------------------
#
# Compiled and run on the build host. The expected values come from the
# standard definitions -- SMPTE bars, the BT.601/709/JPEG matrices -- not from
# this implementation, and one case asserts that exchanging U and V visibly
# changes the pixel, so a channel-swapped implementation cannot pass.
if cc -O2 -Wall -Wextra -o "$tmp/t-yuv" "$video/test-yuv2rgb.c" -I "$video" 2>"$tmp/ccerr"; then
	if "$tmp/t-yuv" >"$tmp/yuvout" 2>&1; then
		note "yuv2rgb: $(grep -c '^ok' "$tmp/yuvout") conversion cases pass"
	else
		bad "yuv2rgb conversion tests failed:"; cat "$tmp/yuvout" >&2
	fi
else
	bad "yuv2rgb test did not compile:"; cat "$tmp/ccerr" >&2
fi

# --- 2. the symbol table and its call sites cannot drift ------------------
#
# Every ffmpeg function goes through the api-> table, generated from the single
# VID_SYMBOLS list. A call with no table entry breaks the build, which is fine.
# The reverse -- a table entry nothing calls -- is silent, and is resolved on
# every load. Both directions are checked.
sed -n '/^#define VID_SYMBOLS(X)/,/^$/p' "$video/vidload.h" \
	| grep -oE '^	X\([A-Z]+, [^,]+, [a-z_0-9]+' | sed 's/.*, //' | sort -u >"$tmp/declared"
grep -oE 'api->[a-z_0-9]+' "$video/vidplayer.c" | sed 's/api->//' | sort -u >"$tmp/called"
# The version functions are called by the loader itself, not the decoder. The
# trailing "(" matters: the X-macro that fills the table contains the text
# "&vid_api.name", and without it the literal token "name" is picked up.
grep -oE 'vid_api\.[a-z_0-9]+\(' "$video/vidload.c" | sed 's/vid_api\.//; s/(//' | sort -u >"$tmp/called_loader"
sort -u "$tmp/called" "$tmp/called_loader" >"$tmp/called_all"

d=$(wc -l <"$tmp/declared"); c=$(wc -l <"$tmp/called_all")
# Control: an extraction that silently produced nothing would make both
# comparisons below pass while checking nothing.
if (( d < 10 || c < 10 )); then
	bad "symbol extraction produced too little to be real (declared=$d called=$c)"
else
	missing=$(comm -13 "$tmp/declared" "$tmp/called_all") || true
	unused=$(comm -23 "$tmp/declared" "$tmp/called_all") || true
	[[ -z "$missing" ]] || bad "ffmpeg symbols called but not in VID_SYMBOLS: $(echo $missing)"
	[[ -z "$unused"  ]] || bad "VID_SYMBOLS declares symbols nothing calls: $(echo $unused)"
	[[ -n "$missing$unused" ]] || note "ffmpeg symbol table matches its call sites ($d symbols)"
fi

# --- 3. ffmpeg is never linked -------------------------------------------
if strip_hash "$nextui_mk" | grep -qE -- '-l(av|sw)[a-z]+|-lpostproc'; then
	bad "a -lav*/-lsw* reached the nextui link line; ffmpeg must only ever be dlopen()ed"
else
	note "no ffmpeg library on the nextui link line"
fi

if [[ -f "$elf" ]]; then
	python3 - "$elf" >"$tmp/needed" <<'PY'
import struct, sys
d = open(sys.argv[1],'rb').read()
assert d[:4] == b'\x7fELF' and d[4] == 2, "not an ELF64"
phoff, = struct.unpack_from('<Q', d, 0x20)
phentsize, phnum = struct.unpack_from('<HH', d, 0x36)
loads, dyn = [], None
for i in range(phnum):
    o = phoff + i*phentsize
    ptype, = struct.unpack_from('<I', d, o)
    off, vaddr = struct.unpack_from('<Q', d, o+0x08)[0], struct.unpack_from('<Q', d, o+0x10)[0]
    filesz, = struct.unpack_from('<Q', d, o+0x20)
    if ptype == 1: loads.append((vaddr, off, filesz))
    if ptype == 2: dyn = (off, filesz)
assert dyn, "no PT_DYNAMIC"
def v2o(v):
    for vaddr, off, filesz in loads:
        if vaddr <= v < vaddr + filesz: return off + (v - vaddr)
    raise SystemExit("unmapped vaddr")
off, size = dyn
needed, strtab = [], None
for p in range(off, off+size, 16):
    tag, val = struct.unpack_from('<QQ', d, p)
    if tag == 0: break
    if tag == 1: needed.append(val)
    if tag == 5: strtab = v2o(val)
assert strtab is not None, "no DT_STRTAB"
for n in needed:
    e = d.index(b'\0', strtab+n)
    print(d[strtab+n:e].decode())
PY
	# Anchor first: an empty list must not pass as "contains no libav*".
	if grep -q '^libSDL2-2\.0\.so\.0$' "$tmp/needed"; then
		if grep -qE '^lib(av|sw)|^libpostproc' "$tmp/needed"; then
			bad "nextui.elf DT_NEEDED contains ffmpeg: $(grep -E '^lib(av|sw)|^libpostproc' "$tmp/needed" | tr '\n' ' ')"
		else
			note "nextui.elf DT_NEEDED has $(wc -l <"$tmp/needed" | tr -d ' ') entries, anchored on libSDL2, none of them ffmpeg"
		fi
		# The audio work's own version of the same rule. Preview sound is
		# decoded with pcm.h and rate-converted by SDL itself precisely so that
		# neither libsamplerate nor libasound has to be linked here: an entry
		# for either is paged in by ld.so on every boot, including every boot
		# that never plays a preview. libasound is SDL's business and SDL
		# dlopen()s it; libsamplerate is minarch's.
		if grep -qE '^libsamplerate|^libasound' "$tmp/needed"; then
			bad "nextui.elf now links $(grep -E '^libsamplerate|^libasound' "$tmp/needed" | tr '\n' ' ') -- preview audio must cost nothing at startup"
		else
			note "no libsamplerate and no libasound in nextui.elf DT_NEEDED"
		fi
	else
		bad "DT_NEEDED did not contain the libSDL2 anchor; the parse cannot be trusted"
	fi
else
	echo "skip nextui.elf DT_NEEDED (not built): $elf"
fi

# --- 4. previews are armed only when the file exists ----------------------
#
# Without this, resting on any ROM would reach VIDP_request and dlopen
# libavcodec for a file that does not exist. Most ROMs have no preview, so this
# is what keeps ffmpeg unmapped during ordinary browsing -- it is a required
# property, not an optimisation.
grep -qF 'video_preview_armed = exists(video_preview_path);' "$nextui_c" \
	&& note "previews arm only when the .mp4 actually exists" \
	|| bad "previews are armed without checking the file exists; that dlopens ffmpeg for every ROM"

grep -qF '#define VIDEO_PREVIEW_REST_MS 2000' "$nextui_c" \
	&& note "the rest timer is 2000 ms" || bad "the two-second rest before a preview starts is not 2000 ms"
grep -qF '/.videos/%s.mp4' "$nextui_c" \
	&& note "previews read .videos/<name>.mp4, mirroring .media" || bad "previews no longer read .videos/<name>.mp4"
grep -qF 'CFG_getShowGameVideo()' "$nextui_c" \
	&& note "the feature is behind CFG_getShowGameVideo()" || bad "no config gate on video previews"

# --- 5. no per-frame texture churn ---------------------------------------
#
# PLAT_drawOnLayer creates and destroys an SDL_Texture on every call. That is
# why previews have their own entry point at all: at 30 fps it would be an
# allocate/free pair per frame in the GPU driver. The texture must be created
# only when the clip changes.
strip_c "$generic_video" | awk '/^void PLAT_drawVideoFrameOnLayer/,/^}/' >"$tmp/present"
creates=$(grep -c 'SDL_CreateTexture' "$tmp/present" || true)
destroys=$(grep -c 'SDL_DestroyTexture' "$tmp/present" || true)
if (( creates == 1 && destroys == 1 )) \
	&& grep -q 'if (!video_layer.texture)' "$tmp/present" \
	&& grep -q 'video_layer.generation != generation' "$tmp/present"; then
	# One create, guarded by the texture not already existing; one destroy,
	# guarded by the clip's generation or size having changed. So the texture
	# survives from frame to frame and is replaced only between clips.
	note "the presenter creates its texture once per clip, not once per frame"
else
	bad "presenter texture handling changed (creates=$creates destroys=$destroys); it may now churn per frame"
fi
grep -q 'SDL_LockTexture' "$tmp/present" \
	&& note "frames are converted straight into the texture's staging memory" \
	|| bad "the presenter no longer locks the texture; an extra full-frame copy crept in"

# --- 6. video matches the static art it replaces --------------------------
#
# The cover image gets rounded corners from GFX_ApplyRoundedCorners_8888. If the
# video draws square corners the swap is visibly wrong, so the same radius --
# scaled the same way against the source width -- has to reach the presenter.
# Scoped to the functions in question: CFG_getThumbnailRadius() also appears in
# onThumbLoaded() and video_round_corners() is also its own definition, so an
# unscoped grep passes even when the preview path has dropped both.
strip_c "$nextui_c" | awk '/^static int videoPreviewTick/,/^}/' >"$tmp/tick"
grep -qF 'CFG_getThumbnailRadius()' "$tmp/tick" \
	&& note "the preview rectangle carries the cover art's corner radius" \
	|| bad "no corner radius is computed for video; it will not match the static art"
grep -q 'video_round_corners(' "$tmp/present" \
	&& note "the presenter rounds the frame's corners" \
	|| bad "the presenter does not round corners"

# Every rectangle is derived from screen->w/h rather than a hardcoded panel
# size, which is what makes HDMI (1280x720 instead of 720x480) work for free.
if strip_c "$nextui_c" | awk '/^static int videoPreviewTick/,/^}/' | grep -qE '720|480|1280'; then
	bad "videoPreviewTick hardcodes a panel dimension; HDMI changes the resolution"
else
	note "the preview rectangle is computed from screen->w/h, never hardcoded"
fi

# --- 7. diagnostics are keyed to the clip, not to the session -------------
#
# Every one of these lines was once gated by a static bool, so the second clip
# of a session that had anything to say said nothing at all -- a full-range
# fixture that played correctly logged nothing, because the clip before it had
# already spent the session's single line, and it read as a silent failure.
# A per-session flag reappearing inside vidp_play() brings that back.
vidplayer="$video/vidplayer.c"
strip_c "$vidplayer" | awk '/^static void vidp_play/,/^}/' >"$tmp/play"
gates=$(grep -c 'vidp_first_time(path,' "$tmp/play" || true)
session=$(grep -c 'vidp_logged_' "$tmp/play" || true)
if (( gates >= 6 && session == 0 )); then
	note "every preview diagnostic is gated per clip path ($gates of them), not per session"
else
	bad "preview diagnostics are gated per session again (per-clip=$gates per-session=$session)"
fi
# The identifying line is the one whose absence means something failed earlier,
# so it is the one that most has to be per clip.
grep -q 'vidp_first_time(path, VIDP_LOG_IDENTITY)' "$tmp/play" \
	&& note "each clip logs its own size/format/range/colourspace line" \
	|| bad "the identifying line is no longer logged once per clip"


# --- 8. the sample conversion is correct ---------------------------------
#
# Same arrangement as the colour conversion above, and for the same reason:
# this is what libswresample would otherwise do, so it has to be at least as
# trustworthy as the library it replaces. The expected values come from the
# definitions of the formats, and two of the cases exist for specific ways it
# can be wrong while looking right -- see the file's own header.
if cc -O2 -Wall -Wextra -o "$tmp/t-pcm" "$video/test-pcm.c" -I "$video" -lm 2>"$tmp/pcmccerr"; then
	if "$tmp/t-pcm" >"$tmp/pcmout" 2>&1; then
		note "pcm: $(grep -c '^ok' "$tmp/pcmout") sample conversion cases pass"
	else
		bad "pcm conversion tests failed:"; cat "$tmp/pcmout" >&2
	fi
else
	bad "pcm test did not compile:"; cat "$tmp/pcmccerr" >&2
fi

# --- 9. audio never takes the video down ---------------------------------
#
# Five ways audio can fail -- no stream, no decoder, a sample format pcm.h
# cannot read, a device that will not open, and the setting switched off -- and
# every one of them has to end in a silent preview rather than no preview. In
# the code that means vidp_audio_open returns false and vidp_play carries on;
# a `goto done` reaching out of the audio path would abandon the picture too.
strip_c "$vidplayer" | awk '/^static bool vidp_audio_open/,/^}/' >"$tmp/audioopen"
if grep -qE 'goto done' "$tmp/audioopen"; then
	bad "the audio open path can goto done; an audio failure would stop the video with it"
else
	note "every audio failure returns false, leaving the video playing"
fi
grep -qF 'CFG_getShowGameVideoAudio()' "$vidplayer" \
	&& note "preview audio is behind CFG_getShowGameVideoAudio()" \
	|| bad "no config gate on preview audio"

# --- 10. the silent path still paces against wall time -------------------
#
# The video-only build's pacing is already tested and already measured, and a
# clip with no sound must keep it exactly. Two clocks, chosen once when the
# clip opens -- not one clock with audio bolted into it.
strip_c "$vidplayer" | awk '/^static void vidp_play/,/^}/' >"$tmp/play"
if grep -qF 'audio_on ? VIDA_clockMs()' "$tmp/play" &&
	grep -qF 'vidp_now_ms() - wall_start' "$tmp/play"; then
	note "the clock is chosen once: audio when there is sound, wall time when there is not"
else
	bad "the silent path no longer paces against wall time"
fi

# --- 11. the reader really does run ahead --------------------------------
#
# Packets in an MP4 are interleaved in decode order, so reaching the video
# packet for moment T means having read the audio for moment T -- which is the
# audio the device wants now, not in a moment. Publishing that frame when the
# audio clock reaches T therefore empties the ring by construction. The
# high-water read-ahead is what stops that, and it is a required property, not
# a tuning knob.
if grep -qF 'VIDA_queuedFrames() < reader->high_water' "$vidplayer"; then
	note "the demuxer reads ahead to a high-water mark, so the ring cannot run dry by construction"
else
	bad "the audio read-ahead is gone; the device will underrun on every frame"
fi

# --- 12. the device is closed before the machine suspends ----------------
#
# PWR_enterSleep() closes NextUI's own audio device before suspending, because
# a PCM left open across suspend-to-RAM comes back in a state SDL takes about
# ten seconds to close. A preview's device is a second PCM that call knows
# nothing about, so nextui.c has to close it through PWR_update's before_sleep
# hook -- and synchronously, or the machine suspends mid-teardown.
grep -qF 'PWR_setTransitionCallbacks(videoPreviewBeforeSleep, videoPreviewBeforePowerOff);' "$nextui_c" \
	&& note "sleep runs videoPreviewBeforeSleep first" \
	|| bad "power transitions no longer stop the preview before sleeping or shutting down"
strip_c "$nextui_c" | awk '/^static void videoPreviewBeforeSleep/,/^}/' >"$tmp/beforesleep"
grep -qF 'VIDP_stopAndWait();' "$tmp/beforesleep" \
	&& note "and waits for the audio device to actually close" \
	|| bad "the sleep hook does not wait; the machine can suspend with the PCM open"

# --- 13. waking does not steal the audio device --------------------------
#
# PWR_exitSleep() reinitialises NextUI's own audio device after every wake. That
# was unconditional, and in nextui.elf -- which never calls SND_init -- it did
# not reinitialise anything: it created a device from nothing, at a sample rate
# of zero, and held the ALSA PCM for the rest of the session. Nothing noticed
# while the launcher had no other use for audio. Preview audio does: after one
# sleep and wake, every preview from then on failed to open with EBUSY and
# played silently. The guard is what stops that coming back.
strip_c "$api_c" | awk '/^static void PWR_exitSleep/,/^}/' >"$tmp/exitsleep"
if grep -qF 'SND_resetAudio' "$tmp/exitsleep"; then
	if grep -qF 'if (pwr.restore_audio_after_sleep)' "$tmp/exitsleep"; then
		note "waking only reinitialises audio in a process that had some"
	else
		bad "PWR_exitSleep reinitialises audio unconditionally; that takes the ALSA PCM away from previews for the rest of the session"
	fi
else
	note "PWR_exitSleep no longer touches the audio device at all"
fi

# --- 14. the ffmpeg build has audio and still has no resamplers ----------
if strip_hash "$ffmpeg_build" | grep -qF -- '--enable-decoder=aac'; then
	note "the ffmpeg build enables the AAC decoder"
else
	bad "the ffmpeg build has no AAC decoder; every preview will be silent"
fi
if strip_hash "$ffmpeg_build" | grep -qF -- '--disable-swresample'; then
	note "libswresample stays disabled; pcm.h and SDL do that work"
else
	bad "libswresample has been enabled; that is a fourth library on the card for a clamp and an interleave"
fi
# vidaudio.c is the SDL side and must stay free of ffmpeg: the decode side
# hands it interleaved S16 and nothing else. Keeping the two apart is what
# lets pcm.h and its test be built on a host with no ffmpeg at all.
if grep -qE '#include <libav' "$vidaudio"; then
	bad "vidaudio.c includes an ffmpeg header; the audio device must not know what a codec is"
else
	note "vidaudio.c knows nothing about ffmpeg"
fi

(( fail == 0 )) && echo && echo "video preview checks passed." || { echo; echo "video preview checks FAILED." >&2; exit 1; }
