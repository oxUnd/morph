"""An idle composer must survive background recap output and focus changes."""
import argparse
import http.server
import json
from pathlib import Path
import queue
import sqlite3
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, pyte
from test_cli_recap_pty import RecapModel
from test_cli_slash_pty import wait_for
from test_cli_composer_cleanup_pty import restored_composer_snapshot


class LiveRecapModel(Model):
    """Hold actual turn, title, and recap requests at deterministic boundaries."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        title = any('underlying task' in message.get('content', '')
                    for message in body.get('messages', [])
                    if message.get('role') == 'system')
        release = threading.Event()
        self.server.releases.append(release)
        if title:
            release.delta = {'content': '{"title":"Live recap composer"}'}
            self.server.titles.put(release)
        else:
            Model.requests.put((body, release))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            self.wfile.write(b': waiting\n\n')
            self.wfile.flush()
            if not release.wait(60):
                return
            delta = getattr(release, 'delta', {'content': getattr(release, 'answer', '')})
            payload = {'choices': [{'index': 0, 'delta': delta}]}
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
            self.wfile.write(b'data: {"choices":[{"delta":{},"finish_reason":"stop"}]}'
                            b'\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def all_rows(terminal):
    history = [''.join(cell.data for _, cell in sorted(row.items()))
               for row in terminal.screen.history.top]
    return history + terminal.screen.display


def one_composer(terminal, label):
    restored_composer_snapshot(terminal, label)
    rows = all_rows(terminal)
    prompts = [i for i, row in enumerate(rows)
               if row.split('Ctrl+T transcript')[0].strip() in ('>', '›')]
    hints = [i for i, row in enumerate(rows) if 'Ctrl+T transcript' in row]
    footers = [i for i, row in enumerate(rows) if 'perm: ' in row]
    diagnostic = (f'{label}\nprompts={prompts} hints={hints} footers={footers}\n' +
                  '\n'.join(f'{i:03}: {row}' for i, row in enumerate(rows)))
    assert len(prompts) == 1, diagnostic
    assert len(hints) == 0, diagnostic
    assert len(footers) == 1, diagnostic
    visible = prompts[0] - len(terminal.screen.history.top)
    assert visible >= 0, diagnostic
    assert terminal.screen.cursor.y == visible, diagnostic


def preserve_failure(terminal, artifacts, label, error):
    artifacts.mkdir(parents=True, exist_ok=True)
    stem = artifacts / label
    stem.with_suffix('.raw').write_text(terminal.raw)
    rows = all_rows(terminal)
    stem.with_suffix('.screen.txt').write_text(
        str(error) + '\n\n' + '\n'.join(f'{i:04}: {row}' for i, row in enumerate(rows)))
    stem.with_suffix('.snapshots.json').write_text(json.dumps(
        terminal.snapshots, ensure_ascii=False, indent=2))
    print(f'FAIL artifacts: {stem}', flush=True)


def recap_answer(marker):
    summary = marker + ' ' + (
        '已检查工作树并保留用户已有修改，完成代码验证后继续核对界面。'
        '后台摘要不得把旧的输入框和快捷键提示保留在终端历史中。'
    ) * 8
    assert len(summary) <= 700
    return json.dumps({'summary': summary,
                       'next_action': '继续检查焦点恢复后的输入框与状态栏。'},
                      ensure_ascii=False)


def exercise(driver, port, width, manual, artifacts):
    with tempfile.TemporaryDirectory(prefix='morph-recap-composer-') as temporary:
        directory = Path(temporary)
        environment = {} if manual else {'MORPH_PTY_ADVANCE_RECAP': '1'}
        terminal = Terminal(driver, directory, port, hooks_enabled=False,
                            extra_env=environment)
        terminal.child.setwinsize(32, width)
        terminal.screen = pyte.HistoryScreen(width, 32, history=2000)
        terminal.stream = pyte.Stream(terminal.screen)
        release = None
        try:
            terminal.wait_for_composer()
            with sqlite3.connect(directory / 'data.db') as db:
                db.execute("INSERT INTO sessions(display_id,name,model,created_at,updated_at) "
                           "VALUES('recapui1','recap-composer','pty-test',1,1)")
                session = db.execute(
                    "SELECT id FROM sessions WHERE name='recap-composer'").fetchone()[0]
                for number in range(4):
                    for role, text in [('user', f'Question {number}'),
                                       ('assistant', f'Completed answer {number}')]:
                        db.execute(
                            'INSERT INTO messages(session_id,role,content,token_count,created_at) '
                            'VALUES(?,?,?,?,?)', (session, role, text, 5, number + 1))
            terminal.send('/switch recap-composer\r')
            one_composer(terminal, f'idle before focus loss at {width} columns')
            terminal.send('\x1b[O')
            one_composer(terminal, f'unfocused idle at {width} columns')
            terminal.send('/recap\r' if manual else '\x1b[99;9~')
            body, release = terminal.request()
            assert 'tools' not in body, body
            one_composer(terminal, f'background recap pending at {width} columns')
            marker = f'RECAP_IDLE_{width}'
            release.answer = recap_answer(marker)
            release.set()
            wait_for(terminal, lambda: marker in terminal.raw and
                     'Generating recap' not in '\n'.join(terminal.screen.display))
            one_composer(terminal, f'recap committed while unfocused at {width} columns')
            terminal.send('\x1b[I')
            one_composer(terminal, f'focus restored after recap at {width} columns')
            terminal.send('\x1b[O\x1b[I')
            one_composer(terminal, f'repeated focus change at {width} columns')
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
        except Exception as error:
            preserve_failure(terminal, artifacts, f'seed-{width}', error)
            raise
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)


def wait_until(terminal, predicate, timeout=12):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        terminal.pump(.05)
        if predicate() and 2026 << 5 not in terminal.screen.mode:
            return
    raise AssertionError('\n'.join(terminal.screen.display))


def live_exercise(driver, server, width, manual, cycles, artifacts):
    with tempfile.TemporaryDirectory(prefix='morph-live-recap-composer-') as temporary:
        directory = Path(temporary)
        terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False,
                            extra_env={} if manual else {'MORPH_PTY_ADVANCE_RECAP': '1'},
                            extra_config='\n[permissions]\naccess = "full"\n'
                                         'approval = "never"\nreviewer = "user"\n')
        terminal.child.setwinsize(32, width)
        terminal.screen = pyte.HistoryScreen(width, 32, history=4000)
        terminal.stream = pyte.Stream(terminal.screen)
        turns = 0
        server.releases = []
        server.titles = queue.Queue()

        def turn(with_job=False, cycle=0):
            nonlocal turns
            terminal.send(f'Complete verification {turns + 1}; notify me when background work ends.\r')
            body, release = terminal.request()
            assert 'tools' in body, body
            if with_job:
                release.delta = {'tool_calls': [{
                    'index': 0, 'id': f'recap_job_{cycle}', 'type': 'function',
                    'function': {'name': 'exec', 'arguments': json.dumps({
                        'command': f'printf JOB_STARTED_{cycle}; '
                                   f'while [ ! -f release-job-{cycle} ]; do sleep .05; done; '
                                   f'printf JOB_FINISHED_{cycle}',
                        'description': f'Recap background verification {cycle}',
                        'background': True, 'notify_only': True})}}]}
                release.set()
                body, release = terminal.request()
                assert any(message.get('role') == 'tool'
                           for message in body.get('messages', [])), body
            release.delta = {'reasoning_content': 'Check the completed work before continuing.',
                             'content': f'LIVE_TURN_{turns + 1} 已完成界面验证，保留已有修改。'}
            release.set()
            turns += 1
            wait_until(terminal, lambda: terminal.raw.count('Worked for ') >= turns)
            one_composer(terminal, f'live completed turn {turns} at {width} columns')

        try:
            terminal.wait_for_composer()
            for cycle in range(cycles):
                for _ in range(2):
                    turn()
                turn(with_job=True, cycle=cycle)
                one_composer(terminal, f'live job and idle composer before recap {cycle}/{width}')
                terminal.send('\x1b[O')
                terminal.send('/recap\r' if manual else '\x1b[99;9~')
                body, recap = terminal.request()
                assert 'tools' not in body, body
                one_composer(terminal, f'live automatic recap pending {cycle}/{width}')
                marker = f'RECAP_LIVE_{cycle}_{width}'
                recap.answer = recap_answer(marker)
                if cycle % 2:
                    # Reverse the race: leave job rows live when recap commits,
                    # then let completion and expiry repaint an otherwise idle UI.
                    recap.set()
                    wait_until(terminal, lambda: marker in terminal.raw and
                               'Generating recap' not in '\n'.join(terminal.screen.display))
                    one_composer(terminal, f'live recap committed with running job {cycle}/{width}')
                # Completion posts a real process event and resizes the footer's
                # task rows while recap and the title worker are still pending.
                (directory / f'release-job-{cycle}').touch()
                wait_until(terminal, lambda: f'Recap background verification {cycle} · exited 0'
                           in '\n'.join(all_rows(terminal)))
                one_composer(terminal, f'live job completed during recap {cycle}/{width}')
                while not server.titles.empty():
                    server.titles.get_nowait().set()
                terminal.pump(.3)
                one_composer(terminal, f'live metadata completed during recap {cycle}/{width}')
                wait_until(terminal, lambda: not any('└─' in row
                           for row in terminal.screen.display), timeout=12)
                one_composer(terminal, f'live job expired during recap {cycle}/{width}')
                if not cycle % 2:
                    recap.set()
                    wait_until(terminal, lambda: marker in terminal.raw and
                               'Generating recap' not in '\n'.join(terminal.screen.display))
                one_composer(terminal, f'live recap committed while unfocused {cycle}/{width}')
                for focus in range(3):
                    terminal.send('\x1b[I')
                    one_composer(terminal, f'live focus restored {cycle}/{focus}/{width}')
                    terminal.send('\x1b[O')
                    one_composer(terminal, f'live focus lost {cycle}/{focus}/{width}')
                terminal.send('\x1b[I')
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
        except Exception as error:
            preserve_failure(terminal, artifacts, f'live-{width}', error)
            raise
        finally:
            for release in server.releases:
                release.set()
            terminal.child.close(force=True)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('driver', type=Path)
    parser.add_argument('--manual', action='store_true')
    parser.add_argument('--width', type=int, action='append')
    parser.add_argument('--live', action='store_true', help='Run real turns and jobs instead of seed data')
    parser.add_argument('--cycles', type=int, default=1)
    parser.add_argument('--artifacts', type=Path, default=Path('/tmp/morph-recap-composer-artifacts'))
    arguments = parser.parse_args()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0),
                                            LiveRecapModel if arguments.live else RecapModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    failures = []
    try:
        for width in arguments.width or (120, 80, 60, 200):
            try:
                if arguments.live:
                    live_exercise(arguments.driver.resolve(), server, width, arguments.manual,
                                  arguments.cycles, arguments.artifacts)
                else:
                    exercise(arguments.driver.resolve(), server.server_port, width,
                             arguments.manual, arguments.artifacts)
                print(f'PASS: background recap and one idle composer at {width} columns')
            except AssertionError as error:
                failures.append(str(error))
    finally:
        server.shutdown()
        server.server_close()
    assert not failures, '\n\n'.join(failures)


if __name__ == '__main__':
    main()
