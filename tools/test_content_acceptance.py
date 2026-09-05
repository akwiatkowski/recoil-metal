"""A skipped corpus test must fail strict acceptance, even with exit code zero."""
import unittest
import xml.etree.ElementTree as ET
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path
from subprocess import CompletedProcess
from tempfile import TemporaryDirectory
from unittest.mock import patch

from content_acceptance import main, report_failures


class ContentAcceptanceTest(unittest.TestCase):
    def test_success_requires_executed_cases(self):
        self.assertEqual(report_failures(ET.fromstring(
            '<testsuite><testcase name="real blueprint"/></testsuite>')), [])
        self.assertTrue(report_failures(ET.fromstring('<testsuite/>')))

    def test_missing_content_is_a_failure(self):
        failures = report_failures(ET.fromstring(
            '<testsuite skipped="1"><testcase name="real projectile">'
            '<skipped>Missing /corpus/projectiles</skipped></testcase></testsuite>'))
        self.assertTrue(any('real projectile' in item and '/corpus/projectiles' in item
                            for item in failures))

    def test_failed_and_incomplete_reports_cannot_pass(self):
        for report in (
            '<testsuite><testcase name="broken"><failure>bad value</failure></testcase></testsuite>',
            '<testsuite errors="1"><testcase name="crash"/></testsuite>',
            '<testsuite skipped="1"><testcase name="unreported skip"/></testsuite>',
        ):
            with self.subTest(report=report):
                self.assertTrue(report_failures(ET.fromstring(report)))

    def test_cli_rejects_skips_process_failure_and_stale_reports(self):
        passed = '<testsuite><testcase name="real content"/></testsuite>'
        skipped = '<testsuite><testcase name="missing"><skipped/></testcase></testsuite>'
        for xml, code, expected in ((passed, 0, 0), (skipped, 0, 1), (passed, 42, 1), ('', 0, 1)):
            with self.subTest(code=code, xml=xml), TemporaryDirectory() as directory:
                report = Path(directory) / 'tests.xml'
                report.write_text(passed)

                def run(command, **kwargs):
                    if xml:
                        Path(command[-1]).write_text(xml)
                    return CompletedProcess(command, code)

                with patch('sys.argv', ['content_acceptance', '--report', str(report)]), \
                     patch('content_acceptance.subprocess.run', side_effect=run), \
                     redirect_stdout(StringIO()):
                    self.assertEqual(main(), expected)


if __name__ == '__main__':
    unittest.main()
