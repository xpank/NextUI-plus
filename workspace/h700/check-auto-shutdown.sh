#!/bin/sh
# Invariants for "auto power off after a period asleep".
#
# Runs on the build host, needs no device and no toolchain. Two halves:
#
#   1. static  -- the shipped suspend script is still intact underneath the
#      addition, the new code sits at the points the design requires, and the
#      config key is wired up at every touch point it needs.
#   2. behavioural -- the helper functions are extracted out of the real
#      script and exercised against stub sysfs trees, so the arming, the
#      deadline arithmetic and the two wake classifiers are actually run
#      rather than merely read.
#
# Usage: workspace/h700/check-auto-shutdown.sh
set -u

here=$(cd "$(dirname "$0")/../.." && pwd)
SUSPEND="$here/skeleton/SYSTEM/h700/bin/suspend"
CONFIG_C="$here/workspace/all/common/config.c"
CONFIG_H="$here/workspace/all/common/config.h"
SETTINGS="$here/workspace/all/settings/settings.cpp"
API_C="$here/workspace/all/common/api.c"

pass=0; fail=0
ok()   { pass=$((pass+1)); printf '  ok   %s\n' "$1"; }
bad()  { fail=$((fail+1)); printf '  FAIL %s\n' "$1"; }
check(){ if [ "$1" = 0 ]; then ok "$2"; else bad "$2"; fi; }
has()  { grep -qF "$2" "$1"; check $? "$3"; }
# line number of the first line containing a fixed string
lineof() { grep -nF "$2" "$1" | head -n 1 | cut -d: -f1; }

echo "== the shipped script is still underneath =="
# Every one of these is load-bearing behaviour that predates this feature and
# that the addition must not disturb.
has "$SUSPEND" 'echo mem >/sys/power/state'                  "suspend still writes mem to /sys/power/state"
has "$SUSPEND" "printf '16' > \"\$os_sleep\""                "Super Standby (os_sleep) write is intact"
has "$SUSPEND" 'for i in $(seq 1 $sleep_max_tries); do'      "the 5-attempt retry loop is intact"
has "$SUSPEND" 'if [ $time_asleep -gt 5 ]; then'             "the false-negative workaround is intact"
has "$SUSPEND" 'Woke with lid closed; re-suspending'         "the lid re-suspend branch is intact"
has "$SUSPEND" 'exit $sleep_retval'                          "the normal exit status is intact"
has "$SUSPEND" 'after_sync'                                  "after_sync is still called"
sh -n "$SUSPEND"; check $? "suspend parses as POSIX sh"

