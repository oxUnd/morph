"""Shared application loop: background output, modal ownership, and recovery."""
import http.server
import os
from pathlib import Path
import signal
import sys
import termios
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte
import pexpect


def until(terminal, predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        terminal.pump(.05)
        text = '\n'.join(terminal.screen.display)
        if predicate(text) and 2026 << 5 not in terminal.screen.mode:
            return text
    raise AssertionError(f'alive={terminal.child.isalive()}\n' + terminal.raw[-3000:])


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    release = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-application-pty-') as temp:
            terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
            terminal.screen = AlternateScreen(80, 24)
            terminal.stream = pyte.Stream(terminal.screen)
            until(terminal, lambda text: 'ctrl+o details' in text)
            terminal.send('Run while I inspect jobs\r')
            _, release = terminal.request()
            terminal.send('/jobs\r')
            until(terminal, lambda text: 'morph / jobs' in text)
            release.delta = {'content': 'APPLICATION_DEFERRED_ANSWER'}
            release.set()
            terminal.pump(.8)
            text = '\n'.join(terminal.screen.display)
            assert 'morph / jobs' in text, text
            assert 'APPLICATION_DEFERRED_ANSWER' not in text, text
            for columns in (120, 60, 80):
                terminal.child.setwinsize(24, columns)
                terminal.screen.resize(lines=24, columns=columns)
                text = until(terminal, lambda text: 'morph / jobs' in text)
                assert 'APPLICATION_DEFERRED_ANSWER' not in text, text
            terminal.send('\x1b[200~q\r\x1b[201~')
            terminal.pump(.2)
            assert 'morph / jobs' in '\n'.join(terminal.screen.display)
            # The panel's exit key must leave the next command for Readline.
            terminal.send('qkept application draft')
            text = until(terminal, lambda text: 'kept application draft' in text
                         and 'Worked for' in text)
            assert text.count('APPLICATION_DEFERRED_ANSWER') == 1, text
            assert 'morph / jobs' not in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            terminal.send('\x15/model\r')
            until(terminal, lambda text: 'current model: pty-test' in text)
            terminal.send('/history\r')
            until(terminal, lambda text: 'saved entries' in text)
            terminal.send('\x03')
            until(terminal, lambda text: 'ctrl+o details' in text and 'saved entries' not in text)
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF)
            terminal.child.close()
            assert terminal.child.exitstatus == 0, terminal.raw[-2000:]
        for modal in (False, True):
            with tempfile.TemporaryDirectory(prefix='morph-application-exit-') as temp:
                terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
                terminal.screen = AlternateScreen(80, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                terminal.wait_for_composer()
                terminal.send('Keep working until termination\r')
                _, release = terminal.request()
                if modal:
                    terminal.send('/jobs\r')
                    until(terminal, lambda text: 'morph / jobs' in text)
                os.kill(terminal.child.pid, signal.SIGTERM)
                terminal.child.expect(pexpect.EOF, timeout=5)
                terminal.raw += terminal.child.before
                flags = termios.tcgetattr(terminal.child.child_fd)[3]
                assert flags & termios.ICANON, 'terminal remains in raw mode after shutdown'
                assert flags & termios.ECHO, 'terminal echo was not restored'
                if modal:
                    assert '\x1b[?1049l' in terminal.raw, terminal.raw[-2000:]
                terminal.child.close()
                assert terminal.child.exitstatus == 0, terminal.raw[-2000:]
                release.set()
        print('PASS: deferred output, modal ownership, resize, input handoff, '
              'and termination during active work at root and in a modal')
    finally:
        if release:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
