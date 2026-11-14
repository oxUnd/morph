"""Inline model mode selection, live Ultra ignition and editable prompt geometry."""
import http.server
from loopback_http import LoopbackHTTPServer
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import shutil
import time
import tomllib

from test_cli_pty import Model, Terminal, terminal_environment
from cli_inline_test_support import inline_panel, no_screen_switch
from test_cli_slash_pty import wait_for, command_heading
from test_cli_resume_pty import AlternateScreen
from test_cli_reflow_pty import NativeScreen, NativeUnavailable
import pyte


def composer(terminal, ultra=False):
    marker = '» ' if ultra else '› '
    rows = terminal.screen.display
    candidates = [i for i, line in enumerate(rows) if line.startswith(marker)]
    assert candidates, '\n'.join(rows)
    row = candidates[-1]
    command = max((i for i, line in enumerate(rows) if line.startswith('/')), default=-1)
    assert sum(line.startswith(('› ', '» ')) for line in rows[command + 1:]) == 1, '\n'.join(rows)
    assert terminal.screen.buffer[row][0].fg == ('af87ff' if ultra else '8a8a8a'), '\n'.join(rows)
    return row


def settled(terminal, ultra=False):
    terminal.pump(1.1)
    row = composer(terminal, ultra)
    boundary = max((i for i, line in enumerate(terminal.screen.display) if line.startswith('/')), default=-1)
    backgrounds = [i for i in range(boundary + 1, terminal.screen.lines)
                   if terminal.screen.buffer[i][0].bg == '3a3a3a']
    assert backgrounds == [row - 1, row, row + 1], (backgrounds, '\n'.join(terminal.screen.display))
    return row


def panel(terminal, ultra=False):
    wait_for(terminal, lambda: 'Select Model Mode' in '\n'.join(terminal.screen.display))
    rows = terminal.screen.display
    title = next(i for i, line in enumerate(rows) if 'Select Model Mode' in line)
    heading = max(i for i, line in enumerate(rows[:title]) if line.startswith('/'))
    assert title == heading + 2, '\n'.join(rows)
    assert not any(line.startswith(('› ', '» ')) for line in rows[heading + 1:]), '\n'.join(rows)
    selected = next(i for i, line in enumerate(rows) if line.startswith('> '))
    assert ('Ultra (current)' if ultra else 'Normal (current)') in rows[selected], '\n'.join(rows)
    assert terminal.screen.buffer[selected][0].bg in ('blue', '0000ee')
    marker = getattr(terminal, 'model_history_marker', None)
    if marker:
        inline_panel(terminal, 'Select Model Mode', marker, maximum=10)


def native_reflow(driver, server):
    kitty = shutil.which('kitty')
    if not kitty:
        print('Native Ultra reflow: Kitty unavailable', flush=True)
        return
    try:
        screen = NativeScreen(kitty)
    except NativeUnavailable as error:
        print(f'Native Ultra reflow unavailable: {error}', flush=True)
        return
    with tempfile.TemporaryDirectory(prefix='morph-model-native-') as temp:
        terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        try:
            terminal.wait_for_composer()
            screen.resize(24, 80)
            screen.feed(terminal.raw)
            terminal.screen = screen
            terminal.stream = screen
            draft = 'NATIVE_ULTRA_DRAFT 中文 ' + 'wrapped/' * 24
            terminal.send(draft)
            for columns in (60, 120, 80, 60, 120):
                terminal.child.setwinsize(32, columns)
                screen.resize(lines=32, columns=columns)
                terminal.pump(.3)
                rows = screen.state['history'] + screen.display
                diagnostic = '\n'.join(rows)
                text = ''.join(''.join(rows).split())
                assert text.count('NATIVE_ULTRA_DRAFT') == 1, diagnostic
                assert text.count(''.join(draft.split())) == 1, diagnostic
                assert sum(row.startswith('» ') for row in rows) == 1, diagnostic
            terminal.send('\x01edited \x05!\r')
            body, release = terminal.request()
            user = [m['content'] for m in body['messages'] if m['role'] == 'user']
            assert user[-1] == 'edited ' + draft + '!', user
            release.set()
            print('PASS: native Ultra wrapping, reflow, cursor editing and exact submission', flush=True)
        finally:
            terminal.child.close(force=True)
            screen.close()