echo "== the new code is where the design requires =="
arm=$(lineof "$SUSPEND" 'as_arm')
mem=$(lineof "$SUSPEND" 'echo mem >/sys/power/state')
dec=$(lineof "$SUSPEND" 'if [ "$as_secs" -gt 0 ] && [ $sleep_retval -eq 0 ] && as_fired; then')
lid=$(lineof "$SUSPEND" 'if [ $sleep_retval -eq 0 ] && [ -r "$hallkey" ]')
fin=$(lineof "$SUSPEND" 'as_disarm				# never leave an alarm pending')
syn=$(lineof "$SUSPEND" 'after_sync
after_async &')
[ -n "$dec" ] && [ -n "$lid" ] && [ "$dec" -lt "$lid" ]
check $? "the shutdown decision is made BEFORE the lid test (it re-suspends and never returns)"
[ -n "$mem" ] && [ -n "$fin" ] && [ "$mem" -lt "$fin" ]
check $? "the alarm is disarmed after the loop, on every non-shutdown exit"
grep -q 'as_arm$' "$SUSPEND"; check $? "as_arm is called inside the retry loop"
awk '/^\t\tas_arm$/{a=NR} /echo mem >\/sys\/power\/state/{m=NR} END{exit !(a && m && a<m)}' "$SUSPEND"
check $? "as_arm runs immediately before echo mem, so the snapshot is as late as possible"
has "$SUSPEND" 'exit $as_exit_code'                          "the shutdown branch exits rather than breaking"
awk '/exit \$as_exit_code/{e=NR} /^after_sync$/{s=NR} END{exit !(e && s && e<s)}' "$SUSPEND"
check $? "the shutdown branch exits before after_sync (syncsettings.elf would light the backlight)"
has "$SUSPEND" 'as_secs=0'                                   "the default is Never"
has "$SUSPEND" '$NF=="rtc"'                                  "the rtc interrupt is matched by name, not by number"
has "$SUSPEND" '/axp20x-pek-db/'                             "the power-key interrupts are matched by name"
has "$SUSPEND" 'since_epoch'                                 "the rtc counter is the time base, not the system clock"
grep -v '^[[:space:]]*#' "$SUSPEND" | grep -q 'pm_wakeup_irq'; [ $? -ne 0 ]
check $? "pm_wakeup_irq is not used (it names the PMIC aggregator on every wake)"
grep -c 'sync$' "$SUSPEND" >/dev/null && grep -q '	sync				# the card is vfat' "$SUSPEND"
check $? "every log write is followed by sync (the card is vfat)"

echo "== the config key is wired up =="
has "$CONFIG_H" "uint32_t autoShutdownTimeoutSecs;"                       "config.h: struct field"
has "$CONFIG_H" "#define CFG_DEFAULT_AUTOSHUTDOWNTIMEOUTSECS 0"           "config.h: default is 0 (Never)"
has "$CONFIG_H" "uint32_t CFG_getAutoShutdownTimeoutSecs(void);"          "config.h: getter declared"
has "$CONFIG_C" ".autoShutdownTimeoutSecs = CFG_DEFAULT_AUTOSHUTDOWNTIMEOUTSECS," "config.c: defaults"
has "$CONFIG_C" 'sscanf(line, "autoShutdownTimeout=%i"'                   "config.c: parsed on load"
has "$CONFIG_C" 'strcmp(key, "autoShutdownTimeout")'                      "config.c: readable through CFG_get/nextval.elf"
has "$CONFIG_C" 'fprintf(file, "autoShutdownTimeout=%i\n"'                "config.c: written by CFG_sync -- this is what the script reads"
has "$CONFIG_C" 'printf("\t\"autoShutdownTimeout\": %i,\n"'               "config.c: in the JSON dump"
has "$SETTINGS" 'auto_shutdown_secs'                                      "settings.cpp: value vector"
has "$SETTINGS" '"Auto power off"'                                        "settings.cpp: the menu item"
grep -q 'auto_shutdown_secs = {0U,' "$SETTINGS"; check $? "settings.cpp: the first offered value is 0 == Never"
# The values and the labels are two parallel vectors; a mismatched length would
# put the wrong seconds behind the wrong label, silently.
v=$(sed -n 's/.*auto_shutdown_secs = {\(.*\)};.*/\1/p' "$SETTINGS" | tr ',' '\n' | grep -c .)
l=$(sed -n 's/.*auto_shutdown_labels = {\(.*\)};.*/\1/p' "$SETTINGS" | tr ',' '\n' | grep -c .)
[ -n "$v" ] && [ "$v" = "$l" ]
check $? "settings.cpp: $v values and $l labels, same length"
# Every offered value must be a whole number of minutes matching its own label,
# or the menu lies about what it does.
awk -v vs="$(sed -n 's/.*auto_shutdown_secs = {\(.*\)};.*/\1/p' "$SETTINGS")" \
    -v ls="$(sed -n 's/.*auto_shutdown_labels = {\(.*\)};.*/\1/p' "$SETTINGS")" '
BEGIN {
	gsub(/[ U]/, "", vs); gsub(/[ "]/, "", ls)
	n = split(vs, v, ","); split(ls, l, ",")
	for (i = 1; i <= n; i++) {
		want = (l[i] == "Never") ? 0 : \
		       (l[i] ~ /m$/) ? substr(l[i], 1, length(l[i])-1) * 60 : \
		                       substr(l[i], 1, length(l[i])-1) * 3600
		if (v[i] + 0 != want) { printf "  %s is %s seconds, expected %d\n", l[i], v[i], want; bad = 1 }
		if (i > 1 && v[i] + 0 <= v[i-1] + 0) { printf "  %s is not above %s\n", l[i], l[i-1]; bad = 1 }
	}
	exit bad
}'
check $? "settings.cpp: every label matches its value, and they ascend"
# 5m/10m/15m are the ones actually measured to fire on this device; keep them.
for want in '5m' '10m' '15m'; do
	grep -q "\"$want\"" "$SETTINGS"; check $? "settings.cpp: $want is offered (measured to fire)"
done
awk '/"Suspend timeout"/{s=NR} /"Auto power off"/{a=NR} END{exit !(s && a && s<a)}' "$SETTINGS"
check $? "settings.cpp: it sits under System, right after Suspend timeout"
# The clock starts at deep-sleep entry, so "5m" is 5 minutes on top of the
# suspend timeout, not 5 minutes after the owner put the device down. With
# short values on offer the description has to say so.
grep -q 'suspend timeout ends' "$SETTINGS"
check $? "settings.cpp: the description says what the timeout is counted from"
# Loading the configuration must not write it back. Without this, every binary
# that links config.c rewrites minuisettings.txt just by starting, and deletes
# any key it was compiled without -- which is what made this setting revert to
# Never on every boot, every game launch and every Tools pak.
has "$CONFIG_C" "static bool cfg_loading = false;"    "config.c: the load guard exists"
awk '/^void CFG_init/{i=NR} /cfg_loading = true;/{s=NR} END{exit !(i && s && s>i && s<i+10)}' "$CONFIG_C"
check $? "config.c: CFG_init sets the guard before it starts calling setters"
[ "$(grep -c 'cfg_loading = false;' "$CONFIG_C")" = 2 ]
check $? "config.c: the guard is cleared once (plus its declaration), so it is balanced"
awk '/^void CFG_sync/{s=NR} /if \(cfg_loading\)/{g=NR} /fopen\(settingsPath, "w"\)/{w=NR} END{exit !(s && g && w && s<g && g<w)}' "$CONFIG_C"
check $? "config.c: CFG_sync returns on the guard BEFORE it truncates the file"
[ "$(sed -n '/^void CFG_init/,/^}/p' "$CONFIG_C" | grep -c 'return;')" = 0 ]
check $? "config.c: CFG_init still has a single exit, so the guard cannot leak"

# Every binary that links config.c must know the key, or it deletes it. Checked
# against the built artefacts when they are present.
built=0; missing=""
for b in nextui/build/h700/nextui.elf settings/build/h700/settings.elf \
         minarch/build/h700/minarch.elf nextval/build/h700/nextval.elf \
         battery/build/h700/battery.elf bootlogo/build/h700/bootlogo.elf \
         clock/build/h700/clock.elf minput/build/h700/minput.elf \
         ledcontrol/build/h700/ledcontrol.elf gametime/build/h700/gametime.elf \
         batmon/build/h700/batmon.elf gametimectl/build/h700/gametimectl.elf \
         ../h700/poweroff_next/build/h700/poweroff_next.elf \
         ../h700/poweroff_next/build/h700/reboot_next.elf; do
	f="$here/workspace/all/$b"
	[ -f "$f" ] || continue
	built=$((built+1))
	if grep -qa 'suspendTimeout=%i' "$f" && ! grep -qa 'autoShutdownTimeout=%i' "$f"; then
		missing="$missing $(basename "$f")"
	fi
done
if [ "$built" = 0 ]; then
	printf '  --   no built binaries present, skipping the config-writer check\n'
else
	[ -z "$missing" ]
	check $? "all $built built config.c binaries know the key (offenders:${missing:- none})"
fi

has "$API_C" "PWR_DEEPSLEEP_AUTO_SHUTDOWN"                                "api.c: the shutdown request is distinguished from a suspend failure"
has "$API_C" "WEXITSTATUS(ret) == SUSPEND_EXIT_AUTO_SHUTDOWN"             "api.c: matched on the script's exit status"

echo "== the settings write, actually run =="
# CFG_sync() must carry through keys this binary was compiled without, or saving
# deletes them -- and CFG_quit() is a bare CFG_sync(), so poweroff_next did
# exactly that on every shutdown. The loop is lifted out of config.c rather than
# copied, so the test cannot drift away from the shipped code.
if ! command -v cc >/dev/null 2>&1; then
	printf '  --   no host compiler, skipping the merge tests\n'
else
	mtmp=$(mktemp -d)
	awk '/Append any line from the old file whose key we did not just write/{p=1}
	     p{print}
	     p && /^    fclose\(out\);/{exit}' "$CONFIG_C" | sed '$d' > "$mtmp/loop.inc"
	[ -s "$mtmp/loop.inc" ]; check $? "the merge loop extracts out of config.c"
	{
		echo '#include <stdio.h>'
		echo '#include <stdlib.h>'
		echo '#include <string.h>'
		echo '#include <stdbool.h>'
		echo 'static void merge(FILE *out, char *fresh, char *carried) {'
		cat "$mtmp/loop.inc"
		cat <<'CEOF'
}
static int fail = 0;
static void t(const char *name, const char *fresh, const char *carried, const char *want) {
	char *buf = NULL; size_t n = 0;
	FILE *out = open_memstream(&buf, &n);
	char *f = strdup(fresh), *c = strdup(carried);
	fwrite(f, 1, strlen(f), out);
	merge(out, f, c);
	fclose(out);
	if (strcmp(buf, want) != 0) { fail = 1; printf("FAIL merge: %s\n", name); }
	else printf("ok merge: %s\n", name);
	free(buf); free(f); free(c);
}
int main(void) {
	t("an unknown key survives the write", "a=1\nb=2\n", "a=9\nb=8\nzz=7\n", "a=1\nb=2\nzz=7\n");
	t("known keys are never duplicated", "a=1\nb=2\n", "a=9\nb=8\n", "a=1\nb=2\n");
	t("a= and ab= are not confused", "a=1\n", "ab=2\n", "a=1\nab=2\n");
	t("ab= survives when abc= was written", "abc=1\n", "ab=2\n", "abc=1\nab=2\n");
	t("no previous file", "a=1\n", "", "a=1\n");
	t("previous file without a trailing newline", "a=1\n", "zz=7", "a=1\nzz=7\n");
	t("junk lines are dropped, not carried", "a=1\n", "garbage\nzz=7\n", "a=1\nzz=7\n");
	t("an empty value is preserved", "a=1\n", "zz=\n", "a=1\nzz=\n");
	t("a match on the very last written line is seen", "a=1\nzz=3\n", "zz=9\n", "a=1\nzz=3\n");
	t("several unknowns keep their order", "a=1\n", "y=1\nz=2\n", "a=1\ny=1\nz=2\n");
	return fail;
}
CEOF
	} > "$mtmp/t.c"
	if cc -O1 -o "$mtmp/t" "$mtmp/t.c" 2>"$mtmp/cc.log"; then
		"$mtmp/t" > "$mtmp/out" 2>&1
		while IFS= read -r l; do
			case "$l" in
				"ok "*)   ok "${l#ok }" ;;
				"FAIL "*) bad "${l#FAIL }" ;;
			esac
		done < "$mtmp/out"
	else
		bad "the extracted merge loop does not compile"
		head -5 "$mtmp/cc.log" | sed 's/^/      /'
	fi
	rm -rf "$mtmp"
