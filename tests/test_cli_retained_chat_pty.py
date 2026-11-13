"""Retained alternate-screen chat: resize, draft, tools, scroll and modal return."""
import http.server
import json
import sqlite3
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
import pyte
import pexpect


def wait_visible(terminal, text, timeout=8):
    deadline = time.monotonic() + timeout
    while text not in '\n'.join(terminal.screen.display):
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        terminal.pump(.05)
    return '\n'.join(terminal.screen.display)


def run_case(binary, server, width, color, production=False):
    with tempfile.TemporaryDirectory(prefix='morph-retained-chat-') as temp:
        directory = Path(temp)
        (directory / 'sample.txt').write_text('TOOL_BODY_中文\nsecond line\n')
        terminal = Terminal(binary, directory, server.server_port, hooks_enabled=False,
                            production=production,
                            extra_env={'NO_COLOR': '' if color else '1'})
        releases = []
        try:
            terminal.child.setwinsize(24, width)
            terminal.screen = pyte.Screen(width, 24)
            terminal.stream = pyte.Stream(terminal.screen)
            terminal.wait_for_composer()
            wait_visible(terminal, 'perm: ask')
            assert '\x1b[?1049h' in terminal.raw
            if width == 120:
                terminal.pump(1.2)
                idle_bytes = len(terminal.raw)
                terminal.pump(1.2)
                assert len(terminal.raw) == idle_bytes, terminal.raw[idle_bytes:]
                terminal.send('notification draft')
                with sqlite3.connect(directory / 'data.db') as database:
                    database.execute("""INSERT INTO scheduled_tasks
                        (title,kind,status,trigger_type,next_run_at,action_type,payload_json,
                         created_at,updated_at)
                        VALUES ('Retained notification','reminder','pending','once',1,
                                'reminder',?,1,1)""",
                        (json.dumps({'message': '**NOTIFY_DONE**\n\n| a | b |\n|---|---|\n| kept | rich |'}),))
                notification = wait_visible(terminal, 'NOTIFY_DONE')
                assert notification.count('NOTIFY_DONE') == 1, notification
                assert '**NOTIFY_DONE**' not in notification
                assert 'notification draft' in notification
                terminal.send('\x15')
            terminal.send('/no-such-command\r')
            wait_visible(terminal, 'unknown command')
            terminal.send('\x1b[A')
            assert '/no-such-command' in '\n'.join(terminal.screen.display)
            terminal.send('\x15/clea\t')
            assert '/clear ' in '\n'.join(terminal.screen.display)
            terminal.send('\r')
            assert 'unknown command' not in '\n'.join(terminal.screen.display)
            terminal.send('\x1b[200~first line\n中文🙂\x1b[201~')
            draft = wait_visible(terminal, '中文🙂')
            assert 'first line' in draft
            assert Model.requests.empty(), 'paste must not submit'
            terminal.send('\r')
            request, release = terminal.request()
            releases.append(release)
            user = [item for item in request['messages'] if item['role'] == 'user'][-1]
            assert 'first line\n中文🙂' in user['content'], user
            terminal.send('draft🙂')
            before = wait_visible(terminal, 'draft🙂')
            draft_row = next(i for i, row in enumerate(before.splitlines()) if 'draft🙂' in row)
            release.delta = {'tool_calls': [{'index': 0, 'id': 'retained-read',
                'type': 'function', 'function': {'name': 'file_read',
                    'arguments': json.dumps({'file_path': str(directory / 'sample.txt')})}}]}
            release.set()
            response, follow = terminal.request()
            releases.append(follow)
            assert any(item['role'] == 'tool' for item in response['messages'])
            tools = wait_visible(terminal, 'file_read')
            assert 'draft🙂' in tools
            assert next(i for i, row in enumerate(tools.splitlines()) if 'draft🙂' in row) == draft_row
            terminal.send('\x14')
            details = wait_visible(terminal, 'TOOL_BODY_中文')
            assert 'draft🙂' in details
            terminal.send('\x14')
            follow.delta = {'content': '\n\n' + '\n'.join(f'ROW_{i:03d}' for i in range(45))}
            follow.set()
            completed = wait_visible(terminal, 'Worked for')
            assert 'ROW_044' in completed and 'draft🙂' in completed
            terminal.send('\x1b[5~')
            scrolled = '\n'.join(terminal.screen.display)
            assert 'ROW_044' not in scrolled
            assert 'draft🙂' in scrolled
            terminal.send('\x1b[6~')
            wait_visible(terminal, 'ROW_044')
            new_width = 80 if width == 60 else 60
            terminal.child.setwinsize(24, new_width)
            terminal.screen.resize(24, new_width)
            wait_visible(terminal, 'draft🙂')
            if width == 80 and color:
                terminal.send('\x15ask a question\r')
                _, ask = terminal.request()
                releases.append(ask)
                terminal.send('protected draft')
                ask.delta = {'tool_calls': [{'index': 0, 'id': 'retained-read',
                    'type': 'function', 'function': {'name': 'ask_user',
                        'arguments': json.dumps({'question': 'Choose a color',
                                                 'choices': ['Blue', 'Red']})}}]}
                ask.set()
                wait_visible(terminal, 'Choose a color')
                terminal.send('1\r')
                answered, answer = terminal.request()
                releases.append(answer)
                assert any(item['role'] == 'tool' and 'Blue' in item['content']
                           for item in answered['messages']), answered
                wait_visible(terminal, 'protected draft')
                answer.delta = {'content': 'QUESTION_DONE'}
                answer.set()
                wait_visible(terminal, 'QUESTION_DONE')
                terminal.pump(.5)
                terminal.send('\x1b[5~' * 8)
                wait_visible(terminal, 'file_read')
                terminal.send('\x1b[6~' * 8)
                wait_visible(terminal, 'QUESTION_DONE')
                terminal.send('\x15\x1b[200~after question\nsecond line\x1b[201~')
                restored_input = wait_visible(terminal, 'second line')
                assert 'after question' in restored_input
            terminal.send('\x15/model\r')
            wait_visible(terminal, 'Normal')
            terminal.send('\x1b')
            restored = wait_visible(terminal, 'perm: ask')
            assert ('QUESTION_DONE' if width == 80 and color else 'ROW_044') in restored
            terminal.send('\x15/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            terminal.raw += terminal.child.before
            assert '\x1b[?1049l' in terminal.raw
            if color:
                assert '\x1b[48;2;58;58;58m' in terminal.raw
            else:
                assert '\x1b[48;2;' not in terminal.raw
            print(f'PASS retained chat: width={width}, color={color}', flush=True)
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)


def main():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width, color in ((120, True), (80, True), (60, True), (80, False)):
            binary = Path(sys.argv[1] if color else sys.argv[2]).resolve()
            run_case(binary, server, width, color, production=not color)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
