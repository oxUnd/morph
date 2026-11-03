"""Show truncation warnings without losing partial output or the editable composer."""
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


class TruncationModel(Model):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        messages = body.get('messages', [])
        finish = 'stop'
        if not body.get('tools'):
            delta = {'content': '{"title":"Truncation regression"}'}
        elif any('probe output limit' in str(item.get('content', ''))
                 for item in messages if item.get('role') == 'user'):
            delta = {'content': 'PARTIAL_REPLY_MARKER'}
            finish = 'length'
        else:
            results = sum(item.get('role') == 'tool' for item in messages)
            if results < 3:
                delta = {'tool_calls': [{'index': 0, 'id': f'read-{results}',
                         'type': 'function', 'function': {
                             'name': 'file_read', 'arguments': json.dumps({
                                 'file_path': str(self.server.fixture)})}}]}
                finish = 'tool_calls'
            else:
                delta = {'content': 'READS_COMPLETE_MARKER'}
        payload = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': finish}]}
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            self.wfile.write(('data: ' + json.dumps(payload) + '\n\n'
                              'data: [DONE]\n\n').encode())
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), TruncationModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width in (120, 80, 60):
            for output_limit in (False, True):
                with tempfile.TemporaryDirectory(prefix='morph-truncation-pty-') as temp:
                    directory = Path(temp)
                    server.fixture = directory / 'large.txt'
                    server.fixture.write_text('relevant source material ' * 3000)
                    terminal = Terminal(driver, directory, server.server_port,
                                        hooks_enabled=False, extra_config='''
[context]
tool_result_max_tokens = 256
''')
                    terminal.screen = AlternateScreen(width, 32)
                    terminal.stream = pyte.Stream(terminal.screen)
                    try:
                        terminal.wait_for_composer()
                        terminal.child.setwinsize(32, width)
                        terminal.pump(.2)
                        terminal.send(('probe output limit' if output_limit else
                                       'probe tool history truncation') + '\r')
                        needle = ('Model output reached its token limit' if output_limit else
                                  '3 of 3 tool results were truncated')
                        until(terminal, lambda text: needle in terminal.raw)
                        terminal.pump(.5)
                        marker = 'PARTIAL_REPLY_MARKER' if output_limit else 'READS_COMPLETE_MARKER'
                        assert marker in terminal.raw, terminal.raw
                        key = 'model.text.max_tokens' if output_limit else 'context.tool_result_max_tokens'
                        assert key in terminal.raw, terminal.raw
                        if output_limit:
                            assert 'Error' in terminal.raw, terminal.raw
                        terminal.send('editable after warning')
                        screen = terminal.snapshot(f'truncation warning at {width} columns')
                        assert 'editable after warning' in screen, screen
                        assert 'Warning' in screen, screen
                        assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                    finally:
                        terminal.child.close(force=True)
        print('PASS: truncation warnings, partial output, and composer at 120/80/60 columns')
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