def no_color_startup(driver, server):
    with tempfile.TemporaryDirectory(prefix='morph-model-plain-') as temp:
        terminal = Terminal(driver.with_name('morph'), Path(temp), server.server_port,
                            hooks_enabled=False, production=True,
                            extra_arguments=('--no-color',),
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        try:
            terminal.wait_for_composer()
            wait_for(terminal, lambda: any(row.startswith('» ') for row in terminal.screen.display))
            terminal.send('/model normal\r')
            wait_for(terminal, lambda: any(row.startswith('> ') for row in terminal.screen.display))
            terminal.send('/model ultra\r')
            wait_for(terminal, lambda: any(row.startswith('» ') for row in terminal.screen.display))
            terminal.pump(1.1)
            assert '\x1b[48;5;98m' not in terminal.raw
            assert '\x1b[1;38;5;141m' not in terminal.raw
            print('PASS: saved Ultra startup and no-color prompt without animation', flush=True)
        finally:
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (200, 120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-model-pty-') as temp:
                directory = Path(temp)
                terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False,
                                    extra_config='extra_body_json = \'{"temperature":0.25}\'\n')
                try:
                    terminal.wait_for_composer()
                    terminal.child.setwinsize(32, columns)
                    terminal.screen = AlternateScreen(columns, 32)
                    terminal.stream = pyte.Stream(terminal.screen)
                    terminal.pump(.2)
                    marker = f'MODEL_HISTORY_SENTINEL_{columns}'
                    terminal.send('Keep model picker history visible\r')
                    _, seed = terminal.request()
                    seed.delta = {'content': marker + ' history/' * 8}
                    seed.set()
                    wait_for(terminal, lambda: marker in '\n'.join(terminal.screen.display)
                             and 'Working ·' not in '\n'.join(terminal.screen.display))
                    terminal.model_history_marker = marker
                    initial = (directory / 'config.toml').read_text()
                    start = len(terminal.raw)
                    terminal.send('/model\r')
                    panel(terminal)
                    for width in (60, 120, 80, columns):
                        terminal.child.setwinsize(32, width)
                        terminal.screen.resize(lines=32, columns=width)
                        terminal.pump(.2)
                        panel(terminal)
                        no_screen_switch(terminal.raw[start:])
                    terminal.send('\x1b')
                    settled(terminal)
                    assert (directory / 'config.toml').read_text() == initial
                    terminal.send('\x1b[A')
                    assert '/model' in terminal.screen.display[composer(terminal)]
                    terminal.send('\x15/m\r')
                    panel(terminal)
                    terminal.send('\x1b[200~2\r\x1b[201~')
                    assert (directory / 'config.toml').read_text() == initial
                    terminal.send('\x1b[B\r')
                    wait_for(terminal, lambda: 'model mode: ultra' in '\n'.join(terminal.screen.display))
                    assert 'Select Model Mode' not in '\n'.join(terminal.screen.display)
                    composer(terminal, True)
                    assert '\x1b[48;5;98m' in terminal.raw[start:], 'Ultra ignition did not paint'
                    # Type during the animation and verify that its frames never alter the draft.
                    draft = 'editable 中文 draft'
                    terminal.send(draft)
                    terminal.pump(1.0)
                    row = composer(terminal, True)
                    assert 'editable 中文' in terminal.screen.display[row]
                    for width in (60, 120, 80, columns):
                        terminal.child.setwinsize(32, width)
                        terminal.screen.resize(lines=32, columns=width)
                        terminal.pump(.2)
                        row = composer(terminal, True)
                        assert 'editable 中文' in terminal.screen.display[row]
                    terminal.send('\r')
                    body, release = terminal.request()
                    user = [m['content'] for m in body['messages'] if m['role'] == 'user']
                    assert user[-1] == draft, user
                    assert 'Proactive multi-agent delegation is active' in json.dumps(body), body
                    assert body['temperature'] == .25 and 'reasoning_effort' not in body, body
                    release.delta = {'content': f'ULTRA_VERIFIED_{columns}'}
                    release.set()
                    wait_for(terminal, lambda: f'ULTRA_VERIFIED_{columns}' in '\n'.join(terminal.screen.display)
                             and 'Working ·' not in '\n'.join(terminal.screen.display))
                    terminal.model_history_marker = f'ULTRA_VERIFIED_{columns}'
                    terminal.send('/model\r')
                    panel(terminal, True)
                    terminal.send('\x1b[A\r')
                    wait_for(terminal, lambda: 'model mode: normal' in '\n'.join(terminal.screen.display))
                    settled(terminal)
                    config = tomllib.loads((directory / 'config.toml').read_text())
                    assert config['agents']['multi_agent_mode'] == 'explicit', config
                    terminal.send('/model ultra\r')
                    settled(terminal, True)
                    saved = (directory / 'config.toml').read_text()
                    assert tomllib.loads(saved)['agents']['multi_agent_mode'] == 'proactive'
                    # A failed save must keep both the runtime selection and marker.
                    (directory / 'config.toml').unlink()
                    (directory / 'config.toml').mkdir()
                    terminal.send('/model normal\r')
                    wait_for(terminal, lambda: 'model mode change failed' in '\n'.join(terminal.screen.display))
                    settled(terminal, True)
                    (directory / 'config.toml').rmdir()
                    (directory / 'config.toml').write_text(saved)
                    for sequence in ('\x1b[?1049h', '\x1b[?1049l', '\x1bc'):
                        assert sequence not in terminal.raw[start:], repr(sequence)
                    assert Model.requests.empty(), 'mode selection requested an LLM'
                    # Structured mode uses receipts and keeps all animation escapes out of output.
                    binary = driver.with_name('morph')
                    result = subprocess.run([str(binary), '-c', str(directory / 'config.toml'),
                                             '-w', str(directory), '--events', 'json'],
                                            input='/model status\n/model normal\n/quit\n',
                                            env=terminal_environment(directory), text=True,
                                            capture_output=True, timeout=20)
                    assert result.returncode == 0, result.stderr
                    events = [json.loads(line) for line in result.stdout.splitlines()]
                    assert any('mode: ultra' in e.get('data', {}).get('output', '') for e in events), result.stdout
                    assert '\x1b' not in result.stdout
                    print(f'PASS: inline model, Ultra animation, draft and resize at {columns} columns', flush=True)
                finally:
                    terminal.child.close(force=True)
        native_reflow(driver, server)
        no_color_startup(driver, server)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
