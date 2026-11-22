"""MCP reverse requests use the real frontend interaction and preserve drafts."""
import json
import os
from pathlib import Path
import signal
import sys
import tempfile
import threading
import time

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until


SERVER = r'''import json
from pathlib import Path
import sys

for line in sys.stdin:
    request = json.loads(line)
    method = request.get('method')
    if method == 'initialize':
        capabilities = request['params']['capabilities']
        assert 'form' in capabilities['elicitation'], capabilities
        result = {'protocolVersion': '2025-11-25', 'capabilities': {'tools': {}},
                  'serverInfo': {'name': 'fixture', 'version': '1'},
                  'instructions': 'MCP_FIXTURE_GUIDANCE_MUST_REACH_THE_MODEL'}
    elif method == 'tools/list':
        result = {'tools': [{'name': 'form', 'description': 'Request a synthetic form',
                            'inputSchema': {'type': 'object', 'properties': {}},
                            'annotations': {'readOnlyHint': True}}]}
    elif method == 'tools/call':
        params = {'mode': 'form', 'message': 'Allow synthetic MCP form?',
                  'requestedSchema': {'type': 'object', 'properties': {}}}
        print(json.dumps({'jsonrpc': '2.0', 'id': 'peer-form',
                          'method': 'elicitation/create', 'params': params}), flush=True)
        answer = json.loads(sys.stdin.readline())
        assert answer['id'] == 'peer-form', answer
        Path(sys.argv[1]).write_text(json.dumps(answer))
        result = {'content': [{'type': 'text', 'text': json.dumps(answer['result'])}]}
    elif method == 'ping':
        result = {}
    else:
        continue
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'], 'result': result}), flush=True)
'''


def main():
    model = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-mcp-interaction-') as temporary:
            directory = Path(temporary)
            script = directory / 'server.py'
            script.write_text(SERVER)
            record = directory / 'answer.json'
            config = f'''[permissions]
access = "full"
approval = "never"
reviewer = "user"
[[mcp.servers]]
name = "fixture"
transport = "stdio"
command = {json.dumps(sys.executable)}
args = {json.dumps([str(script), str(record)])}
protocol_version = "2025-11-25"
auto_connect = true
'''
            terminal = Terminal(Path(sys.argv[1]).resolve(), directory, model.server_port,
                                production=True, hooks_enabled=False, extra_config=config)
            terminal.wait_for_composer()
            terminal.pump(.5)
            for width, action, keys in ((120, 'accept', '\r'),
                                        (80, 'decline', '\x1b[B\r'),
                                        (60, 'cancel', '\x1b[B\x1b[B\r')):
                terminal.child.setwinsize(24, width)
                terminal.screen.resize(24, width)
                terminal.send('\x15request synthetic form\r')
                body, release = terminal.request()
                releases.append(release)
                assert any(tool['function']['name'] == 'mcp_fixture__form'
                           for tool in body['tools']), body['tools']
                assert 'MCP_FIXTURE_GUIDANCE_MUST_REACH_THE_MODEL' in json.dumps(body)
                terminal.send('preserved MCP draft')
                release.delta = {'tool_calls': [{'index': 0, 'id': 'form-call',
                    'type': 'function', 'function': {'name': 'mcp_fixture__form',
                                                   'arguments': '{}'}}]}
                release.set()
                until(terminal, lambda text: 'Allow synthetic MCP form?' in text)
                assert Model.requests.empty(), 'MCP form was answered automatically in Full mode'
                terminal.send('\x1b[57353;1:1u\x1b[57353;1:3u')
                focused = [row.strip() for row in terminal.screen.display
                           if row.lstrip().startswith('› ●')]
                assert focused == ['› ● 2. Decline'], terminal.screen.display
                terminal.send('\x1b[57352;1:1u\x1b[57352;1:3u')
                focused = [row.strip() for row in terminal.screen.display
                           if row.lstrip().startswith('› ●')]
                assert focused == ['› ● 1. Accept'], terminal.screen.display
                if width == 120:
                    terminal.send('\x0b')
                    until(terminal, lambda text: 'Keyboard shortcuts' in text)
                    terminal.send('\x1b')
                    until(terminal, lambda text: 'Other · Type your own answer' in text)
                terminal.send(keys)
                body, release = terminal.request()
                releases.append(release)
                answer = json.loads(record.read_text())['result']
                assert answer['action'] == action, answer
                assert ('content' in answer) == (action == 'accept'), answer
                text = terminal.snapshot('draft after MCP interaction')
                assert 'preserved MCP draft' in text, text
                release.delta = {'content': 'MCP interaction completed.'}
                release.set()
                until(terminal, lambda text: 'Working ·' not in text)
            terminal.send('\x15request pending form\r')
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'tool_calls': [{'index': 0, 'id': 'pending-form',
                'type': 'function', 'function': {'name': 'mcp_fixture__form', 'arguments': '{}'}}]}
            release.set()
            until(terminal, lambda text: 'Other · Type your own answer' in text)
            os.kill(terminal.child.pid, signal.SIGTERM)
            deadline = time.monotonic() + 6
            while terminal.child.isalive() and time.monotonic() < deadline:
                terminal.pump(.05)
            assert not terminal.child.isalive(), 'shutdown blocked on the unanswered form'
            print('PASS MCP forms: Full mode, accept/decline/cancel, guidance, draft preservation')
    finally:
        for release in releases:
            release.set()
        if terminal is not None:
            terminal.child.close(force=True)
        model.shutdown()
        model.server_close()


if __name__ == '__main__':
    main()
