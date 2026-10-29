"""Background command lifecycle through the real CLI, model, and terminal."""
import http.server
import json
from pathlib import Path
import queue
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte


def until(terminal, predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        terminal.pump(.05)
        text = '\n'.join(terminal.screen.display)
        if predicate(text):
            return text
    raise AssertionError('\n'.join(terminal.screen.display))


def main():
    driver = Path(sys.argv[1]).resolve()
    artifacts = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-jobs-pty-') as temp:
            directory = Path(temp)
            terminal = Terminal(driver, directory, server.server_port)
            terminal.child.setwinsize(30, 100)
            terminal.screen = AlternateScreen(100, 30)
            terminal.stream = pyte.Stream(terminal.screen)
            until(terminal, lambda text: 'ctrl+o details' in text)
            terminal.pump(.2)
            terminal.send('Build and inspect background work\r')
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'tool_calls': []}
            commands = [
                {'command': 'printf BUILD_STARTED; while [ ! -f release-build ]; do sleep .05; done; '
                            'printf BUILD_FINISHED; exit 7',
                 'description': '编译项目', 'background': True},
                {'command': 'printf SERVICE_STARTED; while :; do sleep .1; done',
                 'service': True, 'background': True},
            ]
            for index, args in enumerate(commands):
                release.delta['tool_calls'].append({
                    'index': index, 'id': f'background_job_{index}', 'type': 'function',
                    'function': {'name': 'exec', 'arguments': json.dumps(args)}})
            release.set()
            until(terminal, lambda text: 'es once' in text or '编译项目' in text)
            if 'es once' in '\n'.join(terminal.screen.display):
                terminal.send('s\r')
            body, release = terminal.request()
            releases.append(release)
            text = terminal.snapshot('background jobs remain visible while model works')
            assert '编译项目' in text and '└─' in text, text
            assert 'while [ ! -f release-build' in text, text
            assert 'SERVICE_STARTED' in text, text
            assert any(row.startswith('    └─ ') for row in terminal.screen.display), text
            if artifacts:
                (artifacts / 'live.txt').write_text(text)
                cells = [[{'text': terminal.screen.buffer[row][col].data,
                           'fg': terminal.screen.buffer[row][col].fg,
                           'bg': terminal.screen.buffer[row][col].bg}
                          for col in range(100)] for row in range(30)]
                (artifacts / 'live.json').write_text(json.dumps(cells, ensure_ascii=False))
            terminal.child.setwinsize(24, 40)
            terminal.screen.resize(lines=24, columns=40)
            terminal.pump(.4)
            narrow = terminal.snapshot('narrow task area retains indentation')
            assert '编译项目' in narrow and '└─' in narrow, narrow
            terminal.child.setwinsize(30, 100)
            terminal.screen.resize(lines=30, columns=100)
            terminal.pump(.4)
            with sqlite3.connect(directory / 'data.db') as db:
                records = db.execute('SELECT process_id,data_json FROM process_jobs').fetchall()
            assert len(records) == 2, records
            build_id = next(pid for pid, data in records if json.loads(data)['description'])
            service_id = next(pid for pid, data in records if json.loads(data)['service'])
            terminal.send('/jobs\r')
            text = until(terminal, lambda text: 'morph / jobs' in text)
            assert 'running' in text, text
            assert not any(row.startswith('› /jobs') for row in terminal.screen.display), text
            terminal.send('\x1b')
            terminal.send('/jobs ' + service_id[5:13] + '\r')
            until(terminal, lambda text: 'morph / jobs / output' in text)
            terminal.send('k')
            text = until(terminal, lambda text: 'killed' in text)
            assert 'Stop requested' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            text = terminal.snapshot('jobs panel cancellation restores the composer')
            assert 'morph / jobs' not in text, text
            terminal.send('keep my draft')
            text = terminal.snapshot('draft remains editable above the footer')
            assert 'keep my draft' in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            release.delta = {'content': 'Premature completion candidate.'}
            release.set()
            terminal.pump(.6)
            assert Model.requests.empty(), 'finite job unexpectedly accepted as completed'
            assert 'Worked for' not in '\n'.join(terminal.screen.display)
            (directory / 'release-build').touch()
            body, release = terminal.request()
            releases.append(release)
            completion = [m['content'] for m in body['messages']
                          if 'Runtime process completion records' in str(m.get('content', ''))]
            assert completion and 'BUILD_FINISHED' in completion[0], body
            assert '"exit_code":7' in completion[0], completion
            release.delta = {'content': 'Verified build failure: exit 7.'}
            release.set()
            text = until(terminal, lambda text: 'Worked for' in text)
            assert 'Verified build failure' in text, text
            assert 'keep my draft' in text, text
            assert text.count('exited 7 · /jobs') == 1, text
            with sqlite3.connect(directory / 'data.db') as db:
                data = json.loads(db.execute('SELECT data_json FROM process_jobs WHERE process_id=?',
                                             (build_id,)).fetchone()[0])
            assert data['exit_code'] == 7 and 'BUILD_FINISHED' in data['stdout'], data
            terminal.send('\x15/jobs ' + build_id[5:13] + '\r')
            text = until(terminal, lambda text: 'morph / jobs / output' in text)
            assert 'BUILD_FINISHED' in text and 'exit 7' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            text = terminal.snapshot('output panel returns without stale backgrounds')
            assert 'morph / jobs' not in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            terminal.send('Start a job and notify me later\r')
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'tool_calls': [{
                'index': 0, 'id': 'notify_only_job', 'type': 'function',
                'function': {'name': 'exec', 'arguments': json.dumps({
                    'command': 'while [ ! -f release-notify ]; do sleep .05; done; printf NOTIFIED',
                    'description': 'Notify-only verification',
                    'background': True, 'notify_only': True})}}]}
            release.set()
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'content': 'Started; I will notify you.'}
            release.set()
            text = until(terminal, lambda text: 'Started; I will notify you.' in text)
            terminal.pump(.3)
            assert 'Notify-only verification' in text, text
            terminal.send('idle draft')
            (directory / 'release-notify').touch()
            text = until(terminal, lambda text: 'Notify-only verification · exited 0' in text)
            assert 'idle draft' in text, text
            assert Model.requests.empty(), 'notification caused an unsolicited model turn'
            terminal.pump(5.4)
            text = terminal.snapshot('idle completion expires while preserving the draft')
            assert 'idle draft' in text, text
            assert not any('└─ while [ ! -f release-notify' in row
                           for row in terminal.screen.display), text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
    finally:
        for release in releases:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
