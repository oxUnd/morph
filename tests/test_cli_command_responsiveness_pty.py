"""A stalled slash command must leave editing, scrolling and cancellation live."""
import json
from pathlib import Path
import sys
import tempfile
import time

from test_cli_pty import Terminal, pexpect, pyte
from test_cli_transcript_pty import AlternateScreen


SERVER = '''import json, pathlib, sys, time
directory = pathlib.Path(sys.argv[1])
mode = sys.argv[2]
for line in sys.stdin:
    request = json.loads(line)
    if 'id' not in request:
        continue
    if request.get('method') == 'initialize':
        (directory / 'started').touch()
        while not (directory / 'release').exists():
            time.sleep(.01)
        if mode == 'failure':
            sys.exit(7)
        result = {'protocolVersion': '2025-06-18', 'capabilities': {},
                  'serverInfo': {'name': 'slow', 'version': '1'}}
    else:
        result = {}
    print(json.dumps({'jsonrpc': '2.0', 'id': request['id'],
                      'result': result}), flush=True)
'''


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def await_state(terminal, predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        terminal.pump(.02)
        if predicate() and 2026 << 5 not in terminal.screen.mode:
            return visible(terminal)
        assert terminal.child.isalive(), terminal.raw[-4000:]
    raise AssertionError(visible(terminal) + '\n' + terminal.raw[-4000:])


def exercise(binary, width, mode):
    with tempfile.TemporaryDirectory(prefix='morph-command-responsive-') as temporary:
        directory = Path(temporary)
        script = directory / 'server.py'
        script.write_text(SERVER)
        config = ('\n[[mcp.servers]]\nname="slow"\ntransport="stdio"\n'
                  f'command={json.dumps(sys.executable)}\n'
                  f'args={json.dumps([str(script), str(directory), mode])}\n'
                  'auto_connect=false\nconnect_timeout=3\n'
                  'protocol_version="2025-06-18"\n')
        terminal = Terminal(binary, directory, 9, production=True, hooks_enabled=False,
                            extra_config=config)
        terminal.child.setwinsize(32, width)
        terminal.screen = AlternateScreen(width, 32)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            terminal.send('/help\r')
            await_state(terminal, lambda: 'Use /help' in visible(terminal))
            terminal.send('/mcp connect slow\r')
            await_state(terminal, lambda: (directory / 'started').exists())
            frames = len(terminal.published_frames)
            terminal.child.send('draft中文')
            await_state(terminal, lambda: 'draft中文' in visible(terminal), .8)
            assert len(terminal.published_frames) > frames
            # Keyboard edits and wheel scrolling must paint while the server is held.
            terminal.child.send('\x7fZ')
            await_state(terminal, lambda: 'draft中Z' in visible(terminal), .8)
            before = tuple(terminal.screen.display[:-5])
            terminal.child.send('\x1b[<64;5;5M' * 8)
            await_state(terminal,
                        lambda: tuple(terminal.screen.display[:-5]) != before, .8)
            assert not (directory / 'release').exists()
            if mode == 'exit':
                terminal.child.send('\x04')
                terminal.child.expect(pexpect.EOF, timeout=3)
                terminal.child.close()
                assert terminal.child.exitstatus == 0, terminal.raw[-4000:]
                assert not (directory / 'release').exists()
                print(f'PASS stalled slash command: width={width}, outcome={mode}', flush=True)
                return

            terminal.send('\x1b')  # Return from scrollback before cancelling work.
            if mode == 'cancel':
                terminal.child.send('\x1b')
                await_state(terminal, lambda: 'cancelled' in visible(terminal), 1.5)
            elif mode == 'timeout':
                await_state(terminal, lambda: 'failed to connect' in visible(terminal), 5)
            else:
                (directory / 'release').touch()
                expected = 'failed to connect' if mode == 'failure' else "'slow' connected"
                await_state(terminal, lambda: expected in visible(terminal))
            assert 'draft中Z' in visible(terminal), visible(terminal)
            terminal.send('\x15/help /share\r')
            await_state(terminal, lambda: 'offline HTML' in visible(terminal))
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=5)
            terminal.child.close()
            assert terminal.child.exitstatus == 0, terminal.raw[-4000:]
            print(f'PASS stalled slash command: width={width}, outcome={mode}', flush=True)
        finally:
            (directory / 'release').touch()
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    for width in (120, 80, 60):
        for mode in ('success', 'failure', 'timeout', 'cancel', 'exit'):
            exercise(binary, width, mode)


if __name__ == '__main__':
    main()
