"""Extension subprocess output must stay inside the command's retained TUI views."""
from pathlib import Path
import json
import os
import shlex
import shutil
import sqlite3
import subprocess
import sys
import tempfile

from test_cli_pty import Model, Terminal, pexpect, terminal_environment
from test_cli_slash_pty import composer, wait_for
from test_cli_resume_history_pty import seed
from test_cli_transcript_pty import AlternateScreen
import pyte


def repository(root):
    git = shutil.which('git')
    if not git:
        sys.exit(77)
    source = root / 'source'
    source.mkdir()
    (source / 'manifest.toml').write_text(
        'name = "pty-install"\nversion = "0.1.0"\nentry = "run.sh"\n'
        '[build]\ncommand = ' + json.dumps(
            shlex.quote(sys.executable) + ' build.py') + '\n')
    (source / 'run.sh').write_text('#!/bin/sh\nexit 0\n')
    (source / 'build.py').write_text(r'''import os, sys, time
from pathlib import Path
root = Path(os.environ['EXT_TEST_CONTROL'])
assert sys.stdin.read() == ''
os.write(1, b'\x1b[2J\x1b[HBUILD_STDOUT\n')
os.write(2, b'\x1b]0;HIDDEN_TITLE\x07BUILD_STDERR\n')
text = 'SPLIT_UTF8_中文\n'.encode()
os.write(1, text[:-5])
time.sleep(.15)
os.write(1, text[-5:])
os.write(2, b'\x1b[')
time.sleep(.15)
os.write(2, b'2JBUILD_WAITING\n')
while not (root / 'release').exists():
    time.sleep(.02)
if (root / 'fail').exists():
    os.write(2, b'BUILD_FAILURE_DETAIL\n')
    sys.exit(9)
os.write(1, b'BUILD_LAST_WITHOUT_NEWLINE')
''')
    environment = dict(os.environ, GIT_CONFIG_GLOBAL=os.devnull, GIT_CONFIG_NOSYSTEM='1')
    for arguments in (['init', '-q', str(source)],
                      ['-C', str(source), 'add', '.'],
                      ['-C', str(source), '-c', 'user.name=test',
                       '-c', 'user.email=test@example.com', 'commit', '-qm', 'fixture']):
        subprocess.run([git, *arguments], check=True, env=environment, capture_output=True)
    remote = root / 'remotes' / 'owner' / 'repo.git'
    remote.parent.mkdir(parents=True)
    subprocess.run([git, 'clone', '--bare', str(source), str(remote)],
                   check=True, env=environment, capture_output=True)
    config = root / 'gitconfig'
    config.write_text(f'[url "file://{root}/remotes/"]\n'
                      '\tinsteadOf = https://github.com/\n[init]\n\tdefaultBranch = main\n')
    return config


def check_frames(terminal, command, answer_marker):
    separated = False
    for frame, _ in terminal.published_frames:
        rows = frame.splitlines()
        headings = [i for i, row in enumerate(rows) if row.rstrip() == command]
        answers = [i for i, row in enumerate(rows) if answer_marker in row]
        if headings and answers:
            assert headings[0] == answers[-1] + 2, frame
            assert not rows[headings[0] - 1].strip(), frame
            separated = True
        assert 'HIDDEN_TITLE' not in frame, frame
        if 'BUILD_STDOUT' not in frame:
            continue
        assert frame.count('› ') == 1, frame
        assert frame.count('perm: ') == 1, frame
    assert separated, "No published frame showed the restored answer and command together"
    assert not terminal.duplicate_prompts, terminal.duplicate_prompts


