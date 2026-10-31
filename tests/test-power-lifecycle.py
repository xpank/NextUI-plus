#!/usr/bin/env python3
"""Run production transition functions with stub hardware and a delayed worker."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    start = re.search(r"^(?:static )?void " + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert start, name
    depth = 1
    end = start.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


def run(code, name, tmp):
    path = tmp / (name + ".c")
    path.write_text(code)
    binary = tmp / name
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-pthread", str(path), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)


api = (ROOT / "workspace/all/common/api.c").read_text()
player = Path(os.environ.get("VIDPLAYER_SOURCE", ROOT / "workspace/all/common/video/vidplayer.c")).read_text()
power_prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
typedef void (*PWR_callback_t)(void);
typedef struct { int w, h; } Screen;
typedef struct { int x,y,w,h; } SDL_Rect;
static Screen screen = {720,480};
static struct { Screen *screen; } gfx = {&screen};
static struct { int large; } font;
static struct { int initialized, sample_rate_in; double frame_rate; } snd;
static struct { int restore_audio_after_sleep, can_poweroff, resume_tick; PWR_callback_t before_sleep, before_poweroff; } pwr;
static int resets, closes, sleep_hooks, power_hooks, platform_poweroffs;
#define SND_CLOSE_ON_SLEEP 1
#define FIXED_WIDTH 720
#define FIXED_HEIGHT 480
#define FIXED_PITCH 1440
#define HDMI_WIDTH 1280
#define HDMI_HEIGHT 720
#define HDMI_PITCH 2560
#define HAS_POWER_BUTTON 1
#define HAS_POWEROFF_BUTTON 0
#define AUTO_RESUME_PATH "/unused"
#define MUTE_VOLUME_RAW 0
#define LIGHT_PROFILE_SLEEP 0
#define LOG_info(...) ((void)0)
#define CFG_getHaptics() 0
#define GetHDMI() 0
#define exists(...) 0
#define GetVolume() 10
#define SetVolume(...) ((void)0)
#define SetRawVolume(...) ((void)0)
#define LEDS_pushProfileOverride(...) ((void)0)
#define LEDS_popProfileOverride(...) ((void)0)
#define PLAT_clearVideo(...) ((void)0)
#define PLAT_flip(...) ((void)0)
#define PLAT_enableBacklight(...) ((void)0)
#define PLAT_clearLayers(...) ((void)0)
#define PLAT_clearAll(...) ((void)0)
#define VIB_singlePulse(...) ((void)0)
#define GFX_blitMessage(...) ((void)0)
#define GFX_flip(...) ((void)0)
#define GFX_clear(...) ((void)0)
#define SND_overrideMute(...) ((void)0)
#define PWR_updateFrequency(...) ((void)0)
#define PAD_reset() ((void)0)
#define system(...) 0
#define sync() ((void)0)
#define SDL_GetTicks() 1
static void SND_quit(void) { closes++; snd.initialized = 0; }
static void SND_pauseAudio(bool pause) { (void)pause; }
static void SND_resetAudio(int rate, double fps) {
    assert(rate == 48000 && fps == 60); resets++; snd.initialized = 1;
}
static Screen *GFX_resize(int w,int h,int p) { (void)w;(void)h;(void)p; assert(power_hooks > platform_poweroffs); return &screen; }
static void PLAT_powerOff(int reboot) { (void)reboot; platform_poweroffs++; }
static void PWR_waitForWake(void) {}
static void sleep_hook(void) { sleep_hooks++; }
static void power_hook(void) { power_hooks++; }
'''
power_functions = "\n".join(function(api, name) for name in ["PWR_setTransitionCallbacks", "PWR_enterSleep", "PWR_exitSleep", "PWR_sleep", "PWR_powerOff"])
power_main = r'''
int main(void) {
    snd.sample_rate_in=48000; snd.frame_rate=60;
    snd.initialized=1; PWR_enterSleep(); assert(!snd.initialized);
    PWR_exitSleep(); assert(resets==1 && snd.initialized && !pwr.restore_audio_after_sleep);
    snd.initialized=0; PWR_enterSleep(); PWR_exitSleep(); assert(resets==1 && !snd.initialized);
    snd.initialized=1; PWR_enterSleep(); PWR_exitSleep(); assert(resets==2 && snd.initialized);
    puts("ok game audio survives repeated sleep; preview-only frontend opens no game PCM");
    PWR_setTransitionCallbacks(sleep_hook,power_hook);
    PWR_sleep(); assert(sleep_hooks==1 && resets==3);
    pwr.can_poweroff=1; PWR_powerOff(0); PWR_powerOff(1);
    assert(power_hooks==2 && platform_poweroffs==2);
    pwr.can_poweroff=0; PWR_powerOff(0); assert(power_hooks==2);
    puts("ok direct sleep, poweroff and reboot invoke registered resource cleanup");
    return 0;
}
'''
worker_prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
static bool vidp_thread_started=true, vidp_worker_idle, vidp_request_pending, vidp_shutdown;
static pthread_mutex_t vidp_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vidp_idle_cond=PTHREAD_COND_INITIALIZER, request_cond=PTHREAD_COND_INITIALIZER;
static int close_count, work_ms;
#define LOG_info(...) ((void)0)
static void VIDP_stop(void) {
    pthread_mutex_lock(&vidp_mutex); vidp_request_pending=true;
    pthread_cond_signal(&request_cond); pthread_mutex_unlock(&vidp_mutex);
}
static void VIDA_close(void) {
    assert(vidp_worker_idle && !vidp_request_pending);
    assert(pthread_mutex_trylock(&vidp_mutex)==EBUSY);
    close_count++;
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&vidp_mutex);
    while(!vidp_request_pending) pthread_cond_wait(&request_cond,&vidp_mutex);
    vidp_request_pending=false; vidp_worker_idle=false;
    pthread_mutex_unlock(&vidp_mutex);
    usleep(work_ms*1000);
    pthread_mutex_lock(&vidp_mutex); vidp_worker_idle=true;
    pthread_cond_signal(&vidp_idle_cond); pthread_mutex_unlock(&vidp_mutex);
    return NULL;
}
'''
worker_main = r'''
int main(void) {
    for(int initially_idle=0; initially_idle<2; initially_idle++) {
        vidp_worker_idle=initially_idle; vidp_request_pending=false;
        work_ms=initially_idle ? 30 : 1150;
        pthread_t thread; assert(!pthread_create(&thread,NULL,worker,NULL));
        VIDP_stopAndWait(); assert(!pthread_join(thread,NULL));
        assert(close_count==initially_idle+1);
    }
    puts("ok preview close waits beyond one second for acknowledgement and consumes a pending stop");
    puts("ok audio close retains the worker mutex until the device is released");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nextui-power-") as directory:
    tmp = Path(directory)
    run(power_prelude + power_functions + power_main, "power", tmp)
    run(worker_prelude + function(player, "VIDP_stopAndWait") + worker_main, "worker", tmp)