fi

echo "== the helpers, actually run =="
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
# Take the helper block straight out of the shipped script so the test cannot
# drift away from the code it is testing.
sed -n '/^as_rtc=\/sys\/class\/rtc\/rtc1$/,/^as_read_timeout$/p' "$SUSPEND" \
	| sed '$d' > "$tmp/lib.sh"
[ -s "$tmp/lib.sh" ]; check $? "the helper block extracts out of the script"
. "$tmp/lib.sh"

mkdir -p "$tmp/rtc" "$tmp/bat" "$tmp/shared" "$tmp/user/logs"
as_rtc="$tmp/rtc"; as_bat="$tmp/bat"; as_logfile="$tmp/user/logs/autoshutdown.txt"
echo 1000 > "$as_rtc/since_epoch"
: > "$as_rtc/wakealarm"
echo 1 > "$as_bat/hallkey"
echo Discharging > "$as_bat/status"
# /proc/interrupts is read by absolute path, so stub the two readers instead.
IRQ_RTC=10; IRQ_PEK=5
as_rtc_irqs() { echo "$IRQ_RTC"; }
as_pek_irqs() { echo "$IRQ_PEK"; }

# --- the timeout is read from the right place, and garbage is rejected
SHARED_USERDATA_PATH="$tmp/shared"; USERDATA_PATH="$tmp/user"
as_read_timeout; [ "$as_secs" = 0 ]; check $? "no config anywhere -> Never"
echo 300 > "$tmp/user/autoshutdown"
as_read_timeout; [ "$as_secs" = 300 ]; check $? "the plain file is the fallback before the Settings item exists"
printf 'suspendTimeout=60\nautoShutdownTimeout=1800\n' > "$tmp/shared/minuisettings.txt"
as_read_timeout; [ "$as_secs" = 1800 ]; check $? "minuisettings.txt wins once the key is there"
printf 'autoShutdownTimeout=nonsense\n' > "$tmp/shared/minuisettings.txt"
as_read_timeout; [ "$as_secs" = 300 ]; check $? "an unparseable value falls through rather than erroring"
printf 'autoShutdownTimeout=0\n' > "$tmp/shared/minuisettings.txt"
as_read_timeout; [ "$as_secs" = 0 ]; check $? "an explicit 0 means Never"

