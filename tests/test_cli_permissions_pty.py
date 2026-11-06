"""Permission presets, custom policy, cancellation and persistence in a real PTY."""
import http.server
import json
import subprocess
from pathlib import Path
import sys
import tempfile
import threading
try:
    import tomllib
except ModuleNotFoundError:
    print("Permission integration requires Python 3.11+ (tomllib).")
    sys.exit(77)

from test_cli_pty import Model, Terminal, terminal_environment
from test_cli_slash_pty import wait_for, composer, command_heading
from test_cli_resume_pty import AlternateScreen
import pyte


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-permissions-pty-') as temp:
                directory = Path(temp)
                terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
                try:
                    terminal.wait_for_composer()
                    terminal.child.setwinsize(30, columns)
                    terminal.screen = AlternateScreen(columns, 30)
                    terminal.stream = pyte.Stream(terminal.screen)
                    terminal.pump(.2)
                    terminal.send('/permissions\r')
                    wait_for(terminal, lambda: 'Update Model Permissions' in
                             '\n'.join(terminal.screen.display))
                    terminal.send('\x1b')
                    wait_for(terminal, lambda: any(row.startswith('› ')
                             for row in terminal.screen.display))
                    composer(terminal)
                    command_heading(terminal, '/permissions')
                    assert '[permissions]' not in (directory / 'config.toml').read_text()

                    terminal.send('/permissions\r')
                    wait_for(terminal, lambda: 'Update Model Permissions' in
                             '\n'.join(terminal.screen.display))
                    terminal.send('\x1b[B\x1b[B\x1b[B\r')
                    wait_for(terminal, lambda: 'permissions: read_only' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    config = tomllib.loads((directory / 'config.toml').read_text())
                    assert config['permissions'] == {
                        'access': 'read_only', 'approval': 'on_request', 'reviewer': 'user'}
                    assert config['model']['text']['model'] == 'pty-test'

                    terminal.send('/permissions set read_only never user\r')
                    wait_for(terminal, lambda: 'permissions: custom' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    terminal.send('/permissions status\r')
                    wait_for(terminal, lambda: 'approval=never' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    terminal.send('/permissions mode ask\r')
                    wait_for(terminal, lambda: 'permissions: ask' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    assert tomllib.loads((directory / 'config.toml').read_text())['permissions']['access'] == 'workspace'
                    print(f'PASS: permissions selection, cancel, Custom, persistence and composer at {columns} columns')
                    binary = driver.with_name('morph')
                    structured = subprocess.run(
                        [str(binary), '-c', str(directory / 'config.toml'),
                         '-w', str(directory), '--events', 'json'],
                        input='/permissions mode auto\n/permissions status\n/quit\n',
                        env=terminal_environment(directory), text=True,
                        capture_output=True, timeout=20)
                    assert structured.returncode == 0, structured.stderr
                    events = [json.loads(line) for line in structured.stdout.splitlines()]
                    completed = [event for event in events if event.get('name') == 'command.completed']
                    assert any('reviewer=model' in event.get('data', {}).get('output', '')
                               for event in completed), structured.stdout
                    assert tomllib.loads((directory / 'config.toml').read_text())['permissions']['reviewer'] == 'model'
                    print(f'PASS: structured policy selection and status at {columns} columns')
                finally:
                    terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
