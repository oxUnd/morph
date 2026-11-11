"""Quit hints survive terminal cleanup and can restore the selected session."""
from pathlib import Path
import os
import re
import shlex
import sys
import tempfile

from test_cli_resume_history_pty import seed, start
import pexpect


def finish(terminal, command='/quit'):
    terminal.send(command + '\r')
    terminal.child.expect(pexpect.EOF, timeout=10)
    tail = terminal.child.before
    if tail:
        terminal.raw += tail
        terminal.stream.feed(tail)
    terminal.child.close()
    assert terminal.child.exitstatus == 0, terminal.raw[-5000:]
    assert 'Goodbye!' in terminal.raw, terminal.raw[-5000:]
    return '\n'.join(terminal.screen.display)


def commands(terminal, directory, identity, screen, color=True):
    plain = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', terminal.raw)
    matches = re.findall(r'^  ((?:morph|morphctl) resume [^\r\n]+)', plain, re.MULTILINE)
    assert len(matches) == 2, plain[-6000:]
    result = [shlex.split(command) for command in matches]
    for executable, arguments in zip(('morph', 'morphctl'), result):
        assert arguments[:3] == [executable, 'resume', identity], arguments
        assert arguments[3:] == ['-c', str((directory / 'config.toml').resolve()),
                                  '-w', str(directory.resolve())], arguments
    assert plain.index('Goodbye!') < plain.index('To resume this session, run:'), plain[-6000:]
    assert 'morph resume ' + identity in screen, screen
    assert 'morphctl resume ' + identity in screen, screen
    for executable in ('morph', 'morphctl'):
        row = next(index for index, text in enumerate(terminal.screen.display)
                   if text.lstrip().startswith(executable + ' resume '))
        text = terminal.screen.display[row]
        command_cell = terminal.screen.buffer[row][text.index(executable)]
        identity_cell = terminal.screen.buffer[row][text.index(identity)]
        assert command_cell.fg == ('af87ff' if color else 'default'), screen
        assert identity_cell.fg == ('cyan' if color else 'default'), screen
        assert command_cell.bold == color and identity_cell.bold == color, screen
    heading = next(index for index, row in enumerate(terminal.screen.display)
                   if 'To resume this session, run:' in row)
    for row in range(heading, terminal.screen.lines):
        assert all(terminal.screen.buffer[row][column].bg != '3a3a3a'
                   for column in range(terminal.screen.columns)), screen
    assert not any(row.strip() == '›' for row in terminal.screen.display), screen
    assert '› /quit' not in screen and '› /q' not in screen, screen
    return result


def main():
    binary = Path(sys.argv[1]).resolve()
    os.environ['NO_COLOR'] = ''
    for columns in (120, 80, 60):
        with tempfile.TemporaryDirectory(prefix='morph-exit-pty-') as temp:
            directory = Path(temp) / "work's 中文 $HOME"
            directory.mkdir()
            wrapper = Path(temp) / 'morphctl'
            wrapper.write_text('#!/bin/sh\nexec ' + shlex.quote(str(binary)) + ' "$@"\n')
            wrapper.chmod(0o755)
            terminal = start(binary, directory, 1, columns)
            try:
                terminal.wait_for_composer()
                finish(terminal)
                assert 'To resume' not in terminal.raw, terminal.raw[-5000:]
                seed(directory / 'home' / '.morph' / 'data.db')
                executable = wrapper if columns == 80 else binary
                terminal = start(executable, directory, 1, columns, 'resume', '1234abcd')
                terminal.wait_for_composer()
                screen = finish(terminal, '/q' if columns == 60 else '/quit')
                copied = commands(terminal, directory, '1234abcd', screen)
                for arguments in copied:
                    executable = binary if arguments[0] == 'morph' else wrapper
                    color = arguments[0] == 'morphctl'
                    extra = [] if color else ['--no-color']
                    terminal = start(executable, directory, 1, columns, *arguments[1:], *extra)
                    terminal.wait_for_composer()
                    assert 'HISTORY_LAST_ANSWER' in terminal.raw, terminal.raw[-5000:]
                    screen = finish(terminal)
                    commands(terminal, directory, '1234abcd', screen, color=color)
                print(f'checked: {columns}: colored hints, no-color and both resume commands',
                      flush=True)
            finally:
                terminal.child.close(force=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
