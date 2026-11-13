"""MCP command trees in the retained feed, including capability lists."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, pexpect, pyte
from test_cli_retained_chat_pty import wait_visible


SERVER_NAME = 'Fixture MCP 中文🙂 with a longer name that wraps across narrow terminals'
FIRST_DESCRIPTION = ('Read the complete original document without changing its contents '
                     'or losing the description when the terminal is resized.')
FIRST_URI = 'fixture://documents/first/' + 'nested-segment-' * 5 + 'end'

SERVER = ('SERVER_NAME = ' + repr(SERVER_NAME) + '\n'
          'FIRST_DESCRIPTION = ' + repr(FIRST_DESCRIPTION) + '\n'
          'FIRST_URI = ' + repr(FIRST_URI) + '\n') + '''import json
import sys

for line in sys.stdin:
    request = json.loads(line)
    if "id" not in request:
        continue
    method = request["method"]
    if method == "initialize":
        result = {"protocolVersion": "2025-06-18",
                  "capabilities": {"tools": {}, "resources": {}, "prompts": {}},
                  "serverInfo": {"name": SERVER_NAME, "version": "1.2.3"}}
    elif method == "tools/list":
        result = {"tools": [
            {"name": "echo", "description": "Return the supplied text",
             "inputSchema": {"type": "object"}},
            {"name": "inspect", "description": "Inspect a resource without changing it",
             "inputSchema": {"type": "object"}}]}
    elif method == "resources/list":
        result = {"resources": [
            {"name": "Fixture document", "description": FIRST_DESCRIPTION,
             "uri": FIRST_URI, "mimeType": "text/plain"},
            {"name": "Fixture image", "description": "Read the second document",
             "uri": "fixture://documents/second", "mimeType": "image/png"}]}
    elif method == "prompts/list":
        result = {"prompts": [
            {"name": "explain", "description": "Explain the supplied document"},
            {"name": "summarize", "description": "Summarize the supplied document"}]}
    else:
        result = {}
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"], "result": result}), flush=True)
'''


def assert_tree(screen, names):
    for name in names:
        row = next((line for line in screen.splitlines() if name in line), None)
        assert row is not None, screen
        assert '├ ' in row or '└ ' in row, row
    assert '├ ' in screen and '└ ' in screen, screen


def compact_tree(text):
    return ''.join(character for character in text if character not in '│├└ \t\r\n')


def resize(terminal, columns, expected):
    start = len(terminal.raw)
    terminal.child.setwinsize(40, columns)
    terminal.screen.resize(40, columns)
    deadline = time.monotonic() + 8
    while ('\x1b[H\x1b[2J' not in terminal.raw[start:]
           or 2026 << 5 in terminal.screen.mode):
        assert time.monotonic() < deadline, terminal.raw[-4000:]
        terminal.pump(.05)
    return wait_visible(terminal, expected)


def run_case(binary, model, width, color, production):
    with tempfile.TemporaryDirectory(prefix='morph-mcp-tree-') as temp:
        directory = Path(temp)
        script = directory / 'fixture_mcp.py'
        script.write_text(SERVER)
        config = ('\n[[mcp.servers]]\n'
                  'name = "fixture_tree"\ntransport = "stdio"\n'
                  f'command = {json.dumps(sys.executable)}\n'
                  f'args = {json.dumps([str(script)])}\n'
                  'auto_connect = true\nprotocol_version = "2025-06-18"\n'
                  '\n[[mcp.servers]]\n'
                  'name = "manual_peer"\ntransport = "stdio"\n'
                  'command = "not-started-by-this-test"\nauto_connect = false\n')
        terminal = Terminal(binary, directory, model.server_port, hooks_enabled=False,
                            production=production, extra_config=config,
                            extra_env={'NO_COLOR': '' if color else '1'})
        try:
            terminal.child.setwinsize(40, width)
            terminal.screen = pyte.Screen(width, 40)
            terminal.stream = pyte.Stream(terminal.screen)
            terminal.wait_for_composer()
            wait_visible(terminal, 'MCP 1/2')
            terminal.send('/clear\r/mcp\r')
            screen = wait_visible(terminal, 'Capabilities')
            assert 'MCP servers (2)' in screen, screen
            ready = next(row for row in screen.splitlines() if 'fixture_tree [stdio]' in row)
            manual = next(row for row in screen.splitlines() if 'manual_peer [stdio]' in row)
            assert ready.startswith('● fixture_tree [stdio]'), ready
            assert manual.startswith('◯ manual_peer [stdio]'), manual
            assert 'ready' in ready and 'disconnected' in manual, screen
            assert 'Fixture MCP' in screen and '2025-06-18' in screen, screen
            assert compact_tree(SERVER_NAME) in compact_tree(screen), screen
            for capability in ('Tools: yes', 'Resources: yes', 'Prompts: yes'):
                assert capability in screen, screen
            assert '│ ' in screen, screen
            alternate_width = 60 if width == 80 else 80
            resized = resize(terminal, alternate_width, 'manual_peer')
            assert compact_tree(SERVER_NAME) in compact_tree(resized), resized
            assert '2025-06-18' in resized and 'Capabilities' in resized, resized
            assert '│ ' in resized and '└ ' in resized, resized
            resize(terminal, width, 'manual_peer')
            for command, heading, names in (
                    ('tools', 'MCP tools', ('echo', 'inspect')),
                    ('resources', 'MCP resources', ('Fixture document', 'Fixture image')),
                    ('prompts', 'MCP prompts', ('explain', 'summarize'))):
                terminal.send('/clear\r/mcp ' + command + ' fixture_tree\r')
                screen = wait_visible(terminal, names[-1])
                assert heading in screen, screen
                assert_tree(screen, names)
                if command == 'resources':
                    assert FIRST_URI in compact_tree(screen), screen
                    assert compact_tree(FIRST_DESCRIPTION) in compact_tree(screen), screen
                    assert 'fixture://documents/second' in screen, screen
                    resized = resize(terminal, alternate_width, 'Fixture image')
                    assert_tree(resized, names)
                    assert FIRST_URI in compact_tree(resized), resized
                    assert compact_tree(FIRST_DESCRIPTION) in compact_tree(resized), resized
                    assert 'fixture://documents/second' in resized, resized
                    resize(terminal, width, 'Fixture image')
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            terminal.raw += terminal.child.before
            terminal.child.close()
            assert terminal.child.exitstatus == 0, terminal.raw[-4000:]
            if not color:
                assert '\x1b[48;5;' not in terminal.raw
            print(f'PASS MCP tree: width={width}, color={color}', flush=True)
        finally:
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    production = Path(sys.argv[2]).resolve()
    model = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    try:
        run_case(driver, model, 80, True, False)
        run_case(production, model, 60, False, True)
    finally:
        model.shutdown()
        model.server_close()


if __name__ == '__main__':
    main()
