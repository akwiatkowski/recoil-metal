"""Exercise terminal error colours through the executable's missing-map error."""

from pathlib import Path
import os
import pty
import subprocess
import tempfile
import unittest


class LogOutputTest(unittest.TestCase):
    command = [str(Path(__file__).resolve().parents[1] / "build/recoil-metal"),
               "/nonexistent/recoil-metal-log-test.scmap"]

    def test_redirected_errors_are_plain(self):
        result = subprocess.run(self.command, capture_output=True, timeout=10)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(b"ERROR [map]", result.stderr)
        self.assertNotIn(b"\x1b[", result.stderr)

    def test_terminal_errors_are_red_and_log_file_is_plain(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "game.log"
            master, slave = pty.openpty()
            try:
                result = subprocess.run(self.command + ["--log-file", str(log)],
                                        stdout=subprocess.DEVNULL, stderr=slave, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                terminal = os.read(master, 8192)
            finally:
                os.close(slave)
                os.close(master)
            error = next(line for line in terminal.splitlines() if b"ERROR [map]" in line)
            self.assertTrue(error.startswith(b"\x1b[31m"), error)
            self.assertIn(b"\x1b[0m", terminal)
            self.assertIn("ERROR [map]", log.read_text())
            self.assertNotIn("\x1b[", log.read_text())


if __name__ == "__main__":
    unittest.main()
