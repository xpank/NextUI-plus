// Exercise the complete production config implementation on the host. Only
// platform paths and the two utility clamps are supplied by this harness.
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#define __DEFINES_H__
#define MAX_PATH 512
#define GFX_SCALE_FULLSCREEN 0
#define GFX_SCALE_NUM_OPTIONS 3
#define SHARED_USERDATA_PATH getenv("SHARED_USERDATA_PATH")
#define RES_PATH "."
int clamp(int n, int low, int high) { return n < low ? low : n > high ? high : n; }
double clampd(double n, double low, double high) { return n < low ? low : n > high ? high : n; }
#ifndef CONFIG_SOURCE
#define CONFIG_SOURCE "../workspace/all/common/config.c"
#endif
#include CONFIG_SOURCE

static char path[1024];
static void write_settings(const char *text) {
    FILE *f = fopen(path, "w"); assert(f);
    assert(fputs(text, f) >= 0); assert(fclose(f) == 0);
}
static char *read_settings(void) {
    FILE *f = fopen(path, "r"); assert(f);
    assert(fseek(f, 0, SEEK_END) == 0);
    long n = ftell(f); assert(n >= 0); rewind(f);
    char *text = calloc((size_t)n + 1, 1); assert(text);
    assert(fread(text, 1, (size_t)n, f) == (size_t)n);
    assert(fclose(f) == 0); return text;
}
static void equal_file(const char *want) {
    char *got = read_settings(); assert(strcmp(got, want) == 0); free(got);
}
static void has_line(const char *text, const char *line) {
    const char *p = strstr(text, line); assert(p);
    assert(p == text || p[-1] == '\n');
    assert(!strstr(p + strlen(line), line));
}
int main(int argc, char **argv) {
    assert(argc == 2); assert(setenv("SHARED_USERDATA_PATH", argv[1], 1) == 0);
    snprintf(path, sizeof(path), "%s/minuisettings.txt", argv[1]);
    const char *original = "# retained exactly during load\nwifi=1\nzzFutureKey=1234\nsuspendTimeout=120\nzzEmpty=\nzzNoNewline=last";
    write_settings(original);
    struct stat before, after; assert(stat(path, &before) == 0);
    CFG_init(NULL, NULL);
    assert(CFG_getWifi()); assert(CFG_getSuspendTimeoutSecs() == 120);
    equal_file(original); assert(stat(path, &after) == 0);
    assert(before.st_ino == after.st_ino);
    puts("ok loading parses settings without changing the file");

    CFG_setWifi(false);
    char *saved = read_settings();
    has_line(saved, "wifi=0\n"); has_line(saved, "suspendTimeout=120\n");
    has_line(saved, "zzFutureKey=1234\n"); has_line(saved, "zzEmpty=\n");
    has_line(saved, "zzNoNewline=last\n"); free(saved);
    CFG_quit(); saved = read_settings();
    has_line(saved, "zzFutureKey=1234\n"); has_line(saved, "zzEmpty=\n"); free(saved);
    puts("ok setters and process exit preserve unknown settings");

    write_settings("wifi=1\nwifiExtra=2\nwif=3\nwifi=9\ninvalid line\n");
    CFG_sync(); saved = read_settings();
    has_line(saved, "wifi=0\n"); has_line(saved, "wifiExtra=2\n"); has_line(saved, "wif=3\n");
    assert(!strstr(saved, "wifi=1\n")); assert(!strstr(saved, "wifi=9\n"));
    assert(!strstr(saved, "invalid line")); free(saved);
    puts("ok exact keys replace duplicate known keys and retain neighboring names");

    assert(unlink(path) == 0); CFG_init(NULL, NULL); assert(access(path, F_OK) != 0);
    CFG_setWifi(true); saved = read_settings(); has_line(saved, "wifi=1\n"); free(saved);
    puts("ok a missing file stays absent during load and is created by a save");
    write_settings("zzStillHere=1\n"); assert(unsetenv("SHARED_USERDATA_PATH") == 0);
    CFG_sync(); equal_file("zzStillHere=1\n");
    assert(setenv("SHARED_USERDATA_PATH", "", 1) == 0);
    CFG_sync(); equal_file("zzStillHere=1\n");
    puts("ok a missing or empty save path leaves settings untouched");
    return 0;
}
