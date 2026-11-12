"""Restore persisted scrollback through slash commands and the production startup picker."""
import json
import re
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import threading
import http.server

from test_cli_pty import Model, Terminal, terminal_environment
from test_cli_transcript_pty import AlternateScreen
import pexpect
import pyte


def start(binary, directory, port, columns, *arguments):
    terminal = Terminal(binary, directory, port, production=True,
                        hooks_enabled=False, extra_arguments=arguments)
    terminal.child.setwinsize(28, columns)
    terminal.screen = AlternateScreen(columns, 28)
    terminal.stream = pyte.Stream(terminal.screen)
    return terminal


def stop(terminal):
    terminal.send('/quit\r')
    terminal.child.expect(pexpect.EOF, timeout=10)
    terminal.child.close()
    assert terminal.child.exitstatus == 0, terminal.raw[-4000:]


def seed(database):
    with sqlite3.connect(database) as db:
        for identity, name, timestamp in (('1234abcd', 'HistoryNative', 200),
                                          ('8765fedc', 'HistoryLegacy', 100)):
            db.execute('INSERT INTO sessions(display_id,name,model,created_at,updated_at) '
                       'VALUES(?,?,?,?,?)', (identity, name, 'pty-test', timestamp, timestamp))
        native, legacy = [row[0] for row in db.execute('SELECT id FROM sessions ORDER BY id')]
        for session, role, content, turn in (
                (native, 'user', 'HISTORY_FIRST_QUESTION', 'old-turn'),
                (native, 'assistant', '**HISTORY_FIRST_ANSWER**', 'old-turn'),
                (native, 'user', 'HISTORY_LAST_QUESTION', 'recent-turn'),
                (native, 'assistant', '**HISTORY_LAST_ANSWER**', 'recent-turn'),
                (legacy, 'user', 'LEGACY_QUESTION', None),
                (legacy, 'assistant', '**LEGACY_ANSWER**', None)):
            db.execute('INSERT INTO messages(session_id,role,content,turn_id,token_count,'
                       'created_at) VALUES(?,?,?,?,?,?)', (session, role, content, turn, 4, 100))
        calls = {'calls': [{'name': 'exec', 'tool_call_id': 'history-call',
                           'arguments': json.dumps({'command':
                               'touch resumed-tool-must-not-run'})},
                          {'name': 'spawn_agent', 'tool_call_id': 'history-spawn',
                           'arguments': json.dumps({'task_name': 'history-worker',
                               'message': 'HISTORY_DELEGATION_PROMPT',
                               'assignment': {'scope': 'HISTORY_ASSIGNMENT_SCOPE'}})},
                          {'name': 'followup_task', 'tool_call_id': 'history-followup',
                           'arguments': json.dumps({'target': '/root/history-worker',
                               'message': 'HISTORY_FOLLOWUP_PROMPT'})}],
                 'reasoning_content': 'RECORDED_REASONING'}
        result = {'ok': True, 'data': {'stdout': 'HISTORY_TOOL_OUTPUT',
                                      'exit_code': 0, 'duration_ms': 2500}}
        records = (
            ('user_message', 'user', 'HISTORY_FIRST_QUESTION', None, None, 0),
            ('assistant_tool_calls', 'assistant', 'HISTORY_CHECKING', calls, None, 0),
            ('tool_result', 'tool', json.dumps(result), None, 'history-call', 0),
            ('tool_result', 'tool', json.dumps({'data': {'agent_id': '/root/history-worker'}}),
             None, 'history-spawn', 0),
            ('tool_result', 'tool', '{}', None, 'history-followup', 0),
            ('assistant_message', 'assistant', '**HISTORY_FIRST_ANSWER**', None, None, 0),
            ('compaction_summary', 'system', 'INTERNAL_COMPACTION', None, None, 1),
            ('user_message', 'user', 'HISTORY_LAST_QUESTION', None, None, 1),
            ('assistant_message', 'assistant', '**HISTORY_LAST_ANSWER**', None, None, 1))
        for sequence, (kind, role, content, payload, tool_id, active) in enumerate(records, 1):
            db.execute('INSERT INTO model_history_items(session_id,sequence_no,turn_id,kind,'
                       'role,content,payload_json,tool_call_id,tool_name,active,created_at) '
                       'VALUES(?,?,?,?,?,?,?,?,?,?,?)',
                       (native, sequence, 'old-turn' if not active else 'recent-turn',
                        kind, role, content, json.dumps(payload) if payload else None,
                        tool_id, {'history-call': 'exec', 'history-spawn': 'spawn_agent',
                                  'history-followup': 'followup_task'}.get(tool_id),
                        active, sequence))
    return native, legacy


