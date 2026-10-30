"""Hook progress, warning output, and cancellation through the production TUI."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal
from test_cli_jobs_pty import until
from test_cli_transcript_pty import AlternateScreen
import pyte


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
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