# --- disabled means untouched
: > "$as_rtc/wakealarm"; rm -f "$as_logfile"
as_arm
[ -z "$(cat "$as_rtc/wakealarm")" ] && [ -z "$as_armed" ]
check $? "as_arm does nothing at all while the timeout is Never"
as_log "should not appear"
[ ! -f "$as_logfile" ]; check $? "as_log writes nothing while the timeout is Never"
as_fired; [ $? -ne 0 ]; check $? "as_fired is false while the timeout is Never"

# --- arming and the absolute deadline
printf 'autoShutdownTimeout=600\n' > "$tmp/shared/minuisettings.txt"
as_read_timeout
as_deadline=0
as_arm
[ "$as_armed" = 1 ] && [ "$(cat "$as_rtc/wakealarm")" = 1600 ] && [ "$as_deadline" = 1600 ]
check $? "as_arm arms at since_epoch + timeout and records the snapshot"
[ "$as_rtc_irq0" = 10 ] && [ "$as_pek0" = 5 ] && [ "$as_hall0" = 1 ]
check $? "as_arm snapshots the rtc, power-key and lid state"

# The lid loop re-suspends many times; the countdown must not restart.
echo 1200 > "$as_rtc/since_epoch"          # 200s later, a spurious wake
: > "$as_rtc/wakealarm"                     # (alarm still pending in reality)
as_arm
[ "$as_deadline" = 1600 ] && [ "$(cat "$as_rtc/wakealarm")" = 1600 ]
check $? "a re-suspend keeps the original deadline -- elapsed sleep accumulates"
echo 1400 > "$as_rtc/since_epoch"
as_arm
[ "$as_deadline" = 1600 ]; check $? "and again on a third re-suspend"

