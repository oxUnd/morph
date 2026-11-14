"""Approval cards and selectable decisions through the real CLI and local model."""
from loopback_http import LoopbackHTTPServer
import http.server
import json
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until
from test_cli_slash_pty import editor_input


def closed_card(terminal):
    rows = [row.rstrip() for row in terminal.screen.display]
    first = max(i for i, row in enumerate(rows) if row.startswith('╭'))
    last = next(i for i in range(first + 1, len(rows)) if rows[i].startswith('╰'))
    width = min(80, terminal.screen.columns - 2)
    assert rows[first].endswith('╮'), rows[first]
    assert rows[last].endswith('╯'), rows[last]
    for row in range(first, last + 1):
        assert terminal.screen.buffer[row][width - 1].data in ('╮', '│', '╯'), rows[row]
        assert not rows[row][width:].strip(), rows[row]
        if first < row < last:
            assert rows[row].startswith('│ ') and rows[row].endswith(' │'), rows[row]
    assert not rows[last + 1].strip(), '\n'.join(rows)
    return first


def main():
    driver = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    try:
        for columns in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-approval-pty-') as temp:
                directory = Path(temp)
                terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
                try:
                    terminal.wait_for_composer()
                    terminal.child.setwinsize(40, columns)
                    terminal.screen.resize(40, columns)
                    terminal.pump(.2)
                    cases = [
                        ('once', '\r', 'Approved (command)', True),
                        ('no', '\x1b[F\r', 'Denied (command)', False),
                        ('escape', '\x1b', 'Denied (command)', False),
                        ('session', '\x1b[B\r', 'Approved (command, session)', True),
                        ('always', '\x1b[B\x1b[B\r', 'Approved (command, always)', True),
                    ]
                    for name, keys, receipt, approved in cases:
                        terminal.send(f'\x15Review {name} at {columns}\r')
                        _, release = terminal.request()
                        releases.append(release)
                        draft = 'preserved draft 中文'
                        terminal.send(draft)
                        content = 'approval 中文 ' + 'long-command-without-spaces/' * 3
                        program = 'echo' if name == 'always' else 'printf'
                        filename = f'{name}.txt'
                        command = f"{program} '{content}' > {filename}"
                        release.delta = {'tool_calls': [{
                            'index': 0, 'id': f'approval-{name}', 'type': 'function',
                            'function': {'name': 'exec', 'arguments': json.dumps({
                                'command': command, 'host': True})}}]}
                        release.set()
                        text = until(terminal, lambda text: 'Enter confirm' in text)
                        first = closed_card(terminal)
                        assert 'Yes once' in text and 'Allow for this session' in text, text
                        assert 'Always allow for this project' in text, text
                        assert 'Other · Type' not in text and '[y]es' not in text, text
                        assert not any(terminal.screen.buffer[row][0].bg == '3a3a3a'
                                       for row in range(first, terminal.screen.lines)), text
                        assert draft not in text, text
                        # Pasted keys and newlines must never grant approval.
                        terminal.send('\x1b[200~3\r\x1b[201~')
                        assert Model.requests.empty(), 'paste submitted an approval'
                        terminal.send(keys)
                        body, release = terminal.request()
                        releases.append(release)
                        results = [message['content'] for message in body['messages']
                                   if message['role'] == 'tool' and
                                   message.get('tool_call_id') == f'approval-{name}']
                        assert results, body
                        text = terminal.snapshot(f'{name} approval at {columns} columns')
                        assert receipt in text, text
                        assert 'Enter confirm' not in text, text
                        prompt = editor_input(terminal, draft)
                        last = max(i for i, row in enumerate(terminal.screen.display)
                                   if row.startswith('╰'))
                        backgrounds = [row for row in range(last + 1, terminal.screen.lines)
                                       if terminal.screen.buffer[row][0].bg == '3a3a3a']
                        assert backgrounds == [prompt - 1, prompt, prompt + 1], text
                        assert (directory / filename).exists() == approved, results
                        if approved:
                            assert (directory / filename).read_text().strip() == content, results
                        with sqlite3.connect(directory / 'data.db') as database:
                            grants = database.execute('SELECT subject FROM permission_grants').fetchall()
                        assert bool(grants) == (name == 'always'), grants
                        release.delta = {'content': 'Approval decision received.'}
                        release.set()
                        until(terminal, lambda text: 'Working ·' not in text)
                    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                    print(f'PASS: closed card, decisions, paste and draft restoration at {columns} columns')
                finally:
                    terminal.child.close(force=True)
    finally:
        for release in releases:
            release.set()
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
