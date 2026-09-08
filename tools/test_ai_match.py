"""The AI runner must never stop a process just because its PID was reused."""
import argparse
import json
import signal
import subprocess
import sys
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory
from unittest.mock import patch

import ai_match


class AiMatchTest(unittest.TestCase):
    def test_failed_registration_does_not_leave_a_child_running(self):
        with TemporaryDirectory() as directory:
            script = Path(directory) / "child.py"
            script.write_text("import time; time.sleep(60)")
            children = []
            launch = subprocess.Popen

            def spawn(*args, **kwargs):
                child = launch(*args, **kwargs)
                children.append(child)
                return child

            with patch.object(ai_match, "BINARY", Path(sys.executable)), \
                 patch.object(ai_match, "command", return_value=[sys.executable, str(script)]), \
                 patch.object(ai_match.subprocess, "Popen", side_effect=spawn), \
                 patch.object(ai_match, "process_identity", side_effect=PermissionError("blocked")):
                with self.assertRaises(PermissionError):
                    ai_match.run(argparse.Namespace(timeout=None), Path(directory) / "job")
            self.assertEqual(len(children), 1)
            self.assertIsNotNone(children[0].returncode)

    def test_exit_and_timeout_are_recorded_for_real_child_processes(self):
        for program, timeout, expected in (("print('finished'); raise SystemExit(7)", None, 7),
                                            ("import time; time.sleep(60)", 1, -signal.SIGTERM)):
            with self.subTest(timeout=timeout), TemporaryDirectory() as directory:
                script = Path(directory) / "child.py"
                script.write_text(program)
                job = Path(directory) / "job"
                with patch.object(ai_match, "BINARY", Path(sys.executable)), \
                     patch.object(ai_match, "command", return_value=[sys.executable, str(script)]), \
                     patch.object(ai_match, "process_identity", return_value="test child"):
                    self.assertEqual(ai_match.run(argparse.Namespace(timeout=timeout), job), 1)
                state = json.loads((job / "process.json").read_text())
                self.assertEqual(state["returncode"], expected)
                if timeout:
                    self.assertEqual(state["stop_reason"], "TimeoutExpired")
                else:
                    self.assertIn("finished", (job / "stdout.log").read_text())

    def test_command_is_fixed_offscreen_and_keeps_artifacts_in_job(self):
        args = argparse.Namespace(seconds=3600, personality="tech", armies=2,
                                  map="SCMP_009", fa_root=Path("/game data"))
        job = Path("/repo/build/ai-matches/test")
        command = ai_match.command(args, job)
        self.assertEqual(command[0], str(ai_match.BINARY))
        self.assertIn("--observer", command)
        self.assertIn("--mute", command)
        self.assertEqual(command[command.index("--screenshot") + 1:],
                         [str(job / "final.png"), "1000", "700"])
        self.assertEqual(command[command.index("--hash-log") + 1], str(job / "ticks.hash"))

    def test_stop_requires_matching_process_identity(self):
        with TemporaryDirectory() as directory:
            job = Path(directory)
            (job / "process.json").write_text(json.dumps({"pid": 123,
                "identity": "original start time and engine command"}))
            with patch.object(ai_match, "process_identity", return_value="different process"), \
                 patch.object(ai_match.os, "kill") as kill:
                with self.assertRaisesRegex(RuntimeError, "not the recorded match"):
                    ai_match.stop(job, False)
                kill.assert_not_called()
            with patch.object(ai_match, "process_identity",
                              return_value="original start time and engine command"), \
                 patch.object(ai_match.os, "kill") as kill:
                ai_match.stop(job, True)
                kill.assert_called_once_with(123, signal.SIGKILL)

    def test_completed_job_is_not_signalled(self):
        with TemporaryDirectory() as directory:
            job = Path(directory)
            (job / "process.json").write_text(json.dumps({"pid": 123, "returncode": 0}))
            with patch.object(ai_match.os, "kill") as kill:
                ai_match.stop(job, False)
                kill.assert_not_called()

    def test_names_cannot_escape_job_directory(self):
        for name in ("../outside", "/tmp/job", "a/b", ".", "--flag"):
            with self.subTest(name=name), self.assertRaises(argparse.ArgumentTypeError):
                ai_match.job_name(name)
        self.assertEqual(ai_match.job_name("tech-hour-2"), "tech-hour-2")


if __name__ == "__main__":
    unittest.main()
