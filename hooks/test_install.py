"""Regression coverage for launching the installed relay in Codex's shell."""

import json
import shlex
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import install


class HookCommandTests(unittest.TestCase):
    def test_posix_command_preserves_arguments(self):
        with patch.object(install.sys, "platform", "linux"), \
                patch.object(install, "python_path", return_value="/opt/Python Tools/python"):
            command = install.hook_command(Path("/tmp/my home/relay.py"), "codex", "http://127.0.0.1/hook")
        self.assertEqual(shlex.split(command), [
            "/opt/Python Tools/python", "-S", "/tmp/my home/relay.py",
            "--agent", "codex", "--url", "http://127.0.0.1/hook",
        ])

    def test_other_windows_agents_keep_their_command(self):
        with patch.object(install.sys, "platform", "win32"), \
                patch.object(install, "python_path", return_value="C:/Python/python.exe"):
            for agent in ("claude", "cursor"):
                command = install.hook_command(Path("relay.py"), agent, "http://127.0.0.1/hook")
                self.assertTrue(command.startswith('"C:/Python/python.exe" -S '))
            command = install.hook_command(Path("relay.py"), "codex", "http://127.0.0.1/hook", "cmd")
            self.assertTrue(command.startswith('"C:/Python/python.exe" -S '))

    @unittest.skipUnless(sys.platform == "win32", "requires Windows PowerShell")
    def test_powershell_launches_relay_with_literal_paths_and_stdin(self):
        with tempfile.TemporaryDirectory(prefix="relay $' ") as folder:
            relay = Path(folder) / ".claude-light" / "relay.py"
            relay.parent.mkdir()
            relay.write_text(
                "import json,sys\n"
                "print(json.dumps({'args':sys.argv[1:],'input':json.load(sys.stdin)}))\n",
                encoding="utf-8",
            )
            url = "http://127.0.0.1/hook?token=$literal'quote"
            command = install.hook_command(relay, "codex", url)
            payload = {"hook_event_name": "PostToolUse", "session_id": "regression-test"}
            result = subprocess.run(
                ["powershell.exe", "-NoProfile", "-NonInteractive", "-Command", command],
                input=json.dumps(payload), capture_output=True, text=True, timeout=15,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(result.stdout), {
                "args": ["--agent", "codex", "--url", url], "input": payload,
            })


class UninstallTests(unittest.TestCase):
    def run_install(self, home, *args):
        argv = ["install.py", "--home", str(home), *args]
        with patch.object(install.sys, "argv", argv), patch("builtins.print"):
            install.main()

    def test_partial_uninstall_keeps_relay_for_other_agents(self):
        with tempfile.TemporaryDirectory() as home:
            home = Path(home)
            relay = home / ".claude-light" / "relay.py"
            self.run_install(home, "--agents", "claude,cursor,codex", "--ip", "127.0.0.1")
            self.run_install(home, "--agents", "claude", "--uninstall")
            self.assertTrue(relay.exists())
            self.assertNotIn("relay.py", (home / ".claude" / "settings.json").read_text())
            self.run_install(home, "--agents", "cursor,codex", "--uninstall")
            self.assertFalse(relay.exists())

    def test_full_uninstall_removes_relay(self):
        with tempfile.TemporaryDirectory() as home:
            home = Path(home)
            self.run_install(home, "--agents", "claude,cursor,codex", "--ip", "127.0.0.1")
            self.run_install(home, "--agents", "claude,cursor,codex", "--uninstall")
            self.assertFalse((home / ".claude-light" / "relay.py").exists())


if __name__ == "__main__":
    unittest.main()
