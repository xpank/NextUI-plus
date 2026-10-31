#!/usr/bin/env python3
"""Run Flip's production route refresh/open path with a delayed preview worker."""
import os
from pathlib import Path
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]


def function(source, name):
    source = re.sub(r"/\*.*?\*/|//[^\n]*", "", source, flags=re.S)
    start = re.search(r"^(?:static )?(?:void|int|bool) " + name + r"\([^;]*?\)\s*\{", source, re.M)
    assert start, name
    depth, end = 1, start.end()
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end]


nextui = (ROOT / "workspace/all/nextui/nextui.c").read_text()
player = (ROOT / "workspace/all/common/video/vidplayer.c").read_text()
audio = (ROOT / "workspace/all/common/video/vidaudio.c").read_text()
assert "int route_changed = videoPreviewAudioRouteChanged();" in nextui
assert "if (had_sink != has_sink || route_changed)" in nextui
prelude = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <errno.h>
#include <unistd.h>
#define PLATFORM_MY355 1
#define LOG_info(...) ((void)0)
#define LOG_warn(...) ((void)0)
#define USERDATA_PATH "/unused"
#define LAYER_THUMBNAIL 1
#define RTLD_LAZY 1
#define RTLD_NOLOAD 4
#define IN_NONBLOCK 1
#define IN_CLOEXEC 2
#define IN_CLOSE_WRITE 8
#define IN_MOVED_TO 16
#define IN_MOVED_FROM 32
#define IN_DELETE 64
#define IN_Q_OVERFLOW 128
#define IN_MODIFY 256
struct inotify_event { int wd; uint32_t mask, cookie, len; char name[]; };
static int event_pending, event_mask, watch_mask;
static int inotify_init1(int flags) { assert(flags == (IN_NONBLOCK|IN_CLOEXEC)); return 99; }
static int inotify_add_watch(int fd, const char *path, uint32_t mask) {
    assert(fd==99 && !strcmp(path, USERDATA_PATH)); watch_mask=mask; return 1;
}
static ssize_t route_read(int fd, void *buffer, size_t size) {
    assert(fd==99); (void)size;
    if (!event_pending) return -1;
    event_pending=0;
    struct inotify_event *event=buffer;
    event->mask=event_mask; event->len=16;
    strcpy(event->name,".asoundrc");
    return sizeof(*event)+event->len;
}
#define read route_read
static bool vidp_thread_started=true, vidp_worker_idle, vidp_request_pending, vidp_shutdown;
static bool vidp_has_frame;
static unsigned vidp_generation;
static char vidp_request_path[1024];
static pthread_mutex_t vidp_mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t vidp_cond=PTHREAD_COND_INITIALIZER, vidp_idle_cond=PTHREAD_COND_INITIALIZER;
static int worker_busy, closes, cache_resets, opens, audio_inits;
static bool alsa_loaded=true;
static int fake_mutex;
#define thumbMutex (&fake_mutex)
#define SDL_LockMutex(...) ((void)0)
#define SDL_UnlockMutex(...) ((void)0)
#define PLAT_freeVideoLayer() ((void)0)
#define GFX_clearLayers(...) ((void)0)
#define setNeedDraw(...) ((void)0)
#define SDL_GetTicks() 1234
#define CFG_getShowGameVideo() 1
#define exists(path) (!strcmp(path,"/roms/.videos/test.mp4"))
static int thumbchanged;
static char video_preview_path[1024]="/roms/.videos/test.mp4";
static uint32_t video_preview_rest_ms;
static int video_preview_playing=1, video_preview_armed, video_preview_drawn=1;
#define SDL_INIT_AUDIO 1
#define AUDIO_S16SYS 16
#define VIDA_RING_MS 2000
#define VIDA_FADE_MS 40
#define Uint16 uint16_t
#define SDL_zero(value) memset(&(value),0,sizeof(value))
typedef struct { int freq, format, channels, samples; void (*callback)(void); } SDL_AudioSpec;
static struct { int device,rate,period,capacity,fade_frames; bool open,idle; int16_t *ring; } vida;
static bool vida_subsystem=true;
static int vida_period_frames(int rate) { (void)rate; return 1024; }
static int vida_ms_to_frames(int ms,int rate) { return ms*rate/1000; }
static void vida_clear_clip(void) {}
static void vida_fade_out(void) {}
static void vida_callback(void) {}
static int SDL_InitSubSystem(int flags) { (void)flags; audio_inits++; return 0; }
static const char *SDL_GetError(void) { return "mock"; }
static const char *SDL_GetCurrentAudioDriver(void) { return "alsa"; }
static void SDL_LockAudioDevice(int device) { (void)device; }
static void SDL_UnlockAudioDevice(int device) { (void)device; }
static void SDL_PauseAudioDevice(int device,int pause) { (void)device;(void)pause; }
static void SDL_CloseAudioDevice(int device) {
    assert(device && !worker_busy);
    assert(pthread_mutex_trylock(&vidp_mutex)==EBUSY);
    closes++;
}
static int free_config(void) { assert(!vida.open && !worker_busy && closes>0); cache_resets++; return 0; }
static void *mock_dlopen(const char *name,int flags) {
    assert(!strcmp(name,"libasound.so.2") && flags==(RTLD_LAZY|RTLD_NOLOAD));
    return alsa_loaded ? (void*)1 : NULL;
}
static void *mock_dlsym(void *lib,const char *name) {
    assert(lib && !strcmp(name,"snd_config_update_free_global")); return (void*)free_config;
}
static int mock_dlclose(void *lib) { assert(lib); return 0; }
#define dlopen mock_dlopen
#define dlsym mock_dlsym
#define dlclose mock_dlclose
static int SDL_OpenAudioDevice(const char *name,int capture,SDL_AudioSpec *want,SDL_AudioSpec *have,int changes) {
    (void)name;(void)capture;(void)want;(void)have;(void)changes;
    assert(!worker_busy && !vida.open);
    assert(!alsa_loaded || cache_resets>opens);
    opens++; return 2;
}
static void *worker(void *unused) {
    (void)unused;
    pthread_mutex_lock(&vidp_mutex);
    while(!vidp_request_pending) pthread_cond_wait(&vidp_cond,&vidp_mutex);
    do {
        vidp_request_pending=false; worker_busy=1; vidp_worker_idle=false;
        pthread_mutex_unlock(&vidp_mutex);
        usleep(50000);
        pthread_mutex_lock(&vidp_mutex);
    } while (vidp_request_pending);
    worker_busy=0; vidp_worker_idle=true;
    pthread_cond_signal(&vidp_idle_cond);
    pthread_mutex_unlock(&vidp_mutex);
    return NULL;
}
'''
functions = "\n".join([
    function(audio, "VIDA_close"),
    function(player, "VIDP_stop"),
    function(player, "VIDP_stopAndWait"),
    function(nextui, "videoPreviewStop"),
    function(nextui, "videoPreviewAudioRouteChanged"),
    function(nextui, "videoPreviewRefreshAudioRoute"),
    function(audio, "vida_refresh_alsa_config"),
    function(audio, "VIDA_open"),
])
main = r'''
int main(void) {
    assert(!videoPreviewAudioRouteChanged());
    assert((watch_mask & IN_CLOSE_WRITE) && !(watch_mask & IN_MODIFY));
    vida.open=true; vida.device=1; vida.rate=48000; vida.idle=true;
    worker_busy=1;
    pthread_t thread; assert(!pthread_create(&thread,NULL,worker,NULL));
    // Same sink enum, different Bluetooth device: completed config write matters.
    event_pending=1; event_mask=IN_CLOSE_WRITE;
    assert(videoPreviewAudioRouteChanged());
    videoPreviewRefreshAudioRoute();
    assert(!pthread_join(thread,NULL));
    assert(closes==1 && !vida.open && !opens && !cache_resets);
    assert(video_preview_armed && !video_preview_playing && !video_preview_drawn);
    assert(!strcmp(video_preview_path,"/roms/.videos/test.mp4"));
    assert(video_preview_rest_ms==1234 && thumbchanged);
    char reason[128]; assert(VIDA_open(48000,reason,sizeof(reason)));
    assert(cache_resets==1 && opens==1 && !audio_inits);
    puts("ok route change waits for active worker/PCM close; next same-rate open refreshes ALSA first");
    // Unchanged route can keep the warm PCM; no unnecessary cache invalidation.
    assert(VIDA_open(48000,reason,sizeof(reason)));
    assert(opens==1 && cache_resets==1);
    event_pending=1; event_mask=IN_DELETE;
    assert(videoPreviewAudioRouteChanged());
    puts("ok same-sink completed writes and route removal are detected; unchanged clips reuse PCM");
    // Before any preview, a route change never creates worker, game PCM or ALSA.
    vidp_thread_started=false; video_preview_armed=0; video_preview_playing=0;
    videoPreviewRefreshAudioRoute();
    assert(opens==1 && cache_resets==1 && !audio_inits);
    alsa_loaded=false; vida.open=false; free(vida.ring); vida.ring=NULL;
    vida_refresh_alsa_config(); assert(cache_resets==1);
    puts("ok unused preview route refresh stays lazy and non-ALSA backends load no ALSA library");
    return 0;
}
'''
with tempfile.TemporaryDirectory(prefix="nextui-preview-route-") as directory:
    source = Path(directory) / "route.c"
    source.write_text(prelude + functions + main)
    binary = Path(directory) / "route"
    subprocess.run([os.environ.get("CC", "cc"), "-std=gnu99", "-pthread", str(source), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=10)
