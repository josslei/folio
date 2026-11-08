#!/usr/bin/env python3
"""Explicit repository validation; optional formatting fixes, no Git mutations."""

import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]


def run(command, log):
    print('+ ' + ' '.join(map(str, command)), flush=True)
    try:
        with log.open('w', encoding='utf-8') as output:
            result = subprocess.run(command, cwd=ROOT, stdout=output,
                                    stderr=subprocess.STDOUT, check=False)
    except OSError as error:
        log.write_text(str(error) + '\n', encoding='utf-8')
        return False
    if result.returncode:
        print(log.read_text(encoding='utf-8', errors='replace')[-12000:])
    return result.returncode == 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--build-dir', type=Path, default=ROOT / 'build')
    parser.add_argument('--config', default='Debug')
    parser.add_argument('--fix', action='store_true',
                        help='format first-party C++ files before running selected checks')
    parser.add_argument('--checks', nargs='+',
                        choices=['whitespace', 'docs', 'build', 'tests', 'format', 'tidy'],
                        default=['whitespace', 'docs', 'build', 'tests', 'format', 'tidy'])
    parser.add_argument('--resource-limits', action='store_true',
                        help='also build/run the opt-in resource suite serially')
    args = parser.parse_args()
    build = args.build_dir.resolve()
    logs = ROOT / 'build' / 'validation'
    logs.mkdir(parents=True, exist_ok=True)
    session = logs / datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    session.mkdir()
    results = {}

    def check(name, command):
        passed = run(command, session / (name + '.log'))
        results[name] = 'PASS' if passed else 'FAIL'
        return passed

    if args.fix:
        tool = shutil.which('clang-format')
        if not tool or not (ROOT / '.clang-format').is_file():
            print('format-fix: requires .clang-format and installed clang-format')
            results['format-fix'] = 'BLOCKED'
        else:
            files = sorted(path for base in ['app', 'server', 'lib', 'tests']
                           for path in (ROOT / base).rglob('*')
                           if path.is_file() and path.suffix in {'.cpp', '.hpp', '.h'})
            check('format-fix', [tool, '-i', *map(str, files)])

    if 'whitespace' in args.checks:
        check('working-tree-whitespace', ['git', 'diff', '--check'])
        check('staged-whitespace', ['git', 'diff', '--cached', '--check'])
    if 'docs' in args.checks:
        check('docs', [sys.executable, str(ROOT / 'scripts/check_docs.py')])
    if 'build' in args.checks:
        configured = check('configure', ['cmake', '-S', str(ROOT), '-B', str(build),
                           '-DBUILD_TESTING=ON', '-DCMAKE_BUILD_TYPE=' + args.config,
                           '-DCMAKE_EXPORT_COMPILE_COMMANDS=ON'])
        if configured:
            check('build', ['cmake', '--build', str(build), '--config', args.config,
                            '--parallel'])
    if 'tests' in args.checks:
        if results.get('configure') == 'FAIL' or results.get('build') == 'FAIL':
            results['tests'] = 'BLOCKED'
        else:
            check('tests', ['ctest', '--test-dir', str(build), '-C', args.config,
                           '-LE', 'resource_limits', '--parallel',
                           '--output-on-failure', '--no-tests=error'])
    for kind, tool, config in [('format', 'clang-format', '.clang-format'),
                               ('tidy', 'clang-tidy', '.clang-tidy')]:
        if kind not in args.checks:
            continue
        if not (ROOT / config).is_file() or not shutil.which(tool):
            print(f'{kind}: requires approved {config} and installed {tool}')
            results[kind] = 'BLOCKED'
            continue
        if kind == 'tidy' and not (build / 'compile_commands.json').is_file():
            print('tidy: compile_commands.json missing; configure a supported generator')
            results[kind] = 'BLOCKED'
            continue
        files = sorted(path for base in ['app', 'server', 'lib', 'tests']
                       for path in (ROOT / base).rglob('*')
                       if path.suffix in ({'.cpp'} if kind == 'tidy'
                                          else {'.cpp', '.hpp', '.h'}))
        if kind == 'tidy':
            entries = json.loads((build / 'compile_commands.json').read_text())
            compiled = {(Path(entry['directory']) / entry['file']).resolve()
                        for entry in entries}
            files = [path for path in files if path.resolve() in compiled]
        passed = True
        for index, path in enumerate(files):
            command = ([tool, '--dry-run', '--Werror', str(path)] if kind == 'format'
                       else [tool, '-p', str(build), '--warnings-as-errors=*', str(path)])
            passed = run(command, session / f'{kind}-{index}.log') and passed
        results[kind] = 'PASS' if passed and files else 'FAIL'
    if args.resource_limits:
        resource = ROOT / 'build-resource-limits'
        if check('resource-configure', ['cmake', '-S', str(ROOT), '-B', str(resource),
                 '-DBUILD_TESTING=ON', '-DFOLIO_BUILD_RESOURCE_LIMIT_TESTS=ON',
                 '-DCMAKE_BUILD_TYPE=' + args.config]):
            if check('resource-build', ['cmake', '--build', str(resource), '--config',
                     args.config, '--target', 'folio_resource_limit_tests', '--parallel']):
                check('resource-tests', ['ctest', '--test-dir', str(resource), '-C',
                      args.config, '-L', 'resource_limits', '--parallel', '1',
                      '--output-on-failure', '--no-tests=error'])
    print('\n' + '\n'.join(f'{key}: {value}' for key, value in results.items()))
    print('Only executed checks are certified; ' +
          ('formatting fixes were requested; review the source diff.' if args.fix else
           'no automatic fixes were applied.'))
    print(f'Logs: {session}')
    (session / 'summary.json').write_text(json.dumps(results, indent=2) + '\n',
                                          encoding='utf-8')
    return 0 if results and all(value == 'PASS' for value in results.values()) else 1


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.exit(130)
