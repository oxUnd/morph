"""Check asynchronous MCP progress without disturbing readline input."""
import json
import os
from pathlib import Path
import sys
import tempfile
import time

try:
    import pexpect
    import pyte
except ImportError:
    sys.exit(77)


from test_cli_pty import terminal_environment


def main():
    driver = str(Path(sys.argv[1]).resolve())
    with tempfile.TemporaryDirectory(prefix='morph-cli-mcp-') as temporary:
        directory = Path(temporary)
        server = directory / 'server.py'
        server.write_text('''import json, pathlib, sys, time
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
        if name == 'broken':
            sys.exit(1)
        result = {'protocolVersion': '2025-06-18', 'capabilities': {},
                  'serverInfo': {'name': name, 'version': '1'}}
    else:
        continue
    print(json.dumps({'jsonrpc': '2.0', 'id': message['id'], 'result': result}), flush=True)
''')
        config = directory / 'config.toml'
        text = '[memory]\nenabled = false\n'
        for name in ('good', 'broken'):
            text += f'''[[mcp.servers]]
name = "{name}"
transport = "stdio"
command = {json.dumps(sys.executable)}
args = {json.dumps([str(server), name, str(directory / (name + '.gate'))])}
auto_connect = true
'''
        config.write_text(text)
        child = pexpect.spawn(driver, [str(config), temporary, str(directory / 'data.db')],
                              env=terminal_environment(directory),
                              encoding='utf-8', dimensions=(24, 100), timeout=8)
        screen = pyte.Screen(100, 24)
        stream = pyte.Stream(screen)

        def wait_for(predicate):
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                try:
                    stream.feed(child.read_nonblocking(65536, timeout=0.1))
                except pexpect.TIMEOUT:
                    pass
                if predicate('\n'.join(screen.display)):
                    return
            raise AssertionError('\n'.join(screen.display))

        try:
            wait_for(lambda text: 'MCP 0/2 ...' in text)
            child.send('keep-this-input')
            wait_for(lambda text: 'keep-this-input' in text)
            (directory / 'good.gate').touch()
            wait_for(lambda text: 'MCP 1/2 ...' in text and 'keep-this-input' in text)
            (directory / 'broken.gate').touch()
            wait_for(lambda text: 'MCP 1/2 !' in text and 'keep-this-input' in text)
            child.sendcontrol('u')
            child.send('/mcp\r')
            wait_for(lambda text: 'MCP servers (2)' in text and
                     'good' in text and 'ready' in text and 'failed' in text and
                     any(row.startswith('› ') and
                         all(screen.buffer[i][column].bg == '1c1c1c'
                             for column in range(screen.columns))
                         for i, row in enumerate(screen.display)))
            display = '\n'.join(screen.display)
            assert '› /mcp' not in display, display
            assert any(row.rstrip() == '/mcp' for row in screen.display), display
            prompts = [i for i, row in enumerate(screen.display) if row.startswith('› ')]
            assert len(prompts) == 1, display
            assert all(screen.buffer[prompts[0]][column].bg == '1c1c1c'
                       for column in range(screen.columns)), display
            child.send('/quit\r')
            child.expect(pexpect.EOF)
        finally:
            child.close(force=True)
            assert child.exitstatus in (0, None), child.exitstatus


if __name__ == '__main__':
    main()
