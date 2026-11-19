"""Exercise full-screen navigation through the CLI and a controlled local model."""
import json
from pathlib import Path
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_jobs_pty import until
from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen, PacedMarkdownModel
import pyte


def run(binary, port, color):
    pending = []
    with tempfile.TemporaryDirectory(prefix='morph-page-layout-') as temp:
        directory = Path(temp)
        terminal = Terminal(binary, directory, port, memory_enabled=True,
                            production=True,
                            extra_env={'NO_COLOR': '' if color else '1'})
        terminal.screen = AlternateScreen(80, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            terminal.send('/permissions mode full\r')
            terminal.send('Prepare navigation fixtures\r')
            _, chunks = terminal.request()
            pending.append(chunks)
            chunks.put({'tool_calls': [{'index': 0, 'id': 'navigation-job',
                'type': 'function', 'function': {'name': 'exec', 'arguments': json.dumps({
                    'command': 'printf NAVIGATION_JOB_OUTPUT', 'background': True,
                    'notify_only': True, 'description': 'Navigation job'})}}]})
            chunks.put(None)
            _, chunks = terminal.request()
            pending.append(chunks)
            chunks.put('NAVIGATION_REPLY')
            chunks.put(None)
            until(terminal, lambda text: 'NAVIGATION_REPLY' in text and 'Working ·' not in text)
            (directory / 'hooks.json').write_text(json.dumps({'hooks': {'PreToolUse': [{
                'hooks': [{'type': 'command', 'command': 'touch hook-executed'}]
            }]}}))

            for width in ((120, 80, 60) if color else (80,)):
                terminal.child.setwinsize(30, width)
                terminal.screen.resize(lines=30, columns=width)
                terminal.pump(.2)
                for page, subpage in (('jobs', 'output'), ('history', 'entry details'),
                                      ('memory', 'Saved facts'), ('hooks', 'review'),
                                      ('rewind', 'restore')):
                    terminal.send(f'/{page}\r')
                    terminal.assert_navigation(page, color=color)
                    if page == 'memory':
                        terminal.send('\x1b[B\x1b[B')
                    terminal.send('\r')
                    terminal.assert_navigation(page, subpage, color=color)
                    resized = 80 if width != 80 else 60
                    terminal.child.setwinsize(30, resized)
                    terminal.screen.resize(lines=30, columns=resized)
                    terminal.pump(.2)
                    terminal.assert_navigation(page, subpage, color=color)
                    terminal.child.setwinsize(30, width)
                    terminal.screen.resize(lines=30, columns=width)
                    original = terminal.snapshot(f'{width}: {page} subpage')
                    terminal.click_navigation(2)
                    terminal.assert_navigation(page, subpage, color=color)
                    terminal.click_navigation(1)
                    terminal.assert_navigation(page, color=color)
                    terminal.send('\r')
                    terminal.assert_navigation(page, subpage, color=color)
                    reopened = terminal.snapshot(f'{width}: {page} selection retained')
                    assert original == reopened, reopened
                    terminal.send('\x1b')
                    terminal.assert_navigation(page, color=color)
                    terminal.send('\r')
                    terminal.click_navigation(0)
                    text = terminal.snapshot(f'{width}: {page} home returns to chat')
                    assert 'perm: full' in text and f'morph / {page}' not in text, text

                for page in ('resume', 'agents'):
                    terminal.send(f'/{page}\r')
                    terminal.assert_navigation(page, color=color)
                    text = terminal.snapshot(f'{width}: {page} root')
                    if page == 'resume':
                        assert sum(set(row.strip()) == {'─'} for row in text.splitlines()) == 2
                    resized = 80 if width != 80 else 60
                    terminal.child.setwinsize(30, resized)
                    terminal.screen.resize(lines=30, columns=resized)
                    terminal.pump(.2)
                    terminal.assert_navigation(page, color=color)
                    terminal.click_navigation(0)
                    assert 'perm: full' in terminal.snapshot(f'{width}: {page} home'), page
                    terminal.child.setwinsize(30, width)
                    terminal.screen.resize(lines=30, columns=width)
                    terminal.pump(.2)

                terminal.send('KEEP_LAYOUT_DRAFT 中文')
                terminal.send('\x14')
                terminal.assert_navigation('turn details', color=color)
                terminal.click_navigation(0)
                text = terminal.snapshot(f'{width}: turn details restores draft')
                assert 'KEEP_LAYOUT_DRAFT 中文' in text, text
                terminal.send('\x15')
            assert not (directory / 'hook-executed').exists()
            terminal.send('/quit\r')
        finally:
            for chunks in pending:
                chunks.put(None)
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        run(binary, server.server_port, color=True)
        run(binary, server.server_port, color=False)
    finally:
        server.shutdown()
        server.server_close()
    print('PASS: navigation, selection, draft restoration, resize and two resume rules')


if __name__ == '__main__':
    main()
