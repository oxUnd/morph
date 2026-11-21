"""Background MCP progress is inspected in a popup without changing the chat."""
import json
from pathlib import Path
import sys
import tempfile
import time

from test_cli_pty import Terminal, pexpect, pyte
from test_cli_transcript_pty import AlternateScreen


POPUP_TITLE = 'MCP connections'
SERVER_NAMES = ('good-fixture', 'broken-fixture')
SERVER = '''import json, pathlib, sys, time
name, gate = sys.argv[1:]
for line in sys.stdin:
    message = json.loads(line)
    method = message.get('method')
    if method == 'server/discover':
        print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'error': {
            'code': -32601, 'message': 'Method not found'}}), flush=True)
        continue
    if method == 'initialize':
        while not pathlib.Path(gate).exists():
            time.sleep(0.02)
        if name == 'broken-fixture':
            sys.exit(1)
        result = {'protocolVersion': '2025-06-18', 'capabilities': {},
                  'serverInfo': {'name': name, 'version': '1'}}
    else:
        continue
    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)
'''


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def wait_for(terminal, predicate, timeout=8):
    deadline = time.monotonic() + timeout
    while True:
        text = visible(terminal)
        if predicate(text) and 2026 << 5 not in terminal.screen.mode:
            return text
        assert time.monotonic() < deadline, text + '\n' + terminal.raw[-4000:]
        terminal.pump(.05)


def click_mcp(terminal):
    rows = terminal.screen.display
    row = next(i for i, text in enumerate(rows) if 'perm: ' in text and 'MCP ' in text)
    column = rows[row].index('MCP ') + 1
    terminal.send(f'\x1b[<0;{column + 1};{row + 1}M'
                  f'\x1b[<0;{column + 1};{row + 1}m')
    return wait_for(terminal, lambda text: POPUP_TITLE in text and 'Esc close' in text)


def assert_quiet(text, names=SERVER_NAMES):
    for name in names:
        assert name not in text, text
    assert 'Connecting' not in text and 'Discovering capabilities' not in text, text
    assert 'FIXTURE_MCP_FAILURE' not in text, text


def assert_status(text, name, status):
    rows = [row for row in text.splitlines() if name in row]
    statuses = (status,) if isinstance(status, str) else status
    assert len(rows) == 1 and any(value in rows[0] for value in statuses), text
    for label, marker in (('Connecting', '◉'), ('Disconnected', '◯'),
                          ('Ready', '●'), ('Failed', '✖')):
        if label in rows[0]:
            assert rows[0].lstrip(' │').startswith(marker), text
    assert f'MCP {name}' not in rows[0], text
    return rows[0]


def has_status(text, name, status):
    return any(name in row and status in row for row in text.splitlines())


def assert_popup_background(terminal, left, color):
    rows = terminal.screen.display
    top = next(y for y, row in enumerate(rows) if POPUP_TITLE in row)
    bottom = next(y for y in range(top + 1, len(rows)) if '└' in rows[y][left:left + 1])
    expected = '303030' if color else 'default'
    for y in range(top + 1, bottom):
        for x in range(left + 1, len(rows[y]) - 2):
            cell = terminal.screen.buffer[y][x]
            assert cell.bg == expected, (y, x, cell)
    return bottom


def draft_visible(text):
    return 'keep-this-input' in text and 'TAIL' in text


def composer_rows(terminal):
    start = next(i for i, row in enumerate(terminal.screen.display)
                 if row.startswith(('› ', '> ')))
    return tuple(terminal.screen.display[start:terminal.screen.cursor.y + 1])


