"""Check the native runner's evidence gate without launching AppKit."""

from contextlib import redirect_stdout
import io
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
from unittest.mock import patch

import input_acceptance


class EvidenceTest(unittest.TestCase):
    def test_each_new_workflow_must_report_exactly_once(self):
        evidence = [
            "T2 factory native upgrade PASS",
            "T3 factory native upgrade PASS",
            "naval-yard native placement and completion PASS",
            "AUTO MEX native toggle and three completed deposits PASS",
        ]
        existing = "\n".join([
            "input acceptance: PASS 6 products,",
            "selection and native right-click Move PASS",
            "production pagination, pending/active cancellation, Clear Queue PASS",
            *["production, selection, Move, Shift queue, Stop, input swallowing PASS"] * 6,
            *["native Guard target and Stop PASS"] * 6,
            *["native Attack target and Stop PASS"] * 5,
            "native Attack unavailable PASS",
        ])
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for name in ("binary", "map"):
                (root / name).touch()
            (root / "data").mkdir()
            args = ["input_acceptance.py", "--binary", str(root / "binary"),
                    "--map", str(root / "map"), "--data-dir", str(root / "data"),
                    "--output", str(root / "results"), "--profiles", "compact",
                    "--backings", "1", "--factories", "UEB0101", "--timeout", "17"]
            variants = [("complete", evidence)]
            for marker in evidence:
                variants.append(("missing " + marker, [line for line in evidence if line != marker]))
                variants.append(("duplicate " + marker, [*evidence, marker]))
            for name, lines in variants:
                with self.subTest(name=name):
                    def fake_run(command, *, stdout, stderr, timeout):
                        self.assertEqual(timeout, 17)
                        self.assertEqual(stderr, subprocess.STDOUT)
                        stdout.write(existing + "\n" + "\n".join(lines))
                        output = Path(command[command.index("--input-acceptance") + 1])
                        output.write_bytes(b"\x89PNG\r\n\x1a\n" + b"\x00" * 8
                                           + struct.pack(">II", 1280, 720))
                        return subprocess.CompletedProcess(command, 0)

                    with patch("sys.argv", args), patch.object(
                        input_acceptance.subprocess, "run", side_effect=fake_run
                    ), redirect_stdout(io.StringIO()):
                        if name == "complete":
                            input_acceptance.main()
                        else:
                            with self.assertRaisesRegex(SystemExit, "FAIL missing workflow evidence"):
                                input_acceptance.main()


if __name__ == "__main__":
    unittest.main()
