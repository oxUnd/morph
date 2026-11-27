"""Export a real completed CLI turn without a chat bubble or composer damage."""
from loopback_http import LoopbackHTTPServer
from pathlib import Path
import json
import re
import shutil
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal, http
from test_cli_agents_pty import tool_delta


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
                   if terminal.screen.buffer[i][0].bg == '3a3a3a']
    assert backgrounds == [prompts[0] - 1, prompts[0], prompts[0] + 1], backgrounds


def run_agent_export(driver, port, columns):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-share-agents-') as temp:
        directory = Path(temp)
        terminal = Terminal(driver, directory, port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n')
        try:
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(24, columns)
            terminal.wait_for_composer()
            terminal.send('TEAM_EXPORT_QUESTION\r')
            _, root = terminal.request()
            releases.append(root)
            root.delta = tool_delta('spawn_agent', {'task_name': 'reviewer',
                'message': 'Review the implementation', 'fork_turns': 'none'}, 'share-agent')
            root.set()
            pair = [terminal.request(), terminal.request()]
            releases.extend(release for _, release in pair)
            child = next(release for body, release in pair
                         if any('You are `/root/reviewer`' in message.get('content', '')
                                for message in body.get('messages', [])
                                if message['role'] == 'system'))
            parent = next(release for _, release in pair if release is not child)
            parent.delta = tool_delta('wait_agent', {'timeout_ms': 10000}, 'share-wait')
            parent.set()
            wait_for(terminal, lambda: 'Waiting for agents' in '\n'.join(terminal.screen.display))
            child.delta = {'content': 'TEAM_REVIEW_REPORT <verified>\nFULL_REPORT_DETAIL_ONLY'}
            child.set()
            _, final = terminal.request()
            releases.append(final)
            final.delta = {'content': 'TEAM_INTEGRATED_ANSWER'}
            final.set()
            wait_for(terminal, lambda: 'TEAM_INTEGRATED_ANSWER' in '\n'.join(terminal.screen.display)
                     and 'Working ·' not in '\n'.join(terminal.screen.display))
            downloads = directory / 'home' / 'Downloads'
            terminal.send('/share\r')
            wait_for(terminal, lambda: downloads.exists() and bool(list(downloads.glob('*.html'))))
            exported = next(downloads.glob('*.html')).read_text()
            questions = exported.split('<h2 class=label>YOU</h2>', 1)[1].split('</section>', 1)[0]
            assert 'TEAM_EXPORT_QUESTION' in questions
            assert 'TEAM_REVIEW_REPORT' not in questions and 'Sender:' not in questions, questions
            assert 'Sub-agent messages' in exported and 'View messages' in exported
            assert 'TEAM_REVIEW_REPORT &lt;verified&gt;' in exported
            assert 'TEAM_INTEGRATED_ANSWER' in exported
            session_ref = re.search(r'#([0-9a-f]{8})',
                                    '\n'.join(terminal.screen.display)).group(1)
            terminal.send('/quit\r')
            terminal.child.expect(__import__('pexpect').EOF, timeout=10)
            terminal.child.close()
            terminal = Terminal(driver, directory, port,
                                hooks_enabled=False,
                                extra_config='[agents]\nmulti_agent_mode="proactive"\n')
            terminal.child.setwinsize(24, columns)
            terminal.screen.resize(24, columns)
            terminal.wait_for_composer()
            terminal.send(f'/resume {session_ref}\r')
            wait_for(terminal, lambda: 'Finished waiting /root/reviewer' in
                     '\n'.join(terminal.screen.display))
            collapsed = '\n'.join(terminal.screen.display)
            assert 'TEAM_REVIEW_REPORT' in collapsed and 'Message Type:' not in collapsed
            assert 'FULL_REPORT_DETAIL_ONLY' not in collapsed
            assert collapsed.count('Finished waiting') == 1
            terminal.send('RESTORED_DRAFT')
            for label, expected in (('Show details', 'FULL_REPORT_DETAIL_ONLY'),
                                    ('Hide details', 'Show details')):
                rows = terminal.screen.display
                row = next(i for i, text in enumerate(rows)
                           if 'Finished waiting' in text and label in text)
                column = rows[row].index('Finished waiting') + 1
                terminal.send(f'\x1b[<0;{column};{row + 1}M\x1b[<0;{column};{row + 1}m')
                wait_for(terminal, lambda: expected in '\n'.join(terminal.screen.display))
                assert 'RESTORED_DRAFT' in '\n'.join(terminal.screen.display)
            assert 'Message Type:' not in '\n'.join(terminal.screen.display)
            print(f'PASS: delegated export and collapsed cold resume at {columns} columns')
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
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
        for columns in (120, 80, 60):
            run_agent_export(driver, server.server_port, columns)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
