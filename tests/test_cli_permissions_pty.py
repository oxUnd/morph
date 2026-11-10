"""Permission presets, custom policy, cancellation and persistence in a real PTY."""
import http.server
import json
import sqlite3
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
from cli_inline_test_support import inline_panel
from test_cli_slash_pty import wait_for, editor_input, command_heading
from test_cli_resume_pty import AlternateScreen
import pyte


def composer(terminal, expected=''):
    rows = terminal.screen.display
    row = editor_input(terminal, expected)
    boundary = max((i for i, line in enumerate(rows)
                    if line.startswith('/') or 'Worked for ' in line), default=-1)
    assert sum(line.startswith('› ') for line in rows[boundary + 1:]) == 1, '\n'.join(rows)
    backgrounds = [i for i in range(boundary + 1, terminal.screen.lines)
                   if terminal.screen.buffer[i][0].bg == '1c1c1c']
    assert backgrounds == [row - 1, row, row + 1], (backgrounds, '\n'.join(rows))
    return row


def permission_footer(terminal, expected):
    row = max(i for i, line in enumerate(terminal.screen.display)
              if line.startswith('› '))
    text = '\n'.join(terminal.screen.display)
    footer = terminal.screen.display[row + 2]
    assert expected in footer, text
    assert sum('perm: ' in line for line in terminal.screen.display) == 1, text
    assert 'perm: ?' not in footer, text
    return footer


def result_gap(terminal, result):
    rows = terminal.screen.display
    row = composer(terminal)
    output = max(i for i, text in enumerate(rows) if result in text)
    assert row == output + 3, '\n'.join(rows)
    assert not rows[output + 1].strip(), '\n'.join(rows)
    if not rows[output].startswith('/'):
        heading = max(i for i in range(output) if rows[i].startswith('/'))
        assert output == heading + 2, '\n'.join(rows)
        assert not rows[heading + 1].strip(), '\n'.join(rows)


def panel(terminal, marker):
    rows = terminal.screen.display
    text = '\n'.join(rows)
    title = next(i for i, row in enumerate(rows) if 'Update Model Permissions' in row)
    message = next(i for i, row in enumerate(rows) if marker in row)
    command = command_heading(terminal, '/permissions')
    assert message < command < title, text
    assert title == command + 2, text
    assert not rows[command + 1].strip(), text
    assert text.count('Update Model Permissions') == 1, text
    assert not any(row.startswith('› ') for row in rows[command + 1:]), text
    selected = [i for i, row in enumerate(rows) if row.startswith('> ')]
    assert len(selected) == 1, text
    assert terminal.screen.buffer[selected[0]][0].bg == 'blue', text
    inline_panel(terminal, 'Update Model Permissions', marker, maximum=10)
    return selected[0]


def no_screen_switch(raw):
    for sequence in ('\x1b[?1049h', '\x1b[?1049l', '\x1b[2J', '\x1b[H'):
        assert sequence not in raw, repr(sequence) + ' was used by the inline picker'


