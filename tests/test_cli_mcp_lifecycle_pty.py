"""MCP lifecycle output belongs to the retained screen, never the parent shell."""
import http.server
from loopback_http import LoopbackHTTPServer
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, pexpect
from test_cli_retained_chat_pty import wait_visible


SERVER = '''import json
from pathlib import Path
import sys
import time

mode, started_name, release_name = sys.argv[1:]
started, release = Path(started_name), Path(release_name)
print("MCP_STDERR_MUST_STAY_PRIVATE", file=sys.stderr, flush=True)
for line in sys.stdin:
    request = json.loads(line)
    if request.get("method") != "initialize":
        continue
    started.write_text("initialize received")
    if mode != "immediate":
        while not release.exists():
            time.sleep(.01)
    print(json.dumps({"jsonrpc": "2.0", "id": request["id"],
                      "error": {"code": -32603,
                                "message": "FIXTURE_MCP_FAILURE"}}), flush=True)
'''


def wait_file(terminal, path):
    deadline = time.monotonic() + 8
    while not path.exists():
        assert time.monotonic() < deadline, terminal.raw[-4000:]
        terminal.pump(.05)


def wait_failure(terminal, server_name):
    deadline = time.monotonic() + 8
    while True:
        display = '\n'.join(terminal.screen.display)
        if (server_name in display and 'failed' in display.lower()
                and 2026 << 5 not in terminal.screen.mode):
            return display
        assert time.monotonic() < deadline, terminal.raw[-5000:]
        terminal.pump(.05)


def assert_screen_boundary(raw, server_name):
    entered = raw.find('\x1b[?1049h')
    left = raw.rfind('\x1b[?1049l')
    assert entered >= 0 and left > entered, raw[-4000:]
    for outside in (raw[:entered], raw[left + len('\x1b[?1049l'):]):
        assert f'MCP {server_name}' not in outside, outside
        assert 'Operation canceled' not in outside, outside
        assert 'FIXTURE_MCP_FAILURE' not in outside, outside
    assert 'MCP_STDERR_MUST_STAY_PRIVATE' not in raw, raw[-4000:]


def run_case(binary, model, mode, production=False):
    with tempfile.TemporaryDirectory(prefix=f'morph-mcp-{mode}-') as temp:
        directory = Path(temp)
        script = directory / 'fixture_mcp.py'
        script.write_text(SERVER)
        started, release = directory / 'started', directory / 'release'
        name = f'fixture_mcp_{mode}'
        arguments = [str(script), mode, str(started), str(release)]
        extra = ('\n[[mcp.servers]]\n'
                 f'name = {json.dumps(name)}\n'
                 'transport = "stdio"\n'
                 f'command = {json.dumps(sys.executable)}\n'
                 f'args = {json.dumps(arguments)}\n'
                 'auto_connect = true\n'
                 'connect_timeout = 30\n'
                 'protocol_version = "2025-06-18"\n')
        terminal = Terminal(binary, directory, model.server_port,
                            hooks_enabled=False, production=production,
                            extra_config=extra,
                            extra_env={'NO_COLOR': '1' if production else ''})
        try:
            terminal.wait_for_composer()
            wait_visible(terminal, 'perm: ask')
            wait_file(terminal, started)
            terminal.send('protected MCP draft')
            wait_visible(terminal, 'protected MCP draft')
            if mode == 'immediate':
                screen = wait_failure(terminal, name)
                assert 'protected MCP draft' in screen, screen
            elif mode == 'live':
                assert 'FIXTURE_MCP_FAILURE' not in terminal.raw
                release.write_text('fail initialization now')
                screen = wait_failure(terminal, name)
                assert 'protected MCP draft' in screen, screen
            else:
                assert not release.exists()
                assert 'FIXTURE_MCP_FAILURE' not in terminal.raw
            terminal.send('\x15/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            terminal.raw += terminal.child.before
            assert_screen_boundary(terminal.raw, name)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
            print(f'PASS MCP lifecycle: {mode}, production={production}', flush=True)
        finally:
            release.write_text('release fixture during cleanup')
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    production = Path(sys.argv[2]).resolve()
    model = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    model.daemon_threads = True
    threading.Thread(target=model.serve_forever, daemon=True).start()
    try:
        for mode in ('immediate', 'live', 'cancel'):
            run_case(driver, model, mode)
        run_case(production, model, 'cancel', production=True)
    finally:
        model.shutdown()
        model.server_close()


if __name__ == '__main__':
    main()
