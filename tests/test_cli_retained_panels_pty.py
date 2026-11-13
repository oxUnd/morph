"""Retained composer replacements keep the command and restore terminal state."""
import os
from pathlib import Path
import signal
import sys
import tempfile
import termios
import time

import pexpect
import pyte

from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen
from cli_inline_test_support import stable_rows


def wait_panel(terminal, command, title):
    deadline = time.monotonic() + 5
    while True:
        rows = stable_rows(terminal)
        if any(title in row for row in rows):
            break
        assert time.monotonic() < deadline, '\n'.join(rows)
    diagnostic = '\n'.join(rows)
    heading = next(index for index, row in enumerate(rows) if title in row)
    submitted = max(index for index, row in enumerate(rows) if row.rstrip() == command)
    assert heading == submitted + 2, diagnostic
    assert terminal.screen.buffer[submitted][0].fg == 'af87ff', diagnostic
    assert terminal.screen.cursor.y == heading and terminal.screen.cursor.x == 0, diagnostic
    assert not any(row.startswith(('› ', '» ')) for row in rows), diagnostic
    assert not any(row.lstrip().startswith('morph ·') for row in rows), diagnostic
    assert terminal.raw.count('\x1b[?1049h') == 1, terminal.raw[-3000:]


def wait_text(terminal, text):
    deadline = time.monotonic() + 5
    while True:
        rows = stable_rows(terminal)
        if any(text in row for row in rows):
            return rows
        assert time.monotonic() < deadline, '\n'.join(rows)


def check_common_lifecycle(binary):
    with tempfile.TemporaryDirectory(prefix='morph-retained-panel-actions-') as temp:
        terminal = Terminal(binary, Path(temp), 1, hooks_enabled=False,
                            production=True, extra_env={'NO_COLOR': ''})
        terminal.child.setwinsize(28, 80)
        terminal.screen = AlternateScreen(80, 28)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            for command, title, receipt in (
                    ('/model', 'Select Model Mode', 'model mode: ultra'),
                    ('/permissions', 'Update Model Permissions', 'permissions: auto')):
                terminal.send(command + '\r')
                wait_panel(terminal, command, title)
                for width in (120, 60, 80):
                    terminal.child.setwinsize(28, width)
                    terminal.screen.resize(lines=28, columns=width)
                    terminal.pump(.25)
                    wait_panel(terminal, command, title)
                terminal.send('\x1b')
                rows = wait_text(terminal, 'perm: ')
                assert not any(title in row for row in rows), '\n'.join(rows)
                assert sum(row.startswith(('› ', '» ')) for row in rows) == 1, '\n'.join(rows)
                terminal.send(command + '\r')
                wait_panel(terminal, command, title)
                terminal.send('\x1b[200~2\r\x1b[201~')
                wait_panel(terminal, command, title)
                terminal.send('\x1b[B\r')
                rows = wait_text(terminal, receipt)
                assert not any(title in row for row in rows), '\n'.join(rows)
                terminal.pump(1.1)
                rows = stable_rows(terminal)
                editors = [index for index, row in enumerate(rows)
                           if row.startswith(('› ', '» '))]
                assert len(editors) == 1, '\n'.join(rows)
                result = max(index for index, row in enumerate(rows) if receipt in row)
                assert editors[0] == result + 3, '\n'.join(rows)
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            terminal.child.close()
            assert terminal.child.exitstatus == 0, terminal.raw[-3000:]
            print('checked: model/permission cancel, paste isolation, resize and apply', flush=True)
        finally:
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    check_common_lifecycle(binary)
    panels = (('/model', 'Select Model Mode'),
              ('/permissions', 'Update Model Permissions'),
              ('/subagents', 'Subagents'))
    for command, title in panels:
        for terminate in ('signal', 'control_d'):
            with tempfile.TemporaryDirectory(prefix='morph-retained-panel-exit-') as temp:
                terminal = Terminal(binary, Path(temp), 1, hooks_enabled=False,
                                    production=True, extra_env={'NO_COLOR': ''})
                terminal.child.setwinsize(28, 80)
                terminal.screen = AlternateScreen(80, 28)
                terminal.stream = pyte.Stream(terminal.screen)
                try:
                    terminal.wait_for_composer()
                    terminal.send(command + '\r')
                    wait_panel(terminal, command, title)
                    if terminate == 'signal':
                        os.kill(terminal.child.pid, signal.SIGTERM)
                    else:
                        # First Ctrl+D cancels the replacement; the next exits the editor.
                        terminal.child.send('\x04\x04')
                    terminal.child.expect(pexpect.EOF, timeout=8)
                    terminal.raw += terminal.child.before
                    flags = termios.tcgetattr(terminal.child.child_fd)[3]
                    assert flags & termios.ICANON and flags & termios.ECHO, terminal.raw[-3000:]
                    terminal.child.close()
                    assert terminal.child.exitstatus == 0, terminal.raw[-3000:]
                    assert terminal.raw.count('\x1b[?1049h') == 1, terminal.raw[-3000:]
                    assert terminal.raw.count('\x1b[?1049l') == 1, terminal.raw[-3000:]
                    for failure in ('AddressSanitizer', 'UndefinedBehaviorSanitizer',
                                    'runtime error:', 'Application failed:'):
                        assert failure not in terminal.raw, terminal.raw[-5000:]
                    print(f'checked: {command} retained command and {terminate} cleanup',
                          flush=True)
                finally:
                    terminal.child.close(force=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
