"""Readable transcript sections and complete raw JSON in the real terminal."""
from pathlib import Path
import json
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal
from test_cli_slash_pty import wait_for
from test_cli_transcript_pty import AlternateScreen, PacedMarkdownModel
import pyte


def collect(terminal, label):
    terminal.send('\x1b[H')
    previous = terminal.snapshot(label + ': first page')
    pages = [previous]
    for _ in range(20):
        terminal.send('\x1b[6~')
        page = terminal.snapshot(label + ': page down')
        if page == previous:
            return '\n'.join(pages)
        pages.append(page)
        previous = page
    raise AssertionError('transcript did not reach its final page')


def main():
    driver = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width, color in ((120, True), (80, True), (60, True), (80, False)):
            with tempfile.TemporaryDirectory(prefix='morph-transcript-layout-') as temp:
                directory = Path(temp)
                (directory / 'read.txt').write_text('FILE_CONTENT_SENTINEL\n')
                (directory / 'target.c').write_text('context\nold value\nend\n')
                terminal = Terminal(driver if color else driver.parent / 'morph', directory,
                                    server.server_port, hooks_enabled=False,
                                    production=not color,
                                    extra_arguments=() if color else ('--no-color',),
                                    extra_env={'NO_COLOR': '' if color else '1'},
                                    extra_config='[permissions]\naccess="full"\n'
                                                 'approval="never"\nreviewer="user"\n')
                terminal.child.setwinsize(30, width)
                terminal.screen = AlternateScreen(width, 30)
                terminal.stream = pyte.Stream(terminal.screen)
                chunks = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('Inspect and update the example.\r')
                    _, chunks = terminal.request()
                    chunks.put({'reasoning_content': 'REASONING_SENTINEL: inspect before editing.'})
                    chunks.put({'tool_calls': [
                        {'index': 0, 'id': 'layout-read', 'type': 'function', 'function': {
                            'name': 'file_read', 'arguments': json.dumps({
                                'file_path': str(directory / 'read.txt')})}},
                        {'index': 1, 'id': 'layout-patch', 'type': 'function', 'function': {
                            'name': 'apply_patch', 'arguments': json.dumps({
                                'input': '*** Begin Patch\n*** Update File: target.c\n@@\n'
                                         '-old value\n+PATCH_CONTENT_SENTINEL\n*** End Patch'})}}
                    ]})
                    chunks.put(None)
                    _, chunks = terminal.request()
                    draft = 'draft中文🙂 ' + ('wrapped input ' * 4 if width == 60 else '') + 'tail'
                    terminal.send(draft + '\x1b[D\x14')
                    terminal.snapshot(f'{width}: formatted transcript opens')
                    terminal.send('r')
                    wait_for(terminal, lambda: 'raw JSON' in terminal.screen.display[3])
                    terminal.send('\x1b[H')
                    raw = terminal.snapshot(f'{width}: raw JSON has its own mode')
                    assert 'Arguments JSON' in raw and 'R formatted' in raw, raw
                    chunks.put({'tool_calls': [{'index': 0, 'id': 'layout-exec',
                                               'type': 'function', 'function': {
                                                   'name': 'exec', 'arguments': json.dumps({
                                                       'command': "printf 'STDOUT_SENTINEL\\n'; "
                                                                  "printf 'STDERR_SENTINEL\\n' >&2",
                                                       'yield_time_ms': 1000})}}]})
                    chunks.put(None)
                    _, chunks = terminal.request()
                    chunks.put('Verified the result.')
                    chunks.put(None)
                    wait_for(terminal, lambda: '3 calls' in '\n'.join(terminal.screen.display))
                    assert 'raw JSON' in terminal.screen.display[3], terminal.screen.display
                    raw_document = collect(terminal, f'{width}: complete raw transcript')
                    for key in ('Arguments JSON', 'Result JSON', 'patch_diff', 'checkpoint',
                                'workspace_root', 'STDOUT_SENTINEL', 'STDERR_SENTINEL'):
                        assert key in raw_document, (key, raw_document)
                    terminal.send('R')
                    wait_for(terminal, lambda: 'raw JSON' not in terminal.screen.display[3])
                    formatted = collect(terminal, f'{width}: readable transcript')
                    for text in ('REASONING_SENTINEL', 'Arguments', 'Content', 'Changes',
                                 'Standard output', 'Standard error', 'Exit 0',
                                 'FILE_CONTENT_SENTINEL', 'PATCH_CONTENT_SENTINEL',
                                 'STDOUT_SENTINEL', 'STDERR_SENTINEL'):
                        assert text in formatted, (text, formatted)
                    for key in ('patch_diff', 'captured_paths', 'workspace_root', 'artifacts'):
                        assert key not in formatted, (key, formatted)
                    terminal.send('r')
                    resized_width = 80 if width != 80 else 60
                    terminal.child.setwinsize(30, resized_width)
                    terminal.screen.resize(30, resized_width)
                    terminal.pump(.3)
                    resized = terminal.snapshot(f'{width}: resize preserves raw mode')
                    assert 'raw JSON' in resized and 'R formatted' in resized, resized
                    terminal.send('\x1b')
                    terminal.send('X')
                    restored = terminal.snapshot(f'{width}: closing restores the draft and caret')
                    assert 'draft中文🙂' in restored and 'taiXl' in restored, restored
                    assert 'Turn details' not in restored, restored
                    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                finally:
                    if chunks is not None:
                        chunks.put(None)
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    main()
