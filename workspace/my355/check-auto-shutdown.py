#!/usr/bin/env python3
"""Exercise the shipped my355 RTC policy against host fixtures. No device I/O."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import tempfile

REPO = Path(__file__).resolve().parents[2]
SUSPEND = REPO / "skeleton/SYSTEM/my355/bin/suspend"
SOURCE = SUSPEND.read_text()
START = "# BEGIN_MY355_AUTO_OFF_POLICY\n"
END = "# END_MY355_AUTO_OFF_POLICY\n"
assert SOURCE.count(START) == SOURCE.count(END) == 1
policy_text = SOURCE.split(START, 1)[1].split(END, 1)[0]
# Source exactly the production function block, never a separate implementation.
_policy_directory = tempfile.TemporaryDirectory(prefix="flip-production-policy-")
POLICY = Path(_policy_directory.name) / "policy.sh"
POLICY.write_text(policy_text)
subprocess.run(["sh", "-n", str(SUSPEND)], check=True)
arm = SOURCE.index("\t\tfa_prepare\n")
mem = SOURCE.index("\t\techo mem > /sys/power/state\n", arm)
classify = SOURCE.index('\t\tfa_after_suspend "$sleep_retval"\n', mem)
lid = SOURCE.index('\t\tif [ $sleep_retval -ne 0 ] || lid_open;', classify)
shutdown = SOURCE.index("\t\t\texit 42\n", classify)
assert arm < mem < classify < shutdown < lid
assert SOURCE.index("\nfa_finish\n\nlog \"Resuming...\"\n") < SOURCE.index("\nafter &\n")
assert SOURCE.index("--clear-auto-shutdown-alarm") < SOURCE.index("\nbefore\n")
assert 'fa_timeout=0' in SOURCE and 'fa_init "${fa_timeout:-0}"' in SOURCE
assert "trap 'fa_finish' EXIT" in SOURCE
assert "trap 'fa_signal_resume' INT TERM HUP" in SOURCE
launch = (REPO / "skeleton/SYSTEM/my355/paks/MinUI.pak/launch.sh").read_text()
assert launch.index('"$SYSTEM_PATH/bin/suspend" --clear-auto-shutdown-alarm') < launch.index("keymon.elf &")
assert "#define HAS_SLEEP_AUTO_POWER_OFF 1" in (REPO / "workspace/my355/platform/platform.h").read_text()
assert "#define CFG_DEFAULT_AUTOSHUTDOWNTIMEOUTSECS 0" in (REPO / "workspace/all/common/config.h").read_text()
print("PASS native suspend placement, clean exit42 ordering, boot ownership cleanup, signal cleanup and Never default")

PRELUDE = r'''
set -eu
. "$POLICY"
# The host may lack GNU/BusyBox timeout. Fixtures are regular files; the real
# probe and production candidate require the device's confirmed timeout binary.
timeout() { shift; "$@"; }
sync() { :; }
fa_rtc_root="$FIXTURE/rtc"
fa_interrupts="$FIXTURE/interrupts"
fa_hall="$FIXTURE/hall"
fa_supply="$FIXTURE/supply"
fa_marker="$FIXTURE/marker"
fa_lock="$FIXTURE/lock"
fa_read() { cat "$1" 2>/dev/null; }
rtc="$fa_rtc_root/rtc0"
irq() {
    printf ' CPU0 CPU1 CPU2 CPU3\n' > "$fa_interrupts"
    n=1
    for handler in 'RTC alarm' hall_mh248 rk805_pwrkey_fall rk805_pwrkey_rise rk817_plug_in rk817_plug_out; do
        eval "count=\${$n}"
        printf ' %s: %s 0 0 0 rk817 5 Edge %s\n' "$((n+77))" "$count" "$handler" >> "$fa_interrupts"
        n=$((n+1))
    done
}
start() {
    fa_init 60
    fa_prepare
    [ "$fa_active:$fa_owned:$fa_deadline" = 1:1:160 ]
    [ "$(cat "$rtc/wakealarm")" = 160 ]
}
expire() {
    printf '160\n' > "$rtc/since_epoch"
    : > "$rtc/wakealarm"
    irq 1 0 0 0 0 0
}
irq 0 0 0 0 0 0
'''

CASES = {
    "Never performs no reads, writes or locking": r'''
        fa_read() { echo unexpected-read >&2; return 1; }
        fa_init 0; fa_prepare; fa_after_suspend 0; fa_finish
        [ "$fa_active" = 0 ] && [ ! -e "$fa_marker" ] && [ ! -e "$fa_lock" ]
    ''',
    "only confirmed battery alarm requests shutdown": r'''
        start; expire; fa_after_suspend 0
        [ "$fa_decision" = shutdown ]
        fa_finish; [ ! -e "$fa_marker" ] && [ ! -e "$fa_lock" ]
    ''',
    "elapsed deadline with shared parent event is insufficient": r'''
        start; expire; irq 0 0 0 0 0 0
        printf '72: 900 0 0 0 rockchip_gpio_irq 3 Level rk817\n' >> "$fa_interrupts"
        fa_after_suspend 0; [ "$fa_decision" = defer ]
    ''',
    "unconsumed alarm cannot request shutdown": r'''
        start; expire; echo 160 > "$rtc/wakealarm"
        fa_after_suspend 0; [ "$fa_decision" = defer ]
    ''',
    "early RTC interrupt cannot request shutdown": r'''
        start; expire; echo 159 > "$rtc/since_epoch"
        fa_after_suspend 0; [ "$fa_decision" = ordinary ]
    ''',
    "failed suspend cannot request shutdown": r'''
        start; expire; fa_after_suspend 1
        [ "$fa_decision:$fa_active" = ordinary:0 ]
    ''',
    "normal early wake clears only owned alarm": r'''
        start; echo 120 > "$rtc/since_epoch"; irq 0 0 1 1 0 0
        fa_after_suspend 0; [ "$fa_decision" = ordinary ]
        fa_finish
        [ "$(cat "$rtc/wakealarm")" = 0 ] && [ ! -e "$fa_marker" ]
    ''',
    "ordinary closed-lid charger wake retains original deadline": r'''
        start; echo 120 > "$rtc/since_epoch"; irq 0 0 0 0 1 0
        fa_after_suspend 0; fa_prepare
        [ "$fa_deadline" = 160 ] && [ "$(cat "$rtc/wakealarm")" = 160 ]
    ''',
    "charger expiry starts a fresh full interval": r'''
        start; expire; echo 1 > "$fa_supply/ac/online"
        fa_after_suspend 0; [ "$fa_decision" = defer ]
        fa_prepare; [ "$fa_deadline" = 220 ] && [ "$(cat "$rtc/wakealarm")" = 220 ]
    ''',
    "replacement alarm survives normal cleanup": r'''
        start; echo 999 > "$rtc/wakealarm"; fa_finish
        [ "$(cat "$rtc/wakealarm")" = 999 ] && [ ! -e "$fa_marker" ]
    ''',
    "failed deferred cleanup preserves ownership deadline and marker": r'''
        start; expire; echo 1 > "$fa_supply/ac/online"
        fa_clear_owned() { return 0; }
        fa_after_suspend 0
        [ "$fa_active:$fa_owned:$fa_deadline:$fa_decision" = 0:1:160:defer ] &&
            [ "$(cat "$fa_marker")" = 'rtc0 160' ]
    ''',
    "failed final ownership cleanup vetoes confirmed shutdown": r'''
        start; expire
        fa_clear_owned() { return 0; }
        fa_after_suspend 0
        [ "$fa_active:$fa_owned:$fa_decision" = 0:1:ordinary ] && [ -e "$fa_marker" ]
    ''',
    "preexisting alarm is not claimed": r'''
        echo 999 > "$rtc/wakealarm"; fa_init 60; fa_prepare
        [ "$fa_active" = 0 ] && [ "$(cat "$rtc/wakealarm")" = 999 ] && [ ! -e "$fa_marker" ]
    ''',
    "disabled wakeup disables feature without changing ABI": r'''
        echo disabled > "$rtc/device/power/wakeup"; fa_init 60; fa_prepare
        [ "$fa_active" = 0 ] && [ "$(cat "$rtc/device/power/wakeup")" = disabled ]
    ''',
    "duplicate RTCs disable feature": r'''
        cp -R "$rtc" "$fa_rtc_root/rtc2"; fa_init 60; fa_prepare
        [ "$fa_active" = 0 ]
    ''',
    "missing alarm child fails closed": r'''
        start; expire; sed '/RTC alarm/d' "$fa_interrupts" > "$fa_interrupts.new"
        mv "$fa_interrupts.new" "$fa_interrupts"; fa_after_suspend 0
        [ "$fa_active:$fa_decision" = 0:ordinary ]
    ''',
    "duplicate alarm child fails closed": r'''
        start; expire; echo '99: 2 0 0 0 rk817 5 Edge RTC alarm' >> "$fa_interrupts"
        fa_after_suspend 0; [ "$fa_active:$fa_decision" = 0:ordinary ]
    ''',
    "missing RTC time fails closed and cancels owned alarm": r'''
        start; rm "$rtc/since_epoch"; fa_after_suspend 0
        [ "$fa_active:$fa_decision" = 0:ordinary ] && [ "$(cat "$rtc/wakealarm")" = 0 ]
    ''',
    "counter reset cannot request shutdown": r'''
        irq 8 0 0 0 0 0; start; expire; fa_after_suspend 0
        [ "$fa_decision" = defer ]
    ''',
    "all CPU columns are summed without hardcoded IRQ": r'''
        start; expire
        sed 's/78: 1 0 0 0/432: 0 0 0 0 0 0 0 1/' "$fa_interrupts" > "$fa_interrupts.new"
        mv "$fa_interrupts.new" "$fa_interrupts"; fa_after_suspend 0
        [ "$fa_decision" = shutdown ]
    ''',
    "boot cleanup clears exact marker match": r'''
        printf 'rtc0 999\n' > "$fa_marker"; echo 999 > "$rtc/wakealarm"
        fa_boot_cleanup; [ "$(cat "$rtc/wakealarm")" = 0 ] && [ ! -e "$fa_marker" ]
    ''',
    "boot cleanup preserves replacement": r'''
        printf 'rtc0 999\n' > "$fa_marker"; echo 1000 > "$rtc/wakealarm"
        fa_boot_cleanup; [ "$(cat "$rtc/wakealarm")" = 1000 ] && [ ! -e "$fa_marker" ]
    ''',
    "boot cleanup without marker does no RTC reads": r'''
        fa_read() { echo unexpected-read >&2; return 1; }
        fa_boot_cleanup
    ''',
    "malformed boot marker is not interpreted as a path": r'''
        printf '../../etc 999\n' > "$fa_marker"; echo 1000 > "$rtc/wakealarm"
        fa_boot_cleanup; [ "$(cat "$rtc/wakealarm")" = 1000 ]
    ''',
    "concurrent sleep policy is refused": r'''
        mkdir "$fa_lock"; fa_init 60; fa_prepare
        [ "$fa_active" = 0 ] && [ ! -e "$fa_marker" ]
    ''',
    "unreadable arming readback cannot request shutdown": r'''
        fa_init 60
        fa_read() {
            if [ "$1" = "$fa_rtc/wakealarm" ] && [ -e "$fa_marker" ]; then return 1; fi
            cat "$1" 2>/dev/null
        }
        fa_prepare; fa_after_suspend 0
        [ "$fa_active:$fa_decision" = 0:ordinary ] && [ -e "$fa_marker" ]
    ''',
    "failed ownership persistence prevents RTC arming": r'''
        fa_init 60; sync() { return 1; }; fa_prepare
        [ "$fa_active:$fa_owned" = 0:0 ] &&
            [ ! -s "$rtc/wakealarm" ] && [ ! -e "$fa_marker" ]
    ''',
}

for field in range(2, 7):
    values = [1, 0, 0, 0, 0, 0]
    values[field - 1] = 1
    CASES[f"simultaneous competing child {field} prevents shutdown"] = (
        "start; expire; irq " + " ".join(map(str, values))
        + '; fa_after_suspend 0; [ "$fa_decision" = defer ]'
    )
for supply in ("ac", "usb"):
    for value in ("1", "unknown", "", "2"):
        CASES[f"{supply} online={value!r} prevents shutdown"] = (
            f'start; expire; printf "%s\\n" {shlex.quote(value)} > "$fa_supply/{supply}/online"; '
            'fa_after_suspend 0; [ "$fa_decision" = defer ]'
        )
    CASES[f"missing {supply} state prevents shutdown"] = (
        f'start; expire; rm "$fa_supply/{supply}/online"; '
        'fa_after_suspend 0; [ "$fa_decision" = defer ]'
    )
for invalid in ("-1", "abc", "060", "999999999999999999999999999999", "28801"):
    CASES[f"invalid timeout {invalid!r} is inert"] = (
        f'fa_init {shlex.quote(invalid)}; fa_prepare; [ "$fa_active" = 0 ] && [ ! -e "$fa_marker" ]'
    )

# Exercise the actual signal finalizer plus native service/hook bookkeeping.
native_after = re.search(r"^after\(\) \{\n.*?^\}", SOURCE, re.M | re.S).group(0)
native_signal = re.search(r"^fa_signal_resume\(\) \{\n.*?^\}", SOURCE, re.M | re.S).group(0)
CASES["enabled-session signal resumes native services/hooks and clears markers"] = (
    r'''
    start
    export SYSTEM_PATH="$FIXTURE/system"
    export FIXTURE
    mkdir -p "$SYSTEM_PATH/bin" "$SYSTEM_PATH/etc/wifi" "$SYSTEM_PATH/etc/bluetooth"
    for script in bin/run_hooks.sh etc/wifi/wifi_init.sh etc/bluetooth/bt_init.sh; do
        printf '%s\n' '#!/bin/sh' 'printf "%s %s\n" "$0" "$*" >> "$FIXTURE/resume-events"' > "$SYSTEM_PATH/$script"
        chmod +x "$SYSTEM_PATH/$script"
    done
    touch "$FIXTURE/system_suspend"
    wifid_running=1; bluetoothd_running=1; fa_resume_started=0
    log() { :; }
    '''
    + native_after.replace("/tmp/system_suspend", '"$FIXTURE/system_suspend"')
    + "\n" + native_signal + "\nfa_signal_resume\n"
)

failures = []
for name, body in CASES.items():
    with tempfile.TemporaryDirectory(prefix="flip-autooff-policy-") as temp:
        root = Path(temp)
        files = {
            "rtc/rtc0/name": "rk808-rtc rk808-rtc\n",
            "rtc/rtc0/device/power/wakeup": "enabled\n",
            "rtc/rtc0/wakealarm": "",
            "rtc/rtc0/since_epoch": "100\n",
            "hall": "0\n",
            "supply/ac/online": "0\n",
            "supply/usb/online": "0\n",
        }
        for path, value in files.items():
            target = root / path
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(value)
        result = subprocess.run(
            ["sh", "-c", PRELUDE + body],
            env=dict(os.environ, POLICY=str(POLICY), FIXTURE=str(root)),
            text=True, capture_output=True, timeout=5,
        )
        if "signal resumes native" in name and result.returncode == 0:
            events = (root / "resume-events").read_text()
            assert "post-resume.d" in events and "wifi_init.sh start" in events and "bt_init.sh start" in events
            assert not (root / "system_suspend").exists()
            assert not (root / "marker").exists() and not (root / "lock").exists()
            assert (root / "rtc/rtc0/wakealarm").read_text().strip() == "0"
        if result.returncode:
            failures.append(name)
            print(f"FAIL {name}: {result.stdout}{result.stderr}")
        else:
            print(f"PASS {name}")

print(f"{len(CASES) - len(failures)}/{len(CASES)} production policy cases passed (host fixtures only)")
raise SystemExit(bool(failures))
