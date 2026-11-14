"""Retained alternate-screen chat: resize, draft, tools, scroll and modal return."""
import http.server
from loopback_http import LoopbackHTTPServer
import base64
import json
import re
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
    while (text not in '\n'.join(terminal.screen.display)
           or 2026 << 5 in terminal.screen.mode):
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        terminal.pump(.05)
    return '\n'.join(terminal.screen.display)


def assert_composer_box(terminal, row, color, lines=1):
    """Preserve the old full-width input surface and its two padding rows."""
    screen = terminal.screen
    display = '\n'.join(screen.display)
    assert 0 < row and row + lines < screen.lines, display
    background = '3a3a3a' if color else 'default'
    for y in range(row - 1, row + lines + 1):
        assert all(screen.buffer[y][x].bg == background
                   for x in range(screen.columns)), (y, display)
    if color:
        assert screen.buffer[row][0].fg == '8a8a8a', display
    assert not screen.display[row - 1].strip(), display
    assert not screen.display[row + lines].strip(), display


def assert_original_startup(terminal, width, color):
    """Rows and styles captured from the pre-migration c27cac4 CLI in a PTY."""
    screen = terminal.screen
    display = '\n'.join(screen.display)
    input_row = 13 if width == 60 else 10
    assert not screen.display[0].strip(), display
    assert screen.display[1].startswith('  morph'), display
    assert 'v0.' in screen.display[1], display
    assert screen.display[2].startswith('  ─'), display
    assert screen.display[3].startswith('  Workspace  '), display
    assert screen.display[4].startswith('  Model      pty-test'), display
    assert '/help' in display and '/switch' in display, display
    assert 'Enter Send' in display and 'Ctrl+Cmd+V Paste image' in display, display
    assert 'morph · conversation' not in display, display
    assert 'PgUp/PgDn scroll' not in display, display
    assert screen.display[input_row].startswith('› ' if color else '> '), display
    assert screen.cursor.y == input_row and screen.cursor.x == 2, display
    assert_composer_box(terminal, input_row, color)
    assert screen.display[input_row + 2].startswith('  morph · '), display
    assert ('context 0 / 128k  ·  0 credits' if width >= 100
            else 'ctx 0 / 128k · 0 cr') in screen.display[input_row + 2], display
    assert all(not row.strip() for row in screen.display[input_row + 3:]), display
    if color:
        assert screen.buffer[1][2].bold, display
        assert screen.buffer[1][2].fg == 'af87ff', display
        assert screen.buffer[2][2].fg == '444444', display


def assert_original_turn(terminal, color):
    """Verify message surfaces, Markdown, reasoning and completion receipts."""
    screen = terminal.screen
    display = '\n'.join(screen.display)
    user_row = next(i for i, row in enumerate(screen.display) if 'visual parity' in row)
    if color:
        assert user_row == 2, display
        assert_composer_box(terminal, user_row, color)
    assert screen.display[user_row].startswith('› ' if color else '> '), display
    answer_row = next(i for i, row in enumerate(screen.display) if 'VISUAL_ANSWER' in row)
    answer_x = screen.display[answer_row].index('VISUAL_ANSWER')
    assert answer_x == 2, display
    if color:
        assert screen.buffer[answer_row][answer_x].bold, display
        marker_row = next(i for i, row in enumerate(screen.display)
                          if row.startswith('● '))
        assert screen.buffer[marker_row][0].bold, display
    reasoning_row = next(i for i, row in enumerate(screen.display)
                         if 'reasoning first line' in row)
    assert screen.display[reasoning_row].rstrip() == 'reasoning first line…', display
    assert 'reasoning second line' not in display, display
    if color:
        assert screen.buffer[reasoning_row][0].italics, display
    duration_row = next(i for i, row in enumerate(screen.display) if 'Worked for' in row)
    assert re.fullmatch(r'  Worked for (?:\d+m )?\d+s · \d{2}:\d{2}',
                        screen.display[duration_row].rstrip()), display
    assert not screen.display[duration_row + 1].strip(), display
    assert all(screen.buffer[duration_row + 1][x].bg == 'default'
               for x in range(screen.columns)), display


def run_case(binary, server, width, color, production=False):
    with tempfile.TemporaryDirectory(prefix='morph-retained-chat-') as temp:
        directory = Path(temp)
        (directory / 'sample.txt').write_text('TOOL_BODY_中文\nsecond line\n')
        command_image = directory / 'command-image.png'
        command_image.write_bytes(base64.b64decode(
            'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/x8AAwMCAO+'
            'jRZkAAAAASUVORK5CYII='))
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
            assert_original_startup(terminal, width, color)
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
            terminal.send('/no-such-command ' + str(command_image) + '\r')
            unknown = wait_visible(terminal, 'unknown command')
            assert '/no-such-command /' in unknown, unknown
            assert '[IMAGE#' not in unknown, unknown
            terminal.send('\x1b[A')
            assert '/no-such-command' in '\n'.join(terminal.screen.display)
            terminal.send('\x15/clea\t')
            assert '/clear ' in '\n'.join(terminal.screen.display)
            terminal.send('\r')
            assert 'unknown command' not in '\n'.join(terminal.screen.display)
            terminal.send('visual parity\r')
            _, short_turn = terminal.request()
            releases.append(short_turn)
            short_turn.delta = {
                'reasoning_content': 'reasoning first line\nreasoning second line',
                'content': '**VISUAL_ANSWER**\n\nVisual paragraph.'}
            short_turn.set()
            wait_visible(terminal, 'Worked for')
            assert_original_turn(terminal, color)
            terminal.send('/clear\r')
            terminal.send('\x1b[200~first line\n中文🙂\x1b[201~')
            draft = wait_visible(terminal, '中文🙂')
            assert 'first line' in draft
            input_row = next(i for i, row in enumerate(terminal.screen.display)
                             if 'first line' in row)
            assert terminal.screen.display[input_row + 1].startswith('中文🙂'), draft
            assert_composer_box(terminal, input_row, color, lines=2)
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
            tool_draft_row = next(i for i, row in enumerate(tools.splitlines())
                                  if 'draft🙂' in row)
            assert tool_draft_row >= draft_row, tools
            assert_composer_box(terminal, tool_draft_row, color)
            terminal.send('\x14')
            details = wait_visible(terminal, 'TOOL_BODY_中文')
            assert 'Turn details' in details
            assert 'draft🙂' not in details
            terminal.send('\x14')
            returned = wait_visible(terminal, 'draft🙂')
            assert 'Turn details' not in returned
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
                assert '\x1b[48;5;237m' in terminal.raw
            else:
                assert '\x1b[48;' not in terminal.raw
            print(f'PASS retained chat: width={width}, color={color}', flush=True)
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)


def main():
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
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