def check_restored(terminal, label):
    terminal.wait_for_composer()
    screen = terminal.snapshot(label)
    for marker in ('HISTORY_FIRST_QUESTION', 'HISTORY_CHECKING', 'HISTORY_TOOL_OUTPUT',
                   'HISTORY_FIRST_ANSWER', 'HISTORY_LAST_QUESTION', 'HISTORY_LAST_ANSWER'):
        assert terminal.raw.count(marker) == 1, (marker, terminal.raw[-7000:])
    assert 'HISTORY_LAST_ANSWER' in screen, screen
    assert 'INTERNAL_COMPACTION' not in terminal.raw, terminal.raw[-7000:]
    plain = re.sub(r'\x1b\[[0-?]*[ -/]*[@-~]', '', terminal.raw)
    assert 'Spawned /root/history-worker' in plain, plain[-7000:]
    for private in ('HISTORY_DELEGATION_PROMPT', 'HISTORY_ASSIGNMENT_SCOPE',
                    'HISTORY_FOLLOWUP_PROMPT'):
        assert private not in terminal.raw, terminal.raw[-7000:]
    assert '2.5s' in terminal.raw and 'Worked for' not in terminal.raw, terminal.raw[-7000:]
    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
    assert Model.requests.empty(), 'resume unexpectedly called the model'


def main():
    binary = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-resume-history-') as temp:
                directory = Path(temp)
                terminal = start(binary, directory, server.server_port, columns, 'resume')
                try:
                    terminal.wait_for_composer()
                    assert 'no sessions to resume' in terminal.raw, terminal.raw
                    stop(terminal)
                    database = directory / 'home' / '.morph' / 'data.db'
                    with sqlite3.connect(database) as db:
                        assert db.execute('SELECT count(*) FROM sessions').fetchone()[0] == 0
                    native, legacy = seed(database)
                    terminal = start(binary, directory, server.server_port, columns, 'resume')
                    terminal.pump(1)
                    screen = terminal.snapshot(f'{columns}: startup resume list')
                    assert 'Resume a session' in screen and 'HistoryNative' in screen, screen
                    assert 'Ctrl+T transcript' not in terminal.raw, terminal.raw
                    terminal.send('\r')
                    check_restored(terminal, f'{columns}: selected session restores scrollback')
                    assert not (directory / 'resumed-tool-must-not-run').exists()
                    with sqlite3.connect(database) as db:
                        assert db.execute('SELECT count(*) FROM messages').fetchone()[0] == 6
                    terminal.send('CONTINUE_RESUMED_SESSION\r')
                    body, release = terminal.request()
                    assert 'HISTORY_LAST_ANSWER' in json.dumps(body), body
                    release.delta = {'content': 'RESUMED_CONTINUATION_OK'}
                    release.set()
                    terminal.pump(1)
                    assert 'RESUMED_CONTINUATION_OK' in terminal.raw, terminal.raw[-5000:]
                    with sqlite3.connect(database) as db:
                        row = db.execute("SELECT session_id FROM messages WHERE role='user' "
                                         "AND content='CONTINUE_RESUMED_SESSION'").fetchone()
                        assert row == (native,), row
                    terminal.send('/resume 8765fedc\r')
                    screen = terminal.snapshot(f'{columns}: slash resume restores legacy messages')
                    assert 'LEGACY_ANSWER' in screen and 'LEGACY_QUESTION' in screen, screen
                    assert '› /resume' not in screen, screen
                    stop(terminal)
                    terminal = start(binary, directory, server.server_port, columns, 'resume')
                    terminal.pump(1)
                    terminal.send('\x1b')
                    terminal.wait_for_composer()
                    screen = terminal.snapshot(f'{columns}: cancel startup resume list')
                    assert 'HISTORY_FIRST_QUESTION' not in terminal.raw, terminal.raw[-5000:]
                    assert 'Resume a session' not in screen, screen
                    stop(terminal)
                    for arguments in (('resume', '1234abcd'), ('resume', str(native)),
                                      ('resume', 'HistoryNative')):
                        terminal = start(binary, directory, server.server_port, columns,
                                         *arguments)
                        terminal.wait_for_composer()
                        assert 'HISTORY_FIRST_QUESTION' in terminal.raw, terminal.raw[-5000:]
                        assert 'RESUMED_CONTINUATION_OK' in terminal.raw, terminal.raw[-5000:]
                        stop(terminal)
                    terminal = start(binary, directory, server.server_port, columns,
                                     '-s', 'HistoryLegacy')
                    terminal.wait_for_composer()
                    assert 'LEGACY_ANSWER' in terminal.raw, terminal.raw[-5000:]
                    stop(terminal)
                    result = subprocess.run(
                        [str(binary), 'resume', '--config', str(directory / 'config.toml'),
                         '--workdir', str(directory), 'missing-session'],
                        env=terminal_environment(directory), text=True, capture_output=True,
                        timeout=15)
                    assert result.returncode == 1, result
                    assert 'cannot resume session' in result.stderr, result.stderr
                    result = subprocess.run(
                        [str(binary), '-c', str(directory / 'config.toml'), '--events', 'json'],
                        input=f'/resume {legacy}\n/quit\n', env=terminal_environment(directory),
                        text=True, capture_output=True, timeout=15)
                    assert result.returncode == 0, result.stderr
                    events = [json.loads(line) for line in result.stdout.splitlines()]
                    assert events and 'HISTORY_LAST_ANSWER' not in result.stdout, result.stdout
                    assert 'LEGACY_ANSWER' not in result.stdout, result.stdout
                    print(f'checked: {columns}: startup references, continuation and JSON contract',
                          flush=True)
                finally:
                    terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()
    return 0


if __name__ == '__main__':
    sys.exit(main())
