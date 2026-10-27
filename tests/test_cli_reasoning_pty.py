"""Collapse reasoning in the feed and retain its live text in the details view."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte


REASONING = 'Inspecting the current state carefully.\n检查推理输出是否完整，中文换行也要保持正确。'


class ReasoningModel(Model):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        release = threading.Event()
        title = any('title' in message.get('content', '').lower()
                    for message in body.get('messages', []) if message.get('role') == 'system')
        title = title and 'tools' not in body
        release.finish = threading.Event()
        if title:
            release.set()
        else:
            Model.requests.put((body, release))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        try:
            if not release.wait(10):
                return
            deltas = ([{'content': '{"title":"Reasoning regression"}'}] if title else
                      [{'reasoning_content': REASONING[i:i + 3]}
                       for i in range(0, len(REASONING), 3)] + [{'content': 'Answer complete.'}])
            for delta in deltas:
                payload = {'choices': [{'index': 0, 'delta': delta}]}
                self.wfile.write(('data: ' + json.dumps(payload) + '\n\n').encode())
                self.wfile.flush()
                time.sleep(.07)
                if 'reasoning_content' in delta and delta == deltas[-2]:
                    release.finish.wait(10)
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},"finish_reason":"stop"}]}\n\n'
                            b'data: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def main():
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), ReasoningModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width, inspect_details in ((80, False), (40, False), (80, True), (40, True)):
            with tempfile.TemporaryDirectory(prefix='morph-reasoning-pty-') as temp:
                terminal = Terminal(Path(sys.argv[1]).resolve(), Path(temp),
                                    server.server_port, hooks_enabled=False)
                terminal.screen = AlternateScreen(80, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                release = None
                try:
                    terminal.pump(1)
                    terminal.child.setwinsize(30, width)
                    terminal.screen.resize(lines=30, columns=width)
                    terminal.pump(.5)
                    terminal.send('Check reasoning rendering\r')
                    _, release = terminal.request()
                    terminal.send('keep my draft')
                    release.set()
                    terminal.pump(.55)
                    early = terminal.snapshot(f'{width} columns: first reasoning preview')
                    early_rows = early.splitlines()
                    early_question = next(i for i, row in enumerate(early_rows)
                                          if row.startswith('› Check reasoning rendering'))
                    early_preview = next(i for i, row in enumerate(early_rows)
                                         if 'Inspecting' in row)
                    early_distance = early_preview - early_question
                    assert early_rows[early_preview].startswith('Inspecting'), early
                    assert terminal.screen.buffer[early_preview][0].italics, early
                    terminal.pump(1.65)
                    streaming = terminal.snapshot(f'{width} columns: live reasoning and draft')
                    compact = ''.join(streaming.split())
                    assert 'Inspecting' in compact and '…' in compact, streaming
                    assert '检查推理输出' not in compact, streaming
                    assert 'keepmydraft' in compact, streaming
                    assert 'Answercomplete.' not in compact, streaming
                    rows = streaming.splitlines()
                    question_row = next(i for i, row in enumerate(rows)
                                        if row.startswith('› Check reasoning rendering'))
                    preview_row = next(i for i, row in enumerate(rows) if 'Inspecting' in row)
                    preview_distance = preview_row - question_row
                    assert preview_distance == early_distance, streaming
                    if inspect_details:
                        terminal.send('\x0f')
                        details = terminal.snapshot(f'{width} columns: live reasoning details')
                        assert 'Inspectingthecurrentstatecarefully.' in ''.join(details.split()), details
                    release.finish.set()
                    terminal.pump(1)
                    if inspect_details:
                        details = terminal.snapshot(f'{width} columns: completed reasoning details')
                        compact = ''.join(details.split())
                        assert '检查推理输出是否完整，中文换行也要保持正确。' in compact, details
                        assert 'Turn details' in details, details
                        terminal.send('\x0f')
                    text = terminal.snapshot(f'{width} columns: reasoning, answer, and draft')
                    compact = ''.join(text.split())
                    assert 'Inspecting' in compact and '…' in compact, text
                    assert '检查推理输出是否完整，中文换行也要保持正确。' not in compact, text
                    assert 'Answercomplete.' in compact, text
                    assert 'keepmydraft' in compact, text
                    assert compact.count('Inspecting') == 1, text
                    preview_rows = [row for row in text.splitlines() if 'Inspecting' in row]
                    assert len(preview_rows) == 1, text
                    assert '…' in preview_rows[0], text
                    rows = text.splitlines()
                    question_row = next(i for i, row in enumerate(rows)
                                        if row.startswith('› Check reasoning rendering'))
                    preview_row = next(i for i, row in enumerate(rows) if 'Inspecting' in row)
                    assert preview_row - question_row == preview_distance, text
                    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                finally:
                    if release:
                        release.set()
                        release.finish.set()
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
