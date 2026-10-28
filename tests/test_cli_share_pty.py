"""Export a real completed CLI turn without a chat bubble or composer damage."""
from pathlib import Path
import json
import shutil
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, http


def wait_for(terminal, predicate):
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        terminal.pump(.05)
        if predicate():
            return
    raise AssertionError('\n'.join(terminal.screen.display))


def check_composer(terminal):
    rows = terminal.screen.display
    prompts = [i for i, row in enumerate(rows) if row.startswith('› ')]
    assert len(prompts) == 1, '\n'.join(rows)
    assert '› /share' not in '\n'.join(rows), '\n'.join(rows)
    backgrounds = [i for i in range(terminal.screen.lines)
                   if terminal.screen.buffer[i][0].bg == '1c1c1c']
    assert backgrounds == [prompts[0] - 1, prompts[0], prompts[0] + 1], backgrounds


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    release = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-share-pty-') as temp:
            directory = Path(temp)
            terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
            downloads = directory / 'home' / 'Downloads'
            try:
                wait_for(terminal, lambda: any(row.startswith('› ') for row in terminal.screen.display))
                terminal.send('/new share-test\r')
                terminal.send('/share\r')
                wait_for(terminal, lambda: 'no completed conversation' in '\n'.join(terminal.screen.display))
                assert not downloads.exists()
                check_composer(terminal)
                terminal.send('请把本次对话保存成可分享的 HTML。\r')
                _, release = terminal.request()
                terminal.send('/share\r')
                assert not downloads.exists(), 'active turn exported before completion'
                assert 'wait' in '\n'.join(terminal.screen.display).lower()
                terminal.send('\x15')
                release.delta = {'tool_calls': [{'index': 0, 'id': 'share-test-tool',
                                 'type': 'function', 'function': {'name': 'file_info',
                                 'arguments': json.dumps({'file_path': str(directory / 'config.toml')})}}]}
                release.set()
                _, release = terminal.request()
                release.delta = {'content': '可以，保留问题、执行过程与完整回答。\n\n'
                                 '**独立 HTML 文件**，无需联网即可阅读。\n\n'
                                 '- 支持明亮与暗黑主题\n- 保留 Markdown 排版\n\n'
                                 '```sh\n/share\n```\n\n'
                                 '| Format | Offline |\n|---|---|\n| HTML | Yes |'}
                release.set()
                wait_for(terminal, lambda: 'HTML' in '\n'.join(terminal.screen.display)
                         and not any('esc to interrupt' in row for row in terminal.screen.display))
                terminal.send('/share\r')
                wait_for(terminal, lambda: downloads.exists() and len(list(downloads.glob('*.html'))) == 1)
                wait_for(terminal, lambda: 'saved conversation to' in '\n'.join(terminal.screen.display))
                terminal.pump(.2)
                check_composer(terminal)
                assert any(row.rstrip() == '/share' for row in terminal.screen.display)
                path = next(downloads.glob('*.html'))
                html = path.read_text()
                assert '请把本次对话' in html and '<strong>' in html
                assert 'data-set-theme=dark' in html and 'data:image/png;base64,' in html
                assert 'Shared from morph' in html and 'UTC' in html
                assert '<details class=process>' in html and 'file_info' in html
                assert not list(downloads.glob('.morph-share-*'))
                if len(sys.argv) > 2:
                    shutil.copyfile(path, sys.argv[2])
                terminal.send('/share\r')
                wait_for(terminal, lambda: len(list(downloads.glob('*.html'))) == 2)
                check_composer(terminal)
                print('PASS: offline HTML export, repeated save, empty-session error and composer restoration')
            finally:
                if release:
                    release.set()
                terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
