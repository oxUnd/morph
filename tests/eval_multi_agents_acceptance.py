"""Independent acceptance with immutable fixtures and constrained child execution."""
import contextlib
import hashlib
import json
import os
from pathlib import Path
import shutil
import signal
import stat
import subprocess
import tempfile

HEADER = '''#ifndef EVAL_TASK_H
#define EVAL_TASK_H
#include <stddef.h>
int clamp_add(int a, int b);
size_t ascii_upper_count(const char *data, size_t length);
int combined_score(int a, int b, const char *data, size_t length);
#endif
'''

VERIFY = '''#include "task.h"
#include <limits.h>
#include <stdio.h>
int main(void)
{
    const int sums[][3] = {
        {2, 3, 5}, {INT_MAX, 1, INT_MAX}, {INT_MIN, -1, INT_MIN},
        {INT_MIN, INT_MAX, -1}, {INT_MAX, -INT_MAX, 0},
        {INT_MAX, INT_MIN, -1}, {INT_MAX, -1, INT_MAX - 1},
        {INT_MIN, 1, INT_MIN + 1}, {0, 0, 0}, {-7, 2, -5}
    };
    for (size_t i = 0; i < sizeof(sums) / sizeof(sums[0]); i++) {
        if (clamp_add(sums[i][0], sums[i][1]) != sums[i][2]) return 1;
    }
    const char bytes[] = {'A', 0, 'Z', (char)255, 'a'};
    if (ascii_upper_count(bytes, sizeof(bytes)) != 2) return 2;
    if (ascii_upper_count(NULL, 9) != 0) return 3;
    if (ascii_upper_count("AzBY", 0) != 0) return 4;
    if (ascii_upper_count("AzBY", 3) != 2) return 5;
    if (ascii_upper_count("AZaz09@", 7) != 2) return 6;
    char all_bytes[256];
    for (size_t i = 0; i < sizeof(all_bytes); i++) all_bytes[i] = (char)i;
    if (ascii_upper_count(all_bytes, sizeof(all_bytes)) != 26) return 7;
    if (combined_score(3, 4, "AzB", 3) != 9) return 8;
    if (combined_score(INT_MAX, 1, "AZ", 2) != INT_MAX) return 9;
    if (combined_score(INT_MIN, 0, "AZ", 2) != INT_MIN + 2) return 10;
    if (combined_score(INT_MIN, INT_MAX, "AZ", 2) != 1) return 11;
    if (combined_score(5, -5, NULL, 9) != 0) return 12;
    if (combined_score(INT_MAX, -INT_MAX, "aZ", 2) != 1) return 13;
    puts("ACCEPTANCE_OK: 22 checks");
    return 0;
}
'''

CONTRACT = '''#include "task.h"
int eval_contract_probe(void)
{
    int (*add)(int, int) = clamp_add;
    size_t (*count)(const char *, size_t) = ascii_upper_count;
    int (*score)(int, int, const char *, size_t) = combined_score;
    return add == NULL || count == NULL || score == NULL;
}
'''
MODULES = ('src/math.c', 'src/text.c', 'src/report.c')
MAX_FILE_BYTES = 2 * 1024 * 1024


def _read_regular(path):
    info = path.lstat()
    if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1:
        raise ValueError('nonregular_or_linked_file')
    if info.st_size > MAX_FILE_BYTES:
        raise ValueError('file_size_limit')
    return path.read_bytes()


def _source_paths(directory):
    paths = set()
    seen = 0
    for root, folders, files in os.walk(directory, followlinks=False):
        for name in folders + files:
            seen += 1
            if seen > 8192:
                raise ValueError('workspace_entry_limit')
            path = Path(root) / name
            if path.is_symlink():
                raise ValueError('workspace_symlink')
            if path.suffix.lower() in ('.c', '.h'):
                paths.add(path.relative_to(directory).as_posix())
    return sorted(paths)


def capture_integrity(directory, dependency=False):
    """Capture before inference; never reconstruct this baseline after execution."""
    directory = Path(directory)
    result = {'sha256': {}, 'source_paths': [], 'errors': [], 'dependency': dependency}
    immutable = ['verify.c', 'TASK.txt', 'AGENTS.md']
    if not dependency:
        immutable.append('include/task.h')
    for name in immutable:
        try:
            result['sha256'][name] = hashlib.sha256(_read_regular(directory / name)).hexdigest()
        except (OSError, ValueError) as error:
            result['errors'].append({'path': name, 'error': type(error).__name__})
    try:
        result['source_paths'] = _source_paths(directory)
    except (OSError, ValueError) as error:
        result['errors'].append({'error': type(error).__name__})
    return result


def _integrity(directory, baseline, dependency):
    errors = list(baseline.get('errors', []))
    changed = []
    expected = {'verify.c', 'TASK.txt', 'AGENTS.md'}
    if not dependency:
        expected.add('include/task.h')
    if set(baseline.get('sha256', {})) != expected:
        errors.append({'error': 'invalid_integrity_baseline'})
    if baseline.get('dependency') != dependency:
        errors.append({'error': 'baseline_task_mismatch'})
    for name, digest in baseline.get('sha256', {}).items():
        try:
            if hashlib.sha256(_read_regular(directory / name)).hexdigest() != digest:
                changed.append(name)
        except (OSError, ValueError):
            changed.append(name)
    try:
        unexpected = sorted(set(_source_paths(directory)) - set(baseline.get('source_paths', [])))
    except (OSError, ValueError) as error:
        unexpected = []
        errors.append({'error': type(error).__name__})
    return {'integrity_passed': not changed and not unexpected and not errors,
            'verifier_unchanged': 'verify.c' not in changed and 'verify.c' in baseline.get('sha256', {}),
            'changed_protected_files': changed, 'unexpected_source_paths': unexpected,
            'integrity_errors': errors}


