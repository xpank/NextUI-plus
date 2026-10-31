// Device-side sanity check for the three ffmpeg libraries on the card.
//
// vidload.c does the same thing at runtime and reports it in the log, and
// check-video-preview.sh checks the source statically. This is for the case
// neither covers: a card whose .system/h700/lib holds the wrong libraries, or
// none, where the only symptom is that previews never start. It says which
// library or symbol is missing rather than leaving it to be inferred.
//
// Cross-compile it in the toolchain container and run it on the device:
//   $CROSS_COMPILE"gcc" -o probe probe.c -ldl
//   adb push probe /tmp/ && adb shell \
//     'LD_LIBRARY_PATH=/mnt/sdcard/.system/h700/lib /tmp/probe'

#include <dlfcn.h>
#include <stdio.h>
int main(void) {
    const char *libs[] = {"libavutil.so.58","libavcodec.so.60","libavformat.so.60"};
    const char *vers[] = {"avutil_version","avcodec_version","avformat_version"};
    void *h[3]; int bad = 0;
    for (int i = 0; i < 3; i++) {
        h[i] = dlopen(libs[i], RTLD_NOW | RTLD_LOCAL);
        if (!h[i]) { printf("FAIL dlopen %s: %s\n", libs[i], dlerror()); bad = 1; continue; }
        unsigned (*v)(void) = (unsigned (*)(void))dlsym(h[i], vers[i]);
        if (!v) { printf("FAIL dlsym %s\n", vers[i]); bad = 1; continue; }
        unsigned n = v();
        printf("ok  %-20s %s = %u.%u.%u\n", libs[i], vers[i], n>>16, (n>>8)&0xff, n&0xff);
    }
    /* the symbols the decoder actually needs */
    const char *need[] = {"avformat_open_input","avformat_find_stream_info","av_read_frame",
        "av_seek_frame","avcodec_find_decoder","avcodec_alloc_context3",
        "avcodec_parameters_to_context","avcodec_open2","avcodec_send_packet",
        "avcodec_receive_frame","avcodec_flush_buffers","av_packet_alloc",
        "av_frame_alloc","av_frame_unref","av_log_set_level","av_strerror", 0};
    for (int i = 0; need[i]; i++) {
        void *s = NULL;
        for (int j = 0; j < 3 && !s; j++) if (h[j]) { dlerror(); s = dlsym(h[j], need[i]); if (dlerror()) s = NULL; }
        if (!s) { printf("FAIL symbol %s not found in any lib\n", need[i]); bad = 1; }
    }
    printf(bad ? "\nPROBE FAILED\n" : "\nPROBE OK: all libs load and every decoder symbol resolves\n");
    return bad;
}
