"""Transcript shortcuts handle terminal key press, repeat and release events."""
from pathlib import Path
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen, PacedMarkdownModel
import pyte


def check_key_lifecycle(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-transcript-key-') as temp:
                terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
                terminal.child.setwinsize(24, width)
                terminal.screen = AlternateScreen(width, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                try:
                    terminal.wait_for_composer()
                    draft = 'key draft中文🙂 ' + ('wrapped ' * 6 if width == 60 else '') + 'tail'
                    terminal.send(draft + '\x1b[D')
                    for press in ('\x14', '\x1b[116;5:1u'):
                        terminal.send(press)
                        opened = terminal.snapshot(f'{width}: Ctrl+T press opens transcript')
                        assert 'Turn details' in opened, opened
                        first_frame = len(terminal.published_frames)
                        terminal.send('\x1b[116;5:3u')
                        released = terminal.snapshot(f'{width}: Ctrl+T release keeps transcript open')
                        assert 'Turn details' in released, released
                        for frame, _ in terminal.published_frames[first_frame:]:
                            assert 'Turn details' in frame, frame
                        terminal.send('\x1b[116;5:1u')
                        closed = terminal.snapshot(f'{width}: next Ctrl+T press closes transcript')
                        assert 'Turn details' not in closed, closed
                        terminal.send('\x1b[116;5:2u\x1b[116;5:3u')
                        closed = terminal.snapshot(f'{width}: repeat and release do not reopen')
                        assert 'Turn details' not in closed, closed
                        terminal.send('\x1b[116;5:1u')
                        terminal.send('\x1b[116;5:2u\x1b[116;5:3u')
                        held = terminal.snapshot(f'{width}: holding Ctrl+T does not close transcript')
                        assert 'Turn details' in held, held
                        terminal.send('\x1b[114;1:1u\x1b[114;1:2u\x1b[114;1:3u')
                        raw = terminal.snapshot(f'{width}: R repeat and release preserve raw mode')
                        assert 'raw JSON' in terminal.screen.display[3], raw
                        terminal.send('\x1b[114;1:1u\x1b[114;1:2u\x1b[114;1:3u')
                        formatted = terminal.snapshot(f'{width}: next R press restores formatted mode')
                        assert 'raw JSON' not in terminal.screen.display[3], formatted
                        terminal.send('\x1b[27;1:1u\x1b[27;1:3u')
                        escaped = terminal.snapshot(f'{width}: Esc press and release close transcript')
                        assert 'Turn details' not in escaped, escaped
                    terminal.send('X')
                    restored = terminal.snapshot(f'{width}: key lifecycle preserves draft and caret')
                    assert 'key draft中文🙂' in restored and 'taiXl' in restored, restored
                    assert PacedMarkdownModel.requests.empty(), 'shortcut submitted a model turn'
                finally:
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    check_key_lifecycle(Path(sys.argv[1]).resolve())
