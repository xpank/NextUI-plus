// The video preview's own audio device.
//
// Deliberately not NextUI's SND_* API. That one is shaped around an emulator's
// SND_init(sample_rate, frame_rate) pacing, keeps global state, and belongs to
// minarch.elf -- a different process. nextui.elf has never opened an audio
// device in its life: there are zero SND_*, SDL_INIT_AUDIO or SDL_OpenAudio
// references in nextui.c. So there is nothing to mix with, nothing to duck and
// nothing to share, and the smallest correct thing is a device of our own,
// opened when a clip with audio starts and closed when it stops.
//
// Nothing here scales for volume. Volume and mute are applied at the ALSA mixer
// by SetVolume() and PLAT_overrideMute(), which is below SDL, so preview audio
// follows the system volume and the mute switch without being told to.
//
// The device outlives the clip. Opening and closing the PCM is not free on
// this hardware: /sys/kernel/debug/asoc/audiocodec shows DACL, DACR, both
// output mixers, both LINEOUT pins and SPK going On -> Off on close and
// Off -> On on open, with the codec's bias level following. That is an analog
// power cycle of the whole output chain on every selection change, and it is
// what is heard as a click. So between clips the device goes to VIDA_idle():
// still open, still running, emitting digital silence. VIDA_close() is kept
// for the moments the device really has to be handed back.
//
// One clip at a time, called only from the decode worker except for the SDL
// callback, which SDL runs on its own thread. Everything the two share is
// behind SDL_LockAudioDevice.

#ifndef NEXTUI_VIDAUDIO_H
#define NEXTUI_VIDAUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// A device at the clip's own sample rate, stereo S16, silent until
// VIDA_start() so the caller can prime the ring before a sample is heard.
//
// Reuses the open device when its rate matches, which is what keeps the codec
// powered across a selection change; only a clip at a different rate closes
// and reopens, because the rate is fixed when the device opens.
//
// `rate` is the clip's rate and is requested exactly: SDL is opened with no
// allowed changes, so when the hardware cannot take that rate SDL builds the
// converting stream itself. That is the whole reason neither libswresample nor
// libsamplerate is needed here -- and libsamplerate would have had to be
// *linked*, which would put an entry in nextui.elf's DT_NEEDED and cost boot
// time on every boot, including every boot that never plays a preview.
//
// False on any failure, with `reason` filled in. The caller's response is
// always to play the clip silently.
bool VIDA_open(int rate, char *reason, size_t reason_size);

// Fades out, waits for the fade to reach the speaker, and closes. Idempotent.
//
// Closing is a hardware event, not just the release of a file descriptor: the
// codec's DAC, output mixer, line-out and speaker amplifier are all powered
// down when the last stream on the PCM goes away, and back up when one
// arrives. Between two clips that is audible. Use VIDA_idle() there and keep
// this for the moments where the device genuinely has to be given up -- sleep,
// and the frontend exiting to launch a game.
void VIDA_close(void);

// Between clips, while the list is still up.
//
// Fades out exactly as VIDA_close() does and drops the ring, but leaves the
// device open and the PCM running on digital silence. The next VIDA_open() at
// the same rate then reuses it, so navigating from one preview to the next
// never stops the ALSA stream and never power-cycles the codec.
//
// Measured on the device: closing the PCM takes DACL, DACR, both output
// mixers, both LINEOUT pins and SPK from On to Off, and the codec's bias level
// from On to Standby -- a full analog power-down and, 1.9 s later, a
// power-up. `aplay` of a file containing nothing but digital silence
// reproduces the same cycle, which is what shows it is the power sequencing
// and not anything in the samples.
void VIDA_idle(void);

bool VIDA_isOpen(void);

// Frames of stereo the ring is holding, and room for more. Frames, not bytes
// or samples: one frame is one left and one right sample.
int VIDA_queuedFrames(void);
int VIDA_capacityFrames(void);

// Copies interleaved stereo S16 into the ring and returns how many frames were
// taken. Less than `frames` means the ring was full, which the caller's own
// high-water gate should have prevented.
int VIDA_push(const int16_t *stereo, int frames);

// Starts the clip playing. Called once the ring has been primed.
//
// The PCM is already running by then -- it has been since VIDA_open() -- so
// this only ends the silence, which is what makes the clock exact: `played`
// counts frames of the clip, never frames of the gap before it.
void VIDA_start(void);

// Back to the state just after VIDA_open: silent, ring empty, clock at zero,
// fade-in armed. This is the loop point -- both streams seek, both decoders
// flush, and the ring is dropped so the restart is aligned rather than playing
// the previous pass's tail over the new pass's first frame.
void VIDA_reset(void);

// Milliseconds of the clip that have actually reached the speaker.
//
// This is the master clock while audio is playing: the video decoder paces
// against it instead of against wall time, which is what keeps picture and
// sound together over a loop rather than merely starting them together.
int64_t VIDA_clockMs(void);

// The demuxer has reached the end of the file.
//
// Two things follow. The tail of whatever is still queued is ramped down to
// silence, because the alternative is cutting a PCM stream mid-waveform and
// hearing the step. And the ring is allowed to run dry without that counting
// as an underrun: a clip whose audio track ends a fraction before its video
// track -- which is most of them -- is not a device that starved, and counting
// it as one made the "ran the audio device dry" diagnostic fire on a clip that
// was playing perfectly. Measured: none at 48 kHz, where a callback is 21 ms,
// and eighteen over a few loops at 11 kHz, where it is 93 ms.
void VIDA_endOfStream(void);

// How many times the device asked for samples that were not there. Zero is the
// expectation; a rising count means decode is not keeping ahead of playback.
int VIDA_underruns(void);

#endif
