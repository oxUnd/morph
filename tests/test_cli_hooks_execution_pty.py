"""Hook progress, warning output, and cancellation through the production TUI."""
import http.server
from loopback_http import LoopbackHTTPServer
import json
from pathlib import Path
import shlex
import sqlite3
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until
from test_cli_transcript_pty import AlternateScreen, PacedMarkdownModel
import pyte


def check_stop_continuation(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    chunks = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-stop-continuation-') as temp:
            directory = Path(temp)
            script = directory / 'stop.py'
            script.write_text('''import json
import sys
from pathlib import Path
payload = json.load(sys.stdin)
log = Path('stop-inputs.jsonl')
previous = log.read_text().splitlines() if log.exists() else []
with log.open('a') as output:
    output.write(json.dumps(payload) + '\\n')
print(json.dumps({'decision': 'block', 'reason': 'HOOK_CONTINUE_SENTINEL'}
                 if not previous else {'continue': False}))
''')
            hooks = {'hooks': {'Stop': [{'hooks': [{
                'type': 'command', 'command': 'python3 ' + shlex.quote(str(script))}]}]}}
            (directory / 'hooks.json').write_text(json.dumps(hooks))
            terminal = Terminal(driver, directory, server.server_port)
            terminal.child.setwinsize(40, 120)
            terminal.screen = AlternateScreen(120, 40)
            terminal.stream = pyte.Stream(terminal.screen)
            until(terminal, lambda text: 'Trust lifecycle hooks?' in text)
            terminal.send('\x1b[A\x1b[A\r')
            terminal.wait_for_composer()
            terminal.send('Check Stop continuation\r')
            _, chunks = terminal.request()
            chunks.put('FIRST_HOOK_ANSWER')
            until(terminal, lambda text: 'FIRST_HOOK_ANSWER' in text)
            assert not (directory / 'stop-inputs.jsonl').exists()
            terminal.send('keep my draft')
            chunks.put(None)
            body, chunks = terminal.request()
            messages = body['messages']
            assert any(message.get('role') == 'assistant' and
                       message.get('content') == 'FIRST_HOOK_ANSWER'
                       for message in messages), messages
            assert any(message.get('role') == 'user' and
                       message.get('content', '').startswith('<hook_prompt hook_run_id="') and
                       'HOOK_CONTINUE_SENTINEL</hook_prompt>' in message.get('content', '')
                       for message in messages), messages
            chunks.put('SECOND_HOOK_ANSWER')
            until(terminal, lambda text: 'SECOND_HOOK_ANSWER' in text)
            chunks.put(None)
            text = until(terminal, lambda text: 'Worked for' in text)
            assert text.count('FIRST_HOOK_ANSWER') == 1, text
            assert text.count('SECOND_HOOK_ANSWER') == 1, text
            assert 'Stop hook requested continuation' in text, text
            assert 'keep my draft' in text, text
            assert 'Error:' not in text, text
            inputs = [json.loads(line) for line in
                      (directory / 'stop-inputs.jsonl').read_text().splitlines()]
            assert [item['stop_hook_active'] for item in inputs] == [False, True], inputs
            assert inputs[0]['turn_id'] == inputs[1]['turn_id'], inputs
            with sqlite3.connect(directory / 'data.db') as db:
                saved = db.execute('SELECT role,content,payload_json FROM model_history_items '
                                   'ORDER BY sequence_no').fetchall()
            assert [row[0] for row in saved] == ['user', 'assistant', 'user', 'assistant'], saved
            assert saved[1][1] == 'FIRST_HOOK_ANSWER', saved
            assert json.loads(saved[2][2])['source'] == 'hook', saved
            assert saved[3][1] == 'SECOND_HOOK_ANSWER', saved
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            print('PASS: live Stop output, durable continuation, normal continue:false completion')
    finally:
        if chunks is not None:
            chunks.put(None)
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


def main():
    driver = Path(sys.argv[1]).resolve()
    check_stop_continuation(driver)
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    releases = []
    try:
        with tempfile.TemporaryDirectory(prefix='morph-hook-execution-') as temp:
            directory = Path(temp)
            hooks = {'hooks': {
                'UserPromptSubmit': [{'hooks': [{
                    'type': 'command', 'statusMessage': 'Preparing hook context',
                    'command': "sleep 1; printf '%s' "
                               "'{\"systemMessage\":\"Hook warning is visible\"}'"}]}],
                'Interrupt': [{'hooks': [{
                    'type': 'command',
                    'command': "touch interrupted; printf '%s' "
                               "'{\"systemMessage\":\"Interruption saved\"}'"}]}]}}
            (directory / 'hooks.json').write_text(json.dumps(hooks))
            terminal = Terminal(driver, directory, server.server_port)
            terminal.screen = AlternateScreen(80, 24)
            terminal.stream = pyte.Stream(terminal.screen)
            text = until(terminal, lambda text: 'Trust lifecycle hooks?' in text)
            assert 'Trust lifecycle hooks?' in text, text
            terminal.send('\x1b[A\x1b[A\r')
            terminal.wait_for_composer()
            terminal.send('run hooks\r')
            until(terminal, lambda text: 'Preparing hook context' in text)
            _, release = terminal.request()
            releases.append(release)
            assert 'Hook warning is visible' in terminal.raw, terminal.raw
            release.delta = {'content': 'Finished after hooks.'}
            release.set()
            until(terminal, lambda text: 'Finished after hooks.' in text)
            terminal.wait_for_composer()
            terminal.send('interrupt this turn\r')
            _, release = terminal.request()
            releases.append(release)
            terminal.child.send('\x1b')
            until(terminal, lambda text: 'Stopped after' in text)
            assert (directory / 'interrupted').exists()
            assert 'Interruption saved' in terminal.raw, terminal.raw
            release.set()
            terminal.send('editable after hooks')
            text = terminal.snapshot('composer restored after interrupt hook')
            assert 'editable after hooks' in text, text
            assert 'Preparing hook context' not in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            print('PASS: delayed hook progress, durable warnings, interruption cleanup, composer recovery')
    finally:
        for release in releases:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
