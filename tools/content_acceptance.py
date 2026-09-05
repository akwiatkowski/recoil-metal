"""Run Catch's complete content suite and reject any skipped checks."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import xml.etree.ElementTree as ET


def report_failures(root: ET.Element) -> list[str]:
    failures = []
    if root.tag not in ('testsuite', 'testsuites') or not list(root.iter('testcase')):
        failures.append('No executed test cases in the report')
    for case in root.iter('testcase'):
        for result in case:
            if result.tag in ('skipped', 'failure', 'error'):
                detail = ''.join(result.itertext()).strip() or result.get('message', '')
                failures.append(f"{case.get('name', '?')}: {result.tag}: {detail}")
    for suite in root.iter('testsuite'):
        for status in ('skipped', 'failures', 'errors'):
            count = int(suite.get(status, '0'))
            if count:
                failures.append(f"{suite.get('name', 'suite')}: {count} {status}")
    return failures


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, default=Path('build/rm_tests'))
    parser.add_argument('--report', type=Path, help='Retained JUnit report path')
    args = parser.parse_args()
    binary = args.binary.resolve()
    report = (args.report or Path(tempfile.mkdtemp(prefix='recoil-content-')) / 'tests.xml').resolve()
    output = Path(f'{report}.log')
    try:
        report.parent.mkdir(parents=True, exist_ok=True)
        report.write_text('')  # a failed launch must never validate a stale report
        with output.open('w') as stream:
            result = subprocess.run(
                [str(binary), '--reporter', 'junit', '--out', str(report)],
                cwd=Path(__file__).resolve().parents[1], stdout=stream,
                stderr=subprocess.STDOUT, check=False)
        root = ET.parse(report).getroot()
        failures = report_failures(root)
        if result.returncode:
            failures.insert(0, f'Test process exited with {result.returncode}')
    except (OSError, ET.ParseError, ValueError) as error:
        failures = [str(error)]
    print(f'Content acceptance report: {report}\nRunner output: {output}')
    if failures:
        print('FAIL: strict acceptance permits no skipped or failed content checks.')
        for failure in failures:
            print(failure)
        return 1
    print(f'PASS: {len(list(root.iter("testcase")))} reported cases; no skips or failures.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