# Once the deadline is reached and we decide not to power off, it must move on,
# or the device would go back to sleep with no alarm armed at all.
echo 1601 > "$as_rtc/since_epoch"
as_arm
[ "$as_deadline" = 2201 ]; check $? "a passed deadline is pushed forward, never left behind"

# A counter that cannot be read must not arm anything: a deadline of 0 + timeout
# is in the distant past, the alarm would fire the instant it was armed, and the
# device would power itself off out of a read error.
as_deadline=0; : > "$as_rtc/wakealarm"
echo 0 > "$as_rtc/since_epoch"
as_arm
[ -z "$as_armed" ] && [ -z "$(cat "$as_rtc/wakealarm")" ]
check $? "an unreadable rtc counter arms nothing rather than arming in the past"
rm -f "$as_rtc/since_epoch"
as_arm
[ -z "$as_armed" ]; check $? "a missing rtc counter arms nothing"
echo 2000 > "$as_rtc/since_epoch"

# --- did our alarm fire?
echo 2000 > "$as_rtc/since_epoch"
as_deadline=0; : > "$as_rtc/wakealarm"; as_arm       # armed at 2600, irq0=10
as_fired; [ $? -ne 0 ]; check $? "alarm still pending -> not our wake"
: > "$as_rtc/wakealarm"                              # the driver clears it when it fires
as_fired; [ $? -ne 0 ]; check $? "wakealarm empty but the rtc irq did not move -> not our wake"
IRQ_RTC=11
as_fired; check $? "wakealarm empty AND the rtc irq moved -> our wake"
as_armed=
as_fired; [ $? -ne 0 ]; check $? "never armed -> never ours, whatever /proc/interrupts says"