def exercise(driver, root, gitconfig, width, action):
    directory = root / f'{width}-{action}'
    directory.mkdir()
    installed = directory / 'installed'
    terminal = Terminal(
        driver, directory, 1, hooks_enabled=False,
        extra_config=f'\n[ext]\ndir = "{installed}"\n',
        extra_env={'GIT_CONFIG_GLOBAL': str(gitconfig), 'GIT_CONFIG_NOSYSTEM': '1',
                   'GIT_TERMINAL_PROMPT': '0', 'EXT_TEST_CONTROL': str(directory)})
    terminal.child.setwinsize(32, width)
    terminal.screen = AlternateScreen(width, 32)
    terminal.stream = pyte.Stream(terminal.screen)
    try:
        terminal.wait_for_composer()
        answer_marker = 'RESTORED_EXT_ANSWER_END'
        _, legacy = seed(directory / 'data.db')
        with sqlite3.connect(directory / 'data.db') as database:
            database.execute("UPDATE messages SET content=? WHERE session_id=? "
                             "AND role='assistant'", (
                'Export with `:TOhtml` or `pygmentize -f html -O full`.\n\n' + answer_marker,
                legacy))
            database.execute("DELETE FROM messages WHERE session_id=? AND role='user'",
                             (legacy,))
        terminal.send('/resume 8765fedc\r')
        wait_for(terminal, lambda: answer_marker in '\n'.join(terminal.screen.display))
        terminal.published_frames.clear()
        if action == 'failure':
            (directory / 'fail').touch()
        command = '/ext install github:owner/repo'
        if action in ('success', 'failure'):
            command += ' --yes'
        terminal.send(command + '\r')
        if action in ('cancel', 'confirm'):
            wait_for(terminal, lambda: 'Run build' in '\n'.join(terminal.screen.display))
            assert 'BUILD_STDOUT' not in terminal.raw
            terminal.send('\x1b[B\r' if action == 'confirm' else '\r')
        if action != 'cancel':
            wait_for(terminal, lambda: 'BUILD_WAITING' in '\n'.join(terminal.screen.display))
            frame = '\n'.join(terminal.screen.display)
            for text in ('BUILD_STDOUT', 'BUILD_STDERR', 'SPLIT_UTF8_中文'):
                assert text in frame, frame
            composer(terminal)
            assert not (installed / 'pty-install').exists()
            check_frames(terminal, command, answer_marker)
            # Input queued while the synchronous command owns the terminal must
            # still become one intact draft when installation returns.
            terminal.send('draft中文')
            (directory / 'release').touch()
        expected = 'ext install failed:' if action in ('cancel', 'failure') else 'installed ext'
        wait_for(terminal, lambda: expected in '\n'.join(terminal.screen.display))
        terminal.pump(.15)
        check_frames(terminal, command, answer_marker)
        composer(terminal, '' if action == 'cancel' else 'draft中文')
        screen = '\n'.join(terminal.screen.display)
        if action in ('success', 'confirm'):
            assert (installed / 'pty-install' / 'run.sh').exists()
            rows = terminal.screen.display
            last = next(i for i, row in enumerate(rows) if 'BUILD_LAST_WITHOUT_NEWLINE' in row)
            receipt = next(i for i, row in enumerate(rows) if 'installed ext' in row)
            assert receipt == last + 1, screen
        else:
            assert not (installed / 'pty-install').exists()
            if action == 'failure':
                assert 'BUILD_FAILURE_DETAIL' in screen, screen
        terminal.send('\x15/model status\r')
        wait_for(terminal, lambda: 'current model: pty-test' in
                 '\n'.join(terminal.screen.display))
        composer(terminal)
        terminal.send('\x1b[A')
        composer(terminal, '/model status')
        terminal.send('\x15/quit\r')
        terminal.child.expect_exact(pexpect.EOF)
        assert Model.requests.empty(), 'extension management reached the model'
        print(f'PASS: /ext {action}, live output and composer at {width} columns')
    except Exception:
        print(terminal.raw[-6000:])
        print('\n'.join(terminal.screen.display))
        raise
    finally:
        (directory / 'release').touch()
        terminal.child.close(force=True)


def structured_install(binary, root, gitconfig, action):
    directory = root / f'json-{action}'
    directory.mkdir()
    original = root / '120-success'
    config = directory / 'config.toml'
    config.write_text((original / 'config.toml').read_text().replace(str(original), str(directory)))
    (directory / 'release').touch()
    command = '/ext install github:owner/repo'
    if action == 'yes':
        command += ' --yes'
    response = json.dumps({'type': 'interaction.response', 'request_id': 'interaction-1',
                           'data': {'decision': action}}) + '\n'
    result = subprocess.run(
        [str(binary), '-c', str(config), '-w', str(directory), '--events', 'json', '-p', command],
        input=response, text=True, capture_output=True, timeout=15,
        env=terminal_environment(directory, {
            'GIT_CONFIG_GLOBAL': str(gitconfig), 'GIT_CONFIG_NOSYSTEM': '1',
            'GIT_TERMINAL_PROMPT': '0', 'EXT_TEST_CONTROL': str(directory)}))
    events = [json.loads(line) for line in result.stdout.splitlines()]
    expected = 'command.failed' if action == 'cancel' else 'command.completed'
    event = next(event for event in events if event['name'] == expected)
    output = event['data']['output']
    assert '\x1b' not in output and 'HIDDEN_TITLE' not in output, output
    if action == 'cancel':
        assert 'BUILD_STDOUT' not in output, output
        assert not (directory / 'installed' / 'pty-install').exists()
    else:
        assert 'BUILD_STDOUT' in output and 'BUILD_STDERR' in output, output
        assert 'installed ext' in output, output
    if action != 'yes':
        request = next(event for event in events if event['name'] == 'interaction.request')
        assert request['data']['kind'] == 'command_confirmation', request
    assert 'BUILD_STDERR' not in result.stderr, result.stderr
    print(f'PASS: /ext JSON {action}, captured subprocess output and structured confirmation')


def main():
    driver = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='morph-ext-pty-') as temporary:
        root = Path(temporary)
        gitconfig = repository(root)
        for width in (120, 80, 60):
            for action in ('success', 'cancel', 'confirm', 'failure'):
                exercise(driver, root, gitconfig, width, action)
        if len(sys.argv) > 2:
            for action in ('yes', 'confirm', 'cancel'):
                structured_install(Path(sys.argv[2]).resolve(), root, gitconfig, action)


if __name__ == '__main__':
    main()
