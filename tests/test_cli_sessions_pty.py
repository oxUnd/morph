"""Independent runtime sessions in one production CLI and retained client views."""

import json
import queue
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal
from test_cli_subagents_pty import FeedModel
from test_cli_agents_pty import wait_for


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def scenario(binary, port, columns):
    with tempfile.TemporaryDirectory(prefix='morph-sessions-pty-') as temp:
        directory = Path(temp)
        terminal = Terminal(binary, directory, port, production=True, hooks_enabled=False)
        releases = []
        try:
            terminal.child.setwinsize(32, columns)
            terminal.screen.resize(lines=32, columns=columns)
            terminal.wait_for_composer()
            terminal.send('/new Alpha\r')
            wait_for(terminal, lambda: 'created and switched to session: Alpha' in terminal.raw)
            terminal.send('WORK_ALPHA\r')
            body, alpha = terminal.request()
            releases.append(alpha)
            assert any(message.get('content') == 'WORK_ALPHA'
                       for message in body['messages']), body
            alpha.stream({'content': 'ALPHA_STREAM\n\n'})
            wait_for(terminal, lambda: 'ALPHA_STREAM' in visible(terminal))

            # The first model request stays open while the second session starts.
            terminal.send('/new Beta\r')
            wait_for(terminal, lambda: 'created and switched to session: Beta' in visible(terminal))
            assert 'wait for the active turn' not in visible(terminal), visible(terminal)
            terminal.send('WORK_BETA\r')
            body, beta = terminal.request()
            releases.append(beta)
            assert not alpha.is_set()
            assert any(message.get('content') == 'WORK_BETA'
                       for message in body['messages']), body
            assert not any(message.get('content') == 'WORK_ALPHA'
                           for message in body['messages']), body
            beta.stream({'content': 'BETA_STREAM\n\n'})
            wait_for(terminal, lambda: 'BETA_STREAM' in visible(terminal))

            terminal.send('DRAFT_BETA')
            terminal.send('\x1b[5;5~')
            wait_for(terminal, lambda: 'ALPHA_STREAM' in visible(terminal))
            assert 'DRAFT_BETA' not in visible(terminal), visible(terminal)
            terminal.send('DRAFT_ALPHA')
            terminal.send('\x1b[6;5~')
            wait_for(terminal, lambda: 'DRAFT_BETA' in visible(terminal))
            assert 'BETA_STREAM' in visible(terminal), visible(terminal)
            assert 'DRAFT_ALPHA' not in visible(terminal), visible(terminal)

            terminal.send('\x1b[5;5~')
            wait_for(terminal, lambda: 'DRAFT_ALPHA' in visible(terminal))
            beta.delta = {'content': 'BETA_FINAL'}
            beta.set()
            terminal.pump(.8)
            assert 'BETA_FINAL' not in visible(terminal), visible(terminal)
            assert 'ALPHA_STREAM' in visible(terminal), visible(terminal)

            # Cancel the viewed session; Beta's completed result remains intact.
            terminal.send('\x03')
            alpha.set()
            terminal.pump(.8)
            terminal.send('/switch Beta\r')
            wait_for(terminal, lambda: 'BETA_FINAL' in visible(terminal))
            assert 'ALPHA_STREAM' not in visible(terminal), visible(terminal)
            assert 'DRAFT_BETA' in visible(terminal), visible(terminal)
            terminal.send('\x15/sessions\r')
            wait_for(terminal, lambda: 'Current' in visible(terminal) and
                     'Alpha' in visible(terminal) and 'Beta' in visible(terminal))

            database = directory / 'home/.morph/data.db'
            with sqlite3.connect(database) as db:
                for name, prompt in [('Alpha', 'WORK_ALPHA'), ('Beta', 'WORK_BETA')]:
                    inputs = db.execute(
                        "SELECT m.content FROM messages m JOIN sessions s ON s.id=m.session_id "
                        "WHERE s.name=? AND m.role='user'", (name,)).fetchall()
                    assert inputs == [(prompt,)], (name, inputs)
                answers = db.execute(
                    "SELECT m.content FROM messages m JOIN sessions s ON s.id=m.session_id "
                    "WHERE s.name='Beta' AND m.role='assistant'").fetchall()
                assert any('BETA_FINAL' in value for value, in answers), answers
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            if columns == 80:
                terminal.send('/trace\r')
                wait_for(terminal, lambda: 'ReAct trace (Beta)' in visible(terminal))
                terminal.send('/context\r')
                wait_for(terminal, lambda: 'context by kind:' in visible(terminal))
                terminal.send('/history --diagnose\r')
                wait_for(terminal, lambda: 'history: active=' in visible(terminal))
                terminal.send('/compress\r')
                body, compression = terminal.request()
                releases.append(compression)
                assert 'checkpoint' in json.dumps(body['messages']).lower(), body
                assert 'WORK_ALPHA' not in json.dumps(body['messages']), body
                terminal.send('COMPRESS_DRAFT')
                assert 'COMPRESS_DRAFT' in visible(terminal), visible(terminal)
                compression.delta = {'content': json.dumps({
                    'objective': 'Complete Beta', 'constraints': 'Keep sessions isolated',
                    'completed_work': 'BETA_FINAL', 'working_state': 'Idle',
                    'pending_work': 'None recorded', 'verification': 'Beta completed'})}
                compression.set()
                wait_for(terminal, lambda: 'model history compacted atomically:' in
                         visible(terminal))
                assert 'COMPRESS_DRAFT' in visible(terminal), visible(terminal)
                terminal.send('\x15/history --repair\r')
                wait_for(terminal, lambda: 'repaired=' in visible(terminal))
                with sqlite3.connect(database) as db:
                    checkpoint = db.execute(
                        'SELECT c.id FROM code_checkpoints c JOIN sessions s '
                        "ON s.id=c.session_id WHERE s.name='Beta' ORDER BY c.id LIMIT 1"
                    ).fetchone()[0]
                terminal.send(f'/rewind conversation {checkpoint}\r')
                wait_for(terminal, lambda: 'rewound conversation to checkpoint' in
                         visible(terminal))
                terminal.send('/switch Alpha\r')
                terminal.send('\x15ASK_ALPHA\r')
                _, question = terminal.request()
                releases.append(question)
                terminal.send('/switch Beta\r')
                terminal.send('\x15WORK_BETA_SECOND\r')
                body, beta_second = terminal.request()
                releases.append(beta_second)
                assert not any(message.get('content') == 'ASK_ALPHA'
                               for message in body['messages']), body
                assert not any(message.get('content') == 'WORK_BETA'
                               for message in body['messages']), body
                question.delta = {'tool_calls': [{
                    'index': 0, 'id': 'alpha-question', 'type': 'function',
                    'function': {'name': 'ask_user', 'arguments': json.dumps({
                        'question': 'ALPHA_BACKGROUND_QUESTION', 'choices': ['Proceed']})}}]}
                question.set()
                terminal.pump(.5)
                assert 'ALPHA_BACKGROUND_QUESTION' not in visible(terminal), visible(terminal)
                terminal.send('/sessions\r')
                wait_for(terminal, lambda: 'Waiting for input' in visible(terminal))
                terminal.send('/switch Alpha\r')
                wait_for(terminal, lambda: '? ALPHA_BACKGROUND_QUESTION' in visible(terminal))
                terminal.send('\r')
                body, answered = terminal.request()
                releases.append(answered)
                responses = [json.loads(message['content']) for message in body['messages']
                             if message.get('role') == 'tool' and
                             'ask_user_response' in message.get('content', '')]
                assert responses[-1].get('data', responses[-1])['answers'] == ['Proceed']
                assert not beta_second.is_set()
                answered.delta = {'content': 'ALPHA_QUESTION_ANSWERED'}
                answered.set()
                wait_for(terminal, lambda: 'ALPHA_QUESTION_ANSWERED' in visible(terminal))
                terminal.send('/switch Beta\r')
                beta_second.delta = {'content': 'BETA_SECOND_COMPLETE'}
                beta_second.set()
                wait_for(terminal, lambda: 'BETA_SECOND_COMPLETE' in visible(terminal))
                assert 'ALPHA_QUESTION_ANSWERED' not in visible(terminal), visible(terminal)

            terminal.send('\x04')
            terminal.child.expect(__import__('pexpect').EOF, timeout=15)
            terminal.child.close()
            assert terminal.child.exitstatus == 0
            print(f'PASS: independent sessions and retained views at {columns} columns',
                  flush=True)
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), FeedModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            while True:
                try:
                    Model.requests.get_nowait()
                except queue.Empty:
                    break
            scenario(binary, server.server_port, columns)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
