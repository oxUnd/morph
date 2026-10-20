"""Verify resume navigation on the actual CLI terminal, including screen redraw."""
from pathlib import Path
import sqlite3
import sys
import tempfile

from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte
import pexpect


def main():
    driver = Path(sys.argv[1]).resolve()
    artifacts = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='morph-resume-pty-') as temp:
        directory = Path(temp)
        terminal = Terminal(driver, directory, 1)
        terminal.child.setwinsize(24, 120)
        terminal.screen = AlternateScreen(120, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.pump(1)
            with sqlite3.connect(directory / 'data.db') as database:
                existing = database.execute('SELECT count(*) FROM sessions').fetchone()[0]
                for i in range(445 - existing):
                    database.execute(
                        'INSERT INTO sessions(display_id,name,model,created_at,updated_at) '
                        'VALUES(?,?,?,?,?)',
                        (f'{i:08x}', f'会话 {i:03d} 中文🙂 terminal redraw',
                         'pty-test', 100000 + i, 100000 + i))
                database.commit()
                ordered = database.execute(
                    'SELECT name FROM sessions ORDER BY updated_at DESC, id DESC').fetchall()

            rows, columns = 24, 120

            def frame(label, focus):
                screen = terminal.snapshot(label)
                page_size = rows - 12 if rows >= 14 else rows - 5
                start = focus // page_size * page_size
                end = min(start + page_size, 445)
                heading = 'Resume a session' if rows >= 14 else 'Sessions'
                assert screen.count(heading) == 1, screen
                assert f'{focus // page_size + 1} / {(444 // page_size) + 1}' in screen, screen
                assert f'Showing {start + 1}–{end} of 445' in screen, screen
                assert 'Esc' in terminal.screen.display[-1], screen
                assert '›' not in screen and '├' not in screen, screen
                data_rows = [line for line in screen.splitlines() if '会话 ' in line]
                assert len(data_rows) == end - start, screen
                selected = [r for r in range(rows) if
                            terminal.screen.buffer[r][2].bg == '1d3038']
                assert len(selected) == 1, screen
                selected_row = selected[0]
                assert ordered[focus][0][:12] in terminal.screen.display[selected_row], screen
                assert terminal.screen.buffer[selected_row][1].fg == '47d3e6', screen
                assert terminal.screen.buffer[selected_row][2].bold, screen
                assert terminal.screen.cursor.hidden, screen
                if artifacts:
                    (artifacts / f'{len(terminal.snapshots):02d}.txt').write_text(screen)
                return screen

            def resize(height, width, focus):
                nonlocal rows, columns
                rows, columns = height, width
                terminal.screen.resize(lines=height, columns=width)
                terminal.child.setwinsize(height, width)
                terminal.pump(0.5)
                return frame(f'resize to {height}x{width} without a key', focus)

            terminal.send('/resume\r')
            frame('initial picker: 12 rows', 0)
            terminal.send('\x1b[B' * 11)
            frame('down to last row', 11)
            terminal.send('\x1b[B')
            frame('down crosses page', 12)
            terminal.send('\x1b[B' * 7)
            frame('page two selection redraws in place', 19)
            terminal.send('\x1b[A')
            frame('up changes selection in place', 18)
            terminal.send('\x1b[D')
            frame('left page', 6)
            terminal.send('\x1b[C')
            frame('right page', 18)
            resize(32, 120, 18)
            resize(42, 120, 18)
            resize(22, 120, 18)
            narrow = resize(24, 80, 18)
            assert 'ID' not in narrow and 'TOKENS' in narrow, narrow
            narrow = resize(24, 60, 18)
            assert 'TOKENS' not in narrow and 'UPDATED' in narrow, narrow
            narrow = resize(24, 32, 18)
            assert 'UPDATED' not in narrow, narrow
            resize(24, 120, 18)
            idle_frames = terminal.raw.count('\x1b[?2026h')
            terminal.pump(0.5)
            assert terminal.raw.count('\x1b[?2026h') == idle_frames
            terminal.send('\r')
            restored = terminal.snapshot('Enter restores composer and selected session')
            assert 'resumed session: ' + ordered[18][0] in restored, restored
            assert 'Resume a session' not in restored, restored
            assert not terminal.screen.cursor.hidden, restored
            terminal.send('/resume\r')
            frame('reopen picker', 0)
            terminal.send('\x1b')
            restored = terminal.snapshot('Escape restores composer')
            assert 'Resume a session' not in restored, restored
            assert not terminal.screen.cursor.hidden, restored
            terminal.send('/resume\r')
            terminal.send('\x03')
            restored = terminal.snapshot('Ctrl C restores composer')
            assert 'Resume a session' not in restored, restored
            assert not terminal.screen.cursor.hidden, restored
            terminal.send('/resume\r')
            resize(8, 80, 0)
            terminal.send('\x1b[B' * 5)
            frame('small terminal crosses page without scrolling', 5)
            resize(24, 120, 5)
            terminal.send('q')
            terminal.send('/resume\r')
            terminal.send('\x1b[C' * 40)
            frame('last partial page and right boundary', 444)
            terminal.send('\x1b[B')
            frame('down stays at final session', 444)
            terminal.send('\r')
            restored = terminal.snapshot('Enter resumes final session')
            assert 'resumed session: ' + ordered[444][0] in restored, restored
            terminal.send('/new after-picker\r')
            assert 'created and switched to session: after-picker' in terminal.snapshot(
                'normal command input works after cancellation')
            terminal.send('/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            print('PASS: production CLI PTY redraw, navigation, selection, cancellation')
        finally:
            if artifacts:
                (artifacts / 'terminal.raw').write_text(terminal.raw)
                (artifacts / 'last-screen.txt').write_text('\n'.join(terminal.screen.display))
            terminal.child.close(force=True)


if __name__ == '__main__':
    main()
