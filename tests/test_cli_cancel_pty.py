"""ESC cancellation across model calls, exec yields, jobs, and questions."""
import http.server
import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until


def stopped(terminal, seconds=.7):
    return until(terminal, lambda text: 'Working ·' not in text and
                 'Stopped after' in text, seconds)


def no_process(terminal, path):
    pid = int(path.read_text())
    deadline = time.monotonic() + 2
    while time.monotonic() < deadline:
        try:
            os.kill(pid, 0)
        except ProcessLookupError:
            return
        terminal.pump(.02)
    raise AssertionError(f'cancelled process {pid} is still alive')


def tools(release, calls):
    release.delta = {'tool_calls': [
        {'index': index, 'id': f'cancel_tool_{index}', 'type': 'function',
         'function': {'name': name, 'arguments': json.dumps(args)}}
        for index, (name, args) in enumerate(calls)]}
    release.set()


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-cancel-pty-') as temp:
            directory = Path(temp)
            terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
            terminal.wait_for_composer()

            # Idle ESC must never leave Readline waiting for a Meta suffix.
            terminal.send('\x1b')
            terminal.send('first character survives')
            text = terminal.snapshot('idle ESC preserves subsequent input')
            assert '› first character survives' in text, text
            terminal.send('\x15cancel silent model\r')
            _, release = terminal.request()
            releases.append(release)
            terminal.pump(.8)  # Model stops sending chunks after its first three.
            terminal.send('preserved draft')
            terminal.child.send('\x1b')
            text = stopped(terminal)
            assert 'preserved draft' in text, text
            release.set()

            terminal.send('\x15cancel foreground exec\r')
            _, release = terminal.request()
            releases.append(release)
            tools(release, [('exec', {
                'command': "trap '' TERM; printf $$ > foreground.pid; "
                           'while :; do sleep .05; done',
                'yield_time_ms': 10000})])
            until(terminal, lambda text: 'es once' in text or
                  (directory / 'foreground.pid').exists())
            if not (directory / 'foreground.pid').exists():
                terminal.send('s\r')
            until(terminal, lambda text: (directory / 'foreground.pid').exists())
            terminal.child.send('\x1b')
            stopped(terminal)
            no_process(terminal, directory / 'foreground.pid')

            terminal.send('cancel all current background jobs\r')
            _, release = terminal.request()
            releases.append(release)
            calls = []
            for index, mode in enumerate(({}, {'service': True}, {'notify_only': True})):
                calls.append(('exec', {
                    'command': f"trap '' TERM; printf $$ > job-{index}.pid; "
                               'while :; do sleep .05; done',
                    'background': True, **mode}))
            tools(release, calls)
            _, release = terminal.request()
            releases.append(release)
            until(terminal, lambda text: all((directory / f'job-{i}.pid').exists()
                                             for i in range(3)))
            terminal.send('draft after jobs')
            terminal.child.send('\x1b')
            text = stopped(terminal)
            assert 'draft after jobs' in text, text
            for i in range(3):
                no_process(terminal, directory / f'job-{i}.pid')
            release.set()

            terminal.send('\x15cancel a question\r')
            _, release = terminal.request()
            releases.append(release)
            tools(release, [('ask_user', {'question': 'Cancel this question',
                                         'choices': ['Yes', 'No']})])
            until(terminal, lambda text: 'Cancel this question' in text)
            terminal.child.send('\x1b')
            stopped(terminal)

            # Submit immediately after cancellation and exercise editing/paste.
            terminal.send('\x1b')
            terminal.send('\x1b[200~followup 中文\x1b[201~')
            terminal.send('\x1b[D\x1b[C\r')
            body, release = terminal.request()
            releases.append(release)
            users = [m['content'] for m in body['messages'] if m['role'] == 'user']
            assert users[-1] == 'followup 中文', users
            release.delta = {'content': 'Recovered after every cancellation.'}
            release.set()
            until(terminal, lambda text: 'Recovered after every cancellation.' in text)
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            print('PASS: prompt ESC, foreground and background termination, question cancellation, '
                  'and input recovery')
    finally:
        for release in releases:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
