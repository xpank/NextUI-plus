#!/usr/bin/env python3
"""Exercise the launcher's shutdown policy with shell stubs, never hardware.

Only the shutdown function and sentinel blocks are executed. Absolute BaseOS
helper paths are mapped into a disposable directory; PATH contains only stubs.
Run with: python3 tests/test-h700-shutdown.py
"""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest


LAUNCHER = Path(__file__).resolve().parents[1] / "skeleton/SYSTEM/h700/paks/MinUI.pak/launch.sh"
SOURCE = LAUNCHER.read_text()
FUNCTION = re.search(r"(?ms)^h700_shutdown\(\) \{\n.*?^\}", SOURCE).group()


class ShutdownPolicy(unittest.TestCase):
    def run_shutdown(self, action, *, modern=None, executable=True,
                     legacy=0, fallback=0, request=None):
        with tempfile.TemporaryDirectory(prefix="nextui-shutdown-") as directory:
            root = Path(directory)
            log = root / "calls"
            environment = dict(os.environ, PATH=directory, CALL_LOG=str(log))

            def stub(name, status):
                path = root / name
                path.write_text(f'#!/bin/sh\nprintf "%s\\n" "{name}" >> "$CALL_LOG"\nexit {status}\n')
                path.chmod(0o755)
                return path

            for mode in ("poweroff", "reboot"):
                stub(mode + "_next", legacy)
                stub(mode, fallback)
                # A similarly named helper on PATH must never be selected.
                stub("baseos-" + mode, 99)
                if modern is not None:
                    helper = stub("absolute-baseos-" + mode, modern)
                    if not executable:
                        helper.chmod(0o644)

            function = FUNCTION.replace("/usr/sbin/baseos-", directory + "/absolute-baseos-")
            if request is None:
                request = 'h700_shutdown "$1"'
            else:
                (root / action).touch()
                request = request.replace("/tmp/", directory + "/")
            script = function + "\n" + request + '\nprintf "continued\\n" >> "$CALL_LOG"\n'
            result = subprocess.run(["/bin/sh", "-c", script, "shutdown-test", action],
                                    env=environment, capture_output=True, text=True, timeout=5)
            calls = log.read_text().splitlines() if log.exists() else []
            return result.returncode, calls

    def test_modern_helpers_own_success_and_failure(self):
        for action in ("poweroff", "reboot"):
            for status in (0, 7):
                with self.subTest(action=action, status=status):
                    self.assertEqual(self.run_shutdown(action, modern=status),
                                     (status, ["absolute-baseos-" + action]))

    def test_missing_or_nonexecutable_helpers_keep_legacy_behavior(self):
        for action in ("poweroff", "reboot"):
            for modern, executable in ((None, True), (0, False)):
                for legacy, fallback in ((0, 0), (1, 0), (1, 9)):
                    with self.subTest(action=action, modern=modern, executable=executable,
                                      legacy=legacy, fallback=fallback):
                        expected = [action + "_next"]
                        if legacy:
                            expected.append(action)
                        self.assertEqual(self.run_shutdown(action, modern=modern,
                                                          executable=executable,
                                                          legacy=legacy, fallback=fallback),
                                         (fallback if legacy else 0, expected))

    def test_every_launcher_request_uses_same_policy(self):
        blocks = list(re.finditer(
            r'(?m)^(?P<indent>\t*)if \[ -f "/tmp/(?P<action>poweroff|reboot)" \]; then\n'
            r'(?P=indent)\th700_shutdown (?P=action)\n(?P=indent)fi$', SOURCE))
        self.assertEqual([block.group("action") for block in blocks],
                         ["poweroff", "reboot", "poweroff", "reboot"])
        requests = [(block.group("action"), block.group()) for block in blocks]
        self.assertTrue(SOURCE.rstrip().endswith("h700_shutdown poweroff"))
        requests.append(("poweroff", "h700_shutdown poweroff"))
        for index, (action, request) in enumerate(requests):
            with self.subTest(request=index, action=action):
                self.assertEqual(self.run_shutdown(action, modern=0, request=request),
                                 (0, ["absolute-baseos-" + action]))
        # No launcher path may invoke the PMIC helper outside this policy.
        remaining_source = SOURCE.replace(FUNCTION, "")
        self.assertNotIn("poweroff_next", remaining_source)
        self.assertNotIn("reboot_next", remaining_source)


if __name__ == "__main__":
    unittest.main()