def _run(runner, argv, workdir, temporary, timeout):
    environment = {'PATH': os.defpath, 'HOME': str(workdir / 'home'),
                   'TMPDIR': str(temporary), 'LANG': 'C', 'LC_ALL': 'C'}
    command = [str(runner), '--workdir', str(workdir), '--tmpdir', str(temporary),
               '--timeout-ms', str(timeout * 1000), '--', *map(str, argv)]
    process = None
    try:
        process = subprocess.Popen(command, env=environment, stdout=subprocess.PIPE,
                                   stderr=subprocess.PIPE, text=True, encoding='utf-8',
                                   errors='replace', start_new_session=True)
        stdout, _ = process.communicate(timeout=timeout + 5)
        report = json.loads(stdout)
        if not isinstance(report, dict):
            raise ValueError('invalid_runner_report')
        report['runner_exit'] = process.returncode
        return report
    except subprocess.TimeoutExpired:
        if process:
            with contextlib.suppress(ProcessLookupError):
                os.killpg(process.pid, signal.SIGKILL)
            process.communicate()
        return {'ok': False, 'timed_out': True, 'error': 'runner_timeout'}
    except (OSError, ValueError) as error:
        return {'ok': False, 'error': type(error).__name__}


def verify_task(directory, runner, baseline, dependency=False):
    """Build candidate modules separately from a fixed, external verifier."""
    result = {'compile_passed': False, 'tests_passed': False, 'assertions': 0,
              'contract_passed': False, 'timed_out': False, 'stages': []}
    directory = Path(directory).resolve()
    try:
        result.update(_integrity(directory, baseline, dependency))
        if not result['integrity_passed']:
            return result
        compiler = shutil.which('cc', path=os.defpath)
        if not compiler or not Path(runner).is_file():
            result['error'] = 'compiler_or_runner_missing'
            return result
        source = {name: _read_regular(directory / name) for name in MODULES}
        header = _read_regular(directory / 'include/task.h')
        with tempfile.TemporaryDirectory(prefix='morph-eval-trusted-') as trusted_name, \
                tempfile.TemporaryDirectory(prefix='morph-eval-build-') as build_name:
            trusted, build = Path(trusted_name).resolve(), Path(build_name).resolve()
            temporary = build / 'tmp'
            temporary.mkdir(mode=0o700)
            (build / 'home').mkdir(mode=0o700)
            (build / 'contract').mkdir(mode=0o700)
            (trusted / 'task.h').write_text(HEADER)
            (trusted / 'verify.c').write_text(VERIFY)
            (build / 'contract/task.h').write_bytes(header)
            (build / 'contract/probe.c').write_text(CONTRACT)
            for name, data in source.items():
                (build / Path(name).name).write_bytes(data)
            flags = [compiler, '-std=c11', '-Wall', '-Wextra', '-Wpedantic', '-Werror', '-UNDEBUG']
            stages = [
                ('contract', flags + ['-c', str(build / 'contract/probe.c'), '-o', 'contract.o']),
                ('modules', flags + ['-I', str(trusted), '-include', str(trusted / 'task.h'),
                                    '-c', 'math.c', 'text.c', 'report.c']),
                ('verifier', flags + ['-c', str(trusted / 'verify.c'), '-o', 'verify.o']),
                ('link', [compiler, 'math.o', 'text.o', 'report.o', 'verify.o', '-o', 'acceptance']),
                ('run', [str(build / 'acceptance')]),
            ]
            for name, command in stages:
                report = _run(Path(runner).resolve(), command, build, temporary,
                              10 if name == 'run' else 20)
                result['stages'].append({'name': name, 'ok': report.get('ok', False),
                                        'status': report.get('status'), 'exit_code': report.get('exit_code'),
                                        'error': report.get('error'), 'setup_failed': report.get('setup_failed', False)})
                result['timed_out'] |= report.get('timed_out', False)
                if not report.get('ok') or report.get('capture_failed') or report.get('output_truncated'):
                    break
                if name == 'contract':
                    result['contract_passed'] = True
                if name == 'link':
                    result['compile_passed'] = True
                if name == 'run':
                    result['tests_passed'] = report.get('stdout', '').strip() == 'ACCEPTANCE_OK: 22 checks'
            result.update(_integrity(directory, baseline, dependency))
            if any(_read_regular(directory / name) != data for name, data in source.items()):
                result['integrity_passed'] = False
                result['error'] = 'candidate_changed_during_acceptance'
            result['tests_passed'] &= result['integrity_passed']
            result['assertions'] = 22 if result['tests_passed'] else 0
    except (OSError, ValueError, TypeError) as error:
        result['error'] = type(error).__name__
        result['tests_passed'] = False
        result['assertions'] = 0
    return result
