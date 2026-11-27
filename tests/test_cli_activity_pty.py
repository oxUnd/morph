"""The four-region dock stays ordered with one live status marker and held SSE."""
from pathlib import Path
import queue
import re
import sqlite3
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal, pexpect
from test_cli_agents_pty import wait_for
from test_cli_subagents_pty import FeedModel


STATUS = re.compile(r'^[◉◯?] (?:Working|Waiting for|Cancelling|Running tasks|Tasks pending)')


def check_rows(rows, draft, expected, back=False):
    diagnostic = '\n'.join(rows)
    editor = max(i for i, row in enumerate(rows) if row.startswith(('› ' + draft, '> ' + draft)))
    footer = [i for i, row in enumerate(rows) if 'perm: ' in row]
    assert footer == [len(rows) - 1], diagnostic
    header = max(i for i, row in enumerate(rows[:editor]) if STATUS.match(row))
    assert expected in rows[header], diagnostic
    for row in rows[header + 1:editor]:
        if not row.strip():
            continue
        assert row.startswith('  └─ '), diagnostic
        assert row.index('└') == 2, diagnostic
        assert not row[5:].lstrip().startswith(('◉', '◯', '●', '✖', '?', '–')), diagnostic
        assert '├' not in row and '│' not in row, diagnostic
    buttons = [i for i, row in enumerate(rows) if '↓ Back to bottom' in row]
    assert bool(buttons) == back, diagnostic
    if back:
        assert buttons == [header - 1], diagnostic
    assert editor == len(rows) - 3, diagnostic
    return editor


def scenario(binary, port, columns, color):
    with tempfile.TemporaryDirectory(prefix='morph-activity-pty-') as temporary:
        directory = Path(temporary)
        terminal = Terminal(binary, directory, port, production=True, hooks_enabled=False,
                            extra_env={'NO_COLOR': '' if color else '1'})
        replies = []
        try:
            terminal.child.setwinsize(32, columns)
            terminal.screen.resize(lines=32, columns=columns)
            terminal.wait_for_composer()
            terminal.send('/new Dock\r')
            wait_for(terminal, lambda: 'created and switched to session: Dock' in terminal.raw)
            terminal.send('EXERCISE_DOCK\r')
            _, initial = terminal.request()
            replies.append(initial)
            initial.stream({'content': '\n\n'.join(f'History line {i}' for i in range(70))})
            wait_for(terminal, lambda: 'History line 69' in '\n'.join(terminal.screen.display))
            terminal.send('DOCK_DRAFT')
            first_frame = len(terminal.published_frames)
            editor = check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Working')
            assert not initial.is_set()
            terminal.send('\x15QUEUE_FOLLOWUP\r')
            wait_for(terminal, lambda: 'Messages queued' in '\n'.join(terminal.screen.display))
            terminal.send('DOCK_DRAFT')
            assert check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Working') == editor
            terminal.send('\x1b[5~')
            wait_for(terminal, lambda: '↓ Back to bottom' in '\n'.join(terminal.screen.display))
            assert check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Working', back=True) == editor
            assert not initial.is_set() and Model.requests.empty()
            terminal.send('\x1b')
            wait_for(terminal, lambda: '↓ Back to bottom' not in '\n'.join(terminal.screen.display))
            assert Model.requests.empty(), 'returning to bottom consumed queued input'
            check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Working')
            for frame, _ in terminal.published_frames[first_frame:]:
                rows = frame.splitlines()
                if any(row.startswith(('› DOCK_DRAFT', '> DOCK_DRAFT')) for row in rows):
                    check_rows(rows, 'DOCK_DRAFT', 'Working',
                               back=any('↓ Back to bottom' in row for row in rows))
            terminal.send('\x1b')
            body, question = terminal.request()
            replies.append(question)
            assert any(message.get('content') == 'QUEUE_FOLLOWUP' for message in body['messages'])
            question.delta = {'tool_calls': [{
                'index': 0, 'id': 'dock-question', 'type': 'function',
                'function': {'name': 'ask_user', 'arguments':
                             '{"question":"DOCK_QUESTION","choices":["Continue","Stop"]}'}}]}
            question.set()
            wait_for(terminal, lambda: '? Waiting for your answer' in
                     '\n'.join(terminal.screen.display))
            rows = terminal.screen.display
            assert 'DOCK_DRAFT' not in '\n'.join(rows)
            assert 'perm: ' in rows[-1], '\n'.join(rows)
            assert sum(row.startswith('? Waiting for your answer') for row in rows) == 1
            if columns == 120 and color:
                terminal.send('\x1b[F')
                terminal.child.setwinsize(6, columns)
                terminal.screen.resize(lines=6, columns=columns)
                terminal.pump(.3)
                rows = terminal.screen.display
                assert any(row.lstrip().startswith('› ● 3.') for row in rows), '\n'.join(rows)
                assert 'perm: ' in rows[-1], '\n'.join(rows)
                terminal.child.setwinsize(32, columns)
                terminal.screen.resize(lines=32, columns=columns)
                terminal.send('\x1b[H')
            terminal.send('\r')
            _, final = terminal.request()
            replies.append(final)
            wait_for(terminal, lambda: 'DOCK_DRAFT' in '\n'.join(terminal.screen.display))
            check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Working')
            final.delta = {'content': 'DOCK_COMPLETE'}
            final.set()
            wait_for(terminal, lambda: 'Worked for' in '\n'.join(terminal.screen.display))
            database = directory / 'home/.morph/data.db'
            with sqlite3.connect(database) as db:
                db.execute('''INSERT INTO scheduled_tasks
                    (title,kind,status,trigger_type,next_run_at,action_type,created_at,updated_at)
                    VALUES ('dock task','reminder','pending','once',4102444800,'reminder',1,1)''')
            wait_for(terminal, lambda: 'Tasks 0 running' in '\n'.join(terminal.screen.display))
            assert check_rows(terminal.screen.display, 'DOCK_DRAFT', 'Tasks pending') == editor
            if color:
                branch = next(i for i, row in enumerate(terminal.screen.display)
                              if row.startswith('  └─ Tasks '))
                assert terminal.screen.buffer[branch][2].fg == '8a8a8a'
            else:
                assert all(cell.fg == 'default' and cell.bg == 'default'
                           for row in terminal.screen.buffer.values() for cell in row.values())
            with sqlite3.connect(database) as db:
                db.execute("UPDATE scheduled_tasks SET status='completed'")
            wait_for(terminal, lambda: 'Tasks pending' not in '\n'.join(terminal.screen.display))
            assert any(row.startswith(('› DOCK_DRAFT', '> DOCK_DRAFT')) for row in terminal.screen.display)
            terminal.send('\x15/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=10)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
            assert not terminal.duplicate_prompts
            print(f'PASS dock: columns={columns}, color={color}', flush=True)
        finally:
            for reply in replies:
                reply.set()
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), FeedModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            for color in (True, False):
                while True:
                    try:
                        Model.requests.get_nowait()
                    except queue.Empty:
                        break
                scenario(binary, server.server_port, columns, color)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
