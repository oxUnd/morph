"""Completion must preserve the bottom composer and its editable draft."""
from pathlib import Path
import re
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, http, pexpect, pyte


def footer_row(terminal):
    rows = [index for index, row in enumerate(terminal.screen.display)
            if 'morph ·' in row and ('ctx ' in row or 'context ' in row)]
    assert len(rows) == 1, '\n'.join(terminal.screen.display)
    return rows[0]


def scenario(driver, server, height, width, wrapped):
    with tempfile.TemporaryDirectory(prefix='morph-completion-pty-') as temp:
        terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
        release = None
        try:
            terminal.child.setwinsize(height, width)
            terminal.screen = pyte.Screen(width, height)
            terminal.stream = pyte.Stream(terminal.screen)
            terminal.wait_for_composer()
            terminal.send('completion layout probe\r')
            _, release = terminal.request()
            terminal.pump(0.7)
            draft = '' if wrapped is None else 'draft 中文🙂 ' + (
                'wrapped draft ' * 7 if wrapped else 'kept')
            terminal.send(draft)
            terminal.send('\x1b[D' * 3)
            cursor_column = terminal.screen.cursor.x
            cursor_distance = footer_row(terminal) - terminal.screen.cursor.y
            release.delta = {'content': 'FINAL_LAYOUT_MARKER\n' + '\n'.join(
                f'Answer line {index}' for index in range(height * 2))}
            release.set()
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                terminal.pump(0.05)
                if 'Worked for ' in '\n'.join(terminal.screen.display):
                    break
            terminal.pump(0.3)
            display = '\n'.join(terminal.screen.display)
            assert 'Worked for ' in display, display
            assert footer_row(terminal) == height - 1, display
            assert terminal.screen.cursor.x == cursor_column, display
            assert footer_row(terminal) - terminal.screen.cursor.y == cursor_distance, display
            assert sum(row.startswith('› ') for row in terminal.screen.display) == 1, display
            # Inspect each synchronized commit, not just the settled screen.
            replay = pyte.Screen(width, height)
            replay_stream = pyte.Stream(replay)
            completed_frames = 0
            for frame in re.split(r'(?<=\x1b\[\?2026l)', terminal.raw):
                replay_stream.feed(frame)
                if 'Worked for ' not in '\n'.join(replay.display):
                    continue
                completed_frames += 1
                footer = [index for index, row in enumerate(replay.display)
                          if 'morph ·' in row and ('ctx ' in row or 'context ' in row)]
                assert footer == [height - 1], '\n'.join(replay.display)
                assert sum(row.startswith('› ') for row in replay.display) == 1
            assert completed_frames, 'No completed layout commit was captured'
            # The finished layout must stay put on footer polls and the next edit.
            position = (terminal.screen.cursor.x, terminal.screen.cursor.y)
            terminal.pump(1.1)
            assert (terminal.screen.cursor.x, terminal.screen.cursor.y) == position
            terminal.send('X')
            assert footer_row(terminal) == height - 1, '\n'.join(terminal.screen.display)
            terminal.send('\x05\x15/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            print(f'PASS: completion keeps composer at bottom: {height}x{width}, wrapped={wrapped}')
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for height, width, wrapped in [(24, 80, None), (24, 120, False), (18, 40, True)]:
            scenario(driver, server, height, width, wrapped)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
