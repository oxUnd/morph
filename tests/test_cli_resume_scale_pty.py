"""Measure full-history resume and verify old messages remain reachable.

Run with additional sizes for an opt-in benchmark:
    python tests/test_cli_resume_scale_pty.py build/bin/morph 1000 5000 10000
Timing is reported, with no machine-dependent performance assertion.
"""
from pathlib import Path
import sqlite3
import sys
import tempfile
import time

from test_cli_pty import Terminal, pexpect, pyte
from test_cli_transcript_pty import AlternateScreen


def seed(database, count):
    with sqlite3.connect(database) as db:
        session = db.execute('INSERT INTO sessions(display_id,name,model,created_at,'
                             'updated_at) VALUES(?,?,?,?,?)',
                             ('fade1234', 'Resume benchmark', 'pty-test', 1, 1)).lastrowid
        messages, history = [], []
        for index in range(count):
            question = f'QUESTION_{index:05d} Explain this result. 中文问题。'
            answer = (f'ANSWER_{index:05d}\n\n'
                      'A retained widget measures its content and preserves the complete '
                      'conversation. 中文内容用于检验正常换行。\n\n'
                      '- First item with **emphasis** and `inline_code`.\n'
                      '- Second item describes ownership and layout.\n\n'
                      '```c\nint value = 42;\nreturn value;\n```\n\n'
                      f'PERF_TAIL_{index:05d}')
            turn = f'bench-{index}'
            for role, kind, content in (('user', 'user_message', question),
                                        ('assistant', 'assistant_message', answer)):
                messages.append((session, role, content, turn, 100, index + 1))
                history.append((session, len(history) + 1, turn, kind, role,
                                content, 100, 1, index + 1))
        db.executemany('INSERT INTO messages(session_id,role,content,turn_id,token_count,'
                       'created_at) VALUES(?,?,?,?,?,?)', messages)
        db.executemany('INSERT INTO model_history_items(session_id,sequence_no,turn_id,'
                       'kind,role,content,token_count,active,created_at) '
                       'VALUES(?,?,?,?,?,?,?,?,?)', history)
    return session


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def wait(terminal, predicate, timeout=60):
    deadline = time.monotonic() + timeout
    while not predicate() or 2026 << 5 in terminal.screen.mode:
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        try:
            data = terminal.child.read_nonblocking(65536, timeout=.01)
        except pexpect.TIMEOUT:
            continue
        terminal.raw += data
        terminal.stream.feed(data)


def scroll_to(terminal, sequence, marker, count, draft, composer):
    for _ in range(count // 25 + 2):
        if marker in visible(terminal):
            return
        terminal.child.send(sequence * 100)
        terminal.pump(.08)
        assert draft in terminal.screen.display[composer], visible(terminal)
        assert terminal.screen.cursor.y == composer, visible(terminal)
    raise AssertionError(f'history marker unreachable: {marker}\n{visible(terminal)}')


def exercise(binary, count):
    with tempfile.TemporaryDirectory(prefix='morph-resume-scale-') as tmp:
        directory = Path(tmp)
        terminal = Terminal(binary, directory, 1, production=True, hooks_enabled=False,
                            extra_env={'NO_COLOR': ''})
        terminal.child.delaybeforesend = 0
        terminal.child.setwinsize(32, 100)
        terminal.screen = AlternateScreen(100, 32)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            database = directory / 'home' / '.morph' / 'data.db'
            session = seed(database, count)
            tail = f'PERF_TAIL_{count - 1:05d}'
            started = time.perf_counter()
            terminal.child.send('/resume fade1234\r')
            wait(terminal, lambda: tail in visible(terminal))
            restored = time.perf_counter() - started
            draft = 'PERF_DRAFT_中文'
            terminal.child.send(draft)
            wait(terminal, lambda: draft in visible(terminal))
            responsive = time.perf_counter() - started
            composer = next(index for index, row in enumerate(terminal.screen.display)
                            if draft in row)
            started = time.perf_counter()
            terminal.child.send('\x1b[5~')
            wait(terminal, lambda: tail not in visible(terminal))
            pageup = time.perf_counter() - started
            print(f'{count} turns: resume={restored:.6f}s ready={responsive:.6f}s '
                  f'pageup={pageup:.6f}s', flush=True)
            scroll_to(terminal, '\x1b[5~', 'QUESTION_00000', count, draft, composer)
            before = len(terminal.raw)
            terminal.screen.resize(lines=32, columns=60)
            terminal.child.setwinsize(32, 60)
            wait(terminal, lambda: '\x1b[?2026l' in terminal.raw[before:])
            assert 'QUESTION_00000' in visible(terminal), visible(terminal)
            assert draft in terminal.screen.display[composer], visible(terminal)
            scroll_to(terminal, '\x1b[6~', tail, count, draft, composer)
            assert tail in visible(terminal), visible(terminal)
            assert not terminal.screen.cursor.hidden, visible(terminal)
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT count(*) FROM messages WHERE session_id=?',
                                  (session,)).fetchone()[0] == count * 2
                assert db.execute('SELECT count(*) FROM model_history_items WHERE session_id=?',
                                  (session,)).fetchone()[0] == count * 2
            terminal.child.send('\x15/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=30)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
        finally:
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    counts = [int(value) for value in sys.argv[2:]] or [1000]
    for count in counts:
        assert count > 0
        exercise(binary, count)


if __name__ == '__main__':
    main()