def run_case(driver, width, color, production=False):
    with tempfile.TemporaryDirectory(prefix='morph-cli-mcp-') as temporary:
        directory = Path(temporary)
        server = directory / 'server.py'
        server.write_text(SERVER)
        extra = ''
        for name in SERVER_NAMES:
            extra += f'''\n[[mcp.servers]]
name = "{name}"
transport = "stdio"
command = {json.dumps(sys.executable)}
args = {json.dumps([str(server), name, str(directory / (name + '.gate'))])}
auto_connect = true
protocol_version = "2025-06-18"
connect_timeout = 30
'''
        terminal = Terminal(driver, directory, 9, hooks_enabled=False,
                            extra_config=extra, production=production,
                            extra_env={'NO_COLOR': '' if color else '1'})
        terminal.child.setwinsize(30, width)
        terminal.screen = AlternateScreen(width, 30)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            wait_for(terminal, lambda text: 'MCP 0/2 ...' in text)
            draft = 'keep-this-input 中文 ' * 3 + 'TAIL'
            terminal.send(draft + '\x1b[D\x1b[D\x1b[D')
            initial = wait_for(terminal, draft_visible)
            assert_quiet(initial)
            cursor = (terminal.screen.cursor.y, terminal.screen.cursor.x)
            original_rows = composer_rows(terminal)
            original_screen = tuple(terminal.screen.display)
            assert len(original_rows) >= (2 if width == 60 else 1), initial
            first_frame = len(terminal.published_frames)

            pending = click_mcp(terminal)
            assert_status(pending, SERVER_NAMES[0], 'Connecting')
            assert_status(pending, SERVER_NAMES[1], ('Disconnected', 'Connecting'))
            assert draft_visible(pending), pending
            assert (terminal.screen.cursor.y, terminal.screen.cursor.x) == cursor, pending
            assert composer_rows(terminal) == original_rows, pending
            assert pending.count(POPUP_TITLE) == 1, pending
            title_row = next(row for row in terminal.screen.display if POPUP_TITLE in row)
            left = next((i for i, value in enumerate(title_row) if value in '┌╭'), -1)
            assert left > 0, pending
            bottom = assert_popup_background(terminal, left, color)
            assert bottom < cursor[0] - 1, pending
            for y, row in enumerate(terminal.screen.display):
                assert row[:left] == original_screen[y][:left], pending

            terminal.send('Z')
            assert POPUP_TITLE in visible(terminal), visible(terminal)
            terminal.send('\x7f')
            assert composer_rows(terminal) == original_rows, visible(terminal)

            (directory / (SERVER_NAMES[0] + '.gate')).touch()
            live = wait_for(terminal, lambda text:
                            has_status(text, SERVER_NAMES[0], 'Ready') and
                            has_status(text, SERVER_NAMES[1], 'Connecting'))
            assert_status(live, SERVER_NAMES[0], 'Ready')
            assert_status(live, SERVER_NAMES[1], 'Connecting')
            assert live.count('Connecting') == 1, live
            terminal.send('\x1b')
            returned = wait_for(terminal, lambda text: draft_visible(text) and
                                POPUP_TITLE not in text)
            assert_quiet(returned)
            assert (terminal.screen.cursor.y, terminal.screen.cursor.x) == cursor, returned
            assert composer_rows(terminal) == original_rows, returned

            (directory / (SERVER_NAMES[1] + '.gate')).touch()
            failed = wait_for(terminal, lambda text: 'MCP 1/2 !' in text and draft_visible(text))
            assert_quiet(failed)
            assert (terminal.screen.cursor.y, terminal.screen.cursor.x) == cursor, failed
            details = click_mcp(terminal)
            assert_status(details, SERVER_NAMES[0], 'Ready')
            failure = assert_status(details, SERVER_NAMES[1], 'Failed')
            assert failure.lstrip(' │').startswith('✖'), details
            assert 'Connecting' not in details and 'Discovering capabilities' not in details
            assert any(row.lstrip(' │').startswith(('└ ', '├ '))
                       for row in details.splitlines()), details
            assert draft_visible(details), details
            assert composer_rows(terminal) == original_rows, details
            assert_popup_background(terminal, left, color)
            terminal.send('\x1b[<0;1;1M\x1b[<0;1;1m')
            returned = wait_for(terminal, lambda text: draft_visible(text) and
                                POPUP_TITLE not in text)
            assert_quiet(returned)
            assert (terminal.screen.cursor.y, terminal.screen.cursor.x) == cursor, returned
            assert composer_rows(terminal) == original_rows, returned
            assert 'closed' not in returned.lower(), returned
            for frame, _ in terminal.published_frames[first_frame:]:
                if draft_visible(frame) and POPUP_TITLE not in frame:
                    assert_quiet(frame)
            if not color:
                for y, row in terminal.screen.buffer.items():
                    for x, cell in row.items():
                        assert cell.fg == 'default' and cell.bg == 'default', (y, x, cell)
            else:
                y = next(i for i, row in enumerate(terminal.screen.display)
                         if 'perm: ' in row and 'MCP ' in row)
                row = terminal.screen.display[y]
                mcp = terminal.screen.buffer[y][row.index('MCP ')]
                label = terminal.screen.buffer[y][row.index('perm: ')]
                assert mcp.fg != label.fg and mcp.underscore, (mcp, label)

            terminal.send('\x05\x15/mcp\r')
            queried = wait_for(terminal, lambda text: 'MCP servers (2)' in text and
                               SERVER_NAMES[0] in text and 'ready' in text and 'failed' in text)
            assert '› /mcp' not in queried and '> /mcp' not in queried, queried
            assert any(row.rstrip() == '/mcp' for row in terminal.screen.display), queried
            prompts = [i for i, row in enumerate(terminal.screen.display)
                       if row.startswith(('› ', '> '))]
            assert len(prompts) == 1, queried
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            print(f'PASS MCP popup: width={width}, color={color}', flush=True)
        finally:
            for name in SERVER_NAMES:
                (directory / (name + '.gate')).touch()
            terminal.child.close(force=True)
            assert terminal.child.exitstatus in (0, None), terminal.child.exitstatus


def main():
    driver = Path(sys.argv[1]).resolve()
    production = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else driver.with_name('morph')
    for width in (120, 80, 60):
        run_case(driver, width, True)
    run_case(production, 80, False, production=True)


if __name__ == '__main__':
    main()
