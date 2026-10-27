"""Local commands must not become chat bubbles or stale composer backgrounds."""
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, http, pexpect
from test_cli_transcript_pty import AlternateScreen
import pyte


def wait_for(terminal, predicate):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        terminal.pump(0.05)
        if predicate():
            return
        if not terminal.child.isalive():
            break
    raise AssertionError(f'alive={terminal.child.isalive()} exit={terminal.child.exitstatus} '
                         f'signal={terminal.child.signalstatus}\n' + terminal.raw[-3000:] + '\n' +
                         '\n'.join(terminal.screen.display))


def composer(terminal, expected=''):
    rows = terminal.screen.display
    prompts = [index for index, row in enumerate(rows) if row.startswith('› ')]
    assert len(prompts) == 1, '\n'.join(rows)
    row = prompts[0]
    assert rows[row].startswith('› ' + expected), '\n'.join(rows)
    backgrounds = [index for index in range(terminal.screen.lines)
                   if terminal.screen.buffer[index][0].bg == '1c1c1c']
    assert backgrounds == [row - 1, row, row + 1], (backgrounds, '\n'.join(rows))
    return row


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    release = None
    with tempfile.TemporaryDirectory(prefix='morph-slash-pty-') as temp:
        terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
        terminal.child.setwinsize(32, 100)
        terminal.screen = AlternateScreen(100, 32)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            wait_for(terminal, lambda: any(row.startswith('› ')
                                          for row in terminal.screen.display))
            composer(terminal)
            terminal.send('/new slash-rules\r')
            wait_for(terminal, lambda: 'created and switched to session: slash-rules' in
                     '\n'.join(terminal.screen.display))
            composer(terminal)
            for command, title in [('/hooks', 'Lifecycle hooks'),
                                   ('/resume', 'Resume a session'),
                                   ('/history', 'history'), ('/hi', 'history')]:
                terminal.send(command + '\r')
                wait_for(terminal, lambda: title in '\n'.join(terminal.screen.display))
                terminal.send('\x1b')
                composer(terminal)
                assert command not in '\n'.join(terminal.screen.display)
                print(f'PASS: {command} cancellation restores one composer without an echo')
            terminal.send('/model\r')
            display = '\n'.join(terminal.screen.display)
            assert 'current model: pty-test' in display, display
            composer(terminal)
            output_row = next(index for index, row in enumerate(terminal.screen.display)
                              if 'current model: pty-test' in row)
            assert terminal.screen.buffer[output_row][0].bg != '1c1c1c'
            terminal.send('/rename slash-renamed\r')
            assert 'session renamed to: slash-renamed' in '\n'.join(terminal.screen.display)
            composer(terminal)
            terminal.send('/model-unknown\r')
            assert 'unknown command: /model-unknown' in '\n'.join(terminal.screen.display)
            composer(terminal, '/model-unknown')
            terminal.send('\x15/model\r')
            composer(terminal)
            terminal.send('\x1b[A')
            composer(terminal, '/model')
            terminal.send('\x15')
            assert Model.requests.empty(), 'local commands reached the model'
            print('PASS: results, receipts, error correction, and Readline recall')
            terminal.send('live probe\r')
            _, release = terminal.request()
            terminal.send('/hooks\r')
            assert 'wait for the active turn' in '\n'.join(terminal.screen.display)
            assert Model.requests.empty(), 'local command became model steering'
            terminal.send('\x15/model-unknown\r')
            assert 'unknown command: /model-unknown' in '\n'.join(terminal.screen.display)
            assert Model.requests.empty(), 'unknown command reached the model'
            release.delta = {'content': '\n'.join(f'Answer {index}' for index in range(64))}
            release.set()
            terminal.pump(1)
            composer(terminal, '/model-unknown')
            terminal.send('\x15/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            print('PASS: busy commands stay local and retain editable input')
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)
    server.shutdown()
    server.server_close()


if __name__ == '__main__':
    main()