def closed(terminal, marker):
    wait_for(terminal, lambda: 'Update Model Permissions' not in
             '\n'.join(terminal.screen.display))
    composer(terminal)
    text = '\n'.join(terminal.screen.display)
    assert marker in text, text
    for label in ('Ask for approval', 'Approve for me', 'Full Access', 'Read Only',
                  'Up/Down Select  Enter Apply'):
        assert label not in text, text


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
                    permission_footer(terminal, "perm: ask")
                    marker = f'VISIBLE_CONVERSATION_{columns}'
                    terminal.send('Keep this conversation visible\r')
                    _, release = terminal.request()
                    release.delta = {'content': marker}
                    release.set()
                    wait_for(terminal, lambda: marker in '\n'.join(terminal.screen.display)
                             and 'Working ·' not in '\n'.join(terminal.screen.display))
                    composer(terminal)
                    start = len(terminal.raw)
                    terminal.send('/permissions\r')
                    wait_for(terminal, lambda: 'Update Model Permissions' in
                             '\n'.join(terminal.screen.display))
                    selected = panel(terminal, marker)
                    assert 'Ask for approval (current)' in terminal.screen.display[selected]
                    terminal.send('\x1b[B')
                    assert 'Approve for me' in terminal.screen.display[panel(terminal, marker)]
                    # Width changes preserve the conversation and selected row in place.
                    for resized in (60, 120, 80, columns):
                        terminal.child.setwinsize(30, resized)
                        terminal.screen.resize(lines=30, columns=resized)
                        terminal.pump(.2)
                        assert 'Approve for me' in terminal.screen.display[panel(terminal, marker)]
                    terminal.send('\x1b')
                    closed(terminal, marker)
                    permission_footer(terminal, 'perm: ask')
                    result_gap(terminal, '/permissions')
                    no_screen_switch(terminal.raw[start:])
                    terminal.send('\x1b[A')
                    composer(terminal, '/permissions')
                    terminal.send('\x15')
                    assert '[permissions]' not in (directory / 'config.toml').read_text()

                    terminal.send('/permissions\r')
                    wait_for(terminal, lambda: 'Update Model Permissions' in
                             '\n'.join(terminal.screen.display))
                    panel(terminal, marker)
                    before = (directory / 'config.toml').read_text()
                    terminal.send('\x1b[200~3\r\x1b[201~')
                    assert (directory / 'config.toml').read_text() == before
                    panel(terminal, marker)
                    terminal.send('\x1b[B\x1b[B\x1b[B\r')
                    wait_for(terminal, lambda: 'permissions: read_only' in
                             '\n'.join(terminal.screen.display))
                    closed(terminal, marker)
                    permission_footer(terminal, 'perm: read_only')
                    result_gap(terminal, 'permissions: read_only')
                    config = tomllib.loads((directory / 'config.toml').read_text())
                    assert config['permissions'] == {
                        'access': 'read_only', 'approval': 'on_request', 'reviewer': 'user'}
                    assert config['model']['text']['model'] == 'pty-test'

                    terminal.send('/permissions set read_only never user\r')
                    wait_for(terminal, lambda: 'permissions: custom' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    permission_footer(terminal, 'perm: custom read_only/never/user')
                    terminal.send('/permissions\r')
                    wait_for(terminal, lambda: 'Custom: read_only / never / user' in
                             '\n'.join(terminal.screen.display))
                    assert '(current)' not in '\n'.join(terminal.screen.display)
                    terminal.send('\x03')
                    closed(terminal, "permissions: custom")
                    permission_footer(terminal, "perm: custom read_only/never/user")
                    result_gap(terminal, '/permissions')
                    terminal.send('/permissions status\r')
                    wait_for(terminal, lambda: 'approval=never' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    terminal.send('/permissions mode ask\r')
                    wait_for(terminal, lambda: 'permissions: ask' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    assert tomllib.loads((directory / 'config.toml').read_text())['permissions']['access'] == 'workspace'
                    permission_footer(terminal, 'perm: ask')
                    for mode in ('ask', 'auto', 'read_only', 'full'):
                        terminal.send(f'/permissions mode {mode}\r')
                        wait_for(terminal, lambda: f'permissions: {mode}' in
                                 '\n'.join(terminal.screen.display))
                        permission_footer(terminal, f'perm: {mode}')
                        for narrow in (40, 20, columns):
                            terminal.child.setwinsize(30, narrow)
                            terminal.screen.resize(lines=30, columns=narrow)
                            terminal.pump(.2)
                            permission_footer(terminal, f'perm: {mode}')
                    # The runtime getter is busy during a turn; retain its last policy snapshot.
                    terminal.send('Verify permissions stay visible while working\r')
                    _, release = terminal.request()
                    draft = 'preserved permission draft 中文'
                    terminal.send(draft)
                    terminal.pump(1.1)
                    permission_footer(terminal, 'perm: full')
                    editor_input(terminal, draft)
                    release.delta = {'content': 'PERMISSION_FOOTER_VERIFIED'}
                    release.set()
                    wait_for(terminal, lambda: 'PERMISSION_FOOTER_VERIFIED' in
                             '\n'.join(terminal.screen.display) and 'Working ·' not in
                             '\n'.join(terminal.screen.display))
                    permission_footer(terminal, 'perm: full')
                    editor_input(terminal, draft)
                    terminal.send('\x15/permissions mode ask\r')
                    wait_for(terminal, lambda: 'perm: ask' in
                             '\n'.join(terminal.screen.display))
                    permission_footer(terminal, 'perm: ask')
                    no_screen_switch(terminal.raw[start:])
                    # Primary turns automatically seed checkpoints; no Git or test-only
                    # checkpoint producer is needed for the real rewind command flow.
                    terminal.send('/rewind list\r')
                    wait_for(terminal, lambda: 'rewind checkpoints' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    command_heading(terminal, '/rewind list')
                    assert not any(row.startswith('› /rewind')
                                   for row in terminal.screen.display)
                    with sqlite3.connect(directory / 'data.db') as checkpoint_db:
                        checkpoint_id, checkpoint_prompt = checkpoint_db.execute(
                            'SELECT id,prompt FROM code_checkpoints ORDER BY id DESC LIMIT 1'
                        ).fetchone()
                    terminal.send(f'/rewind code {checkpoint_id}\r')
                    wait_for(terminal, lambda: 'rewound code to checkpoint' in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    receipts = terminal.raw.count('rewound code to checkpoint')
                    terminal.send('/rewind\r')
                    wait_for(terminal, lambda: 'Rewind · choose a turn' in
                             '\n'.join(terminal.screen.display))
                    terminal.send('\r')
                    wait_for(terminal, lambda: 'Rewind · choose what to restore' in
                             '\n'.join(terminal.screen.display))
                    preview = '\n'.join(terminal.screen.display)
                    assert checkpoint_prompt in preview, preview
                    assert 'Code and conversation' in preview, preview
                    assert 'Code only' in preview, preview
                    assert 'Code only (unavailable)' not in preview, preview
                    assert 'Conversation only' in preview, preview
                    assert not any(row.startswith('› ') for row in terminal.screen.display), preview
                    terminal.send('\x1b')
                    wait_for(terminal, lambda: 'Rewind ·' not in
                             '\n'.join(terminal.screen.display))
                    composer(terminal)
                    command_heading(terminal, '/rewind')
                    result_gap(terminal, '/rewind')
                    assert terminal.raw.count('rewound code to checkpoint') == receipts
                    assert 'rewind cancelled' not in terminal.raw
                    assert Model.requests.empty(), 'rewind unexpectedly requested a model response'
                    print(f'PASS: inline permissions and live footer in every mode at {columns} columns')
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