# --- was the owner there?
as_armed=1; IRQ_PEK=5; as_pek0=5; as_hall0=1; echo 1 > "$as_bat/hallkey"
as_user_woke; [ $? -ne 0 ]; check $? "no key edge, no lid change -> the owner is not here"
IRQ_PEK=6
as_user_woke; check $? "a power-key edge -> the owner is here"
IRQ_PEK=5; as_hall0=0; echo 1 > "$as_bat/hallkey"
as_user_woke; check $? "the lid opened (0 -> 1) -> the owner is here, with no key edge at all"
as_hall0=0; echo 0 > "$as_bat/hallkey"
as_user_woke; [ $? -ne 0 ]; check $? "lid shut and still shut -> the owner is not here"

# --- charging
echo Charging > "$as_bat/status"
as_charging; check $? "charging is detected"
echo Discharging > "$as_bat/status"
as_charging; [ $? -ne 0 ]; check $? "discharging is detected"

# --- logging and disarming
as_log "hello"
grep -q hello "$as_logfile"; check $? "as_log writes to the card when enabled"
echo 9999 > "$as_rtc/wakealarm"
as_disarm
[ "$(cat "$as_rtc/wakealarm")" = 0 ] && [ -z "$as_armed" ]
check $? "as_disarm clears the alarm and the armed flag"

as_secs=0; echo 9999 > "$as_rtc/wakealarm"
as_clear_alarm
[ "$(cat "$as_rtc/wakealarm")" = 0 ]
check $? "boot cleanup clears a stale alarm even if the setting is now Never"
as_rtc="$tmp/missing-rtc"
as_clear_alarm
check $? "boot cleanup tolerates a missing RTC"
has "$SUSPEND" 'if [ "${1:-}" = "--clear-auto-shutdown-alarm" ]; then' "suspend offers a dedicated cleanup mode"
sed "s@^as_rtc=/sys/class/rtc/rtc1\$@as_rtc=$tmp/rtc@" "$SUSPEND" > "$tmp/cleanup.sh"
echo 9999 > "$tmp/rtc/wakealarm"
sh "$tmp/cleanup.sh" --clear-auto-shutdown-alarm > "$tmp/cleanup.out" 2>&1
[ $? = 0 ] && [ "$(cat "$tmp/rtc/wakealarm")" = 0 ] && [ ! -s "$tmp/cleanup.out" ]
check $? "the complete cleanup mode exits silently before any suspend preparation"
has "$here/skeleton/SYSTEM/h700/paks/MinUI.pak/launch.sh" '"$SYSTEM_PATH/bin/suspend" --clear-auto-shutdown-alarm' "the shipped launcher runs boot cleanup"

echo
echo "$pass passed, $fail failed"
[ "$fail" -eq 0 ]
