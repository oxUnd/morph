"""Exercise full-screen tool details through the production Readline event loop."""
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import copy

from test_cli_pty import Model, Terminal
import pyte


class AlternateScreen(pyte.Screen):
    """Model xterm's separate main/alternate buffers for viewport assertions."""
    def set_mode(self, *modes, **kwargs):
        if kwargs.get('private') and 1049 in modes:
            saved = copy.deepcopy(self.__dict__)
            self.reset()
            self._main_screen = saved
            modes = tuple(mode for mode in modes if mode != 1049)
        super().set_mode(*modes, **kwargs)

    def reset_mode(self, *modes, **kwargs):
        if kwargs.get('private') and 1049 in modes:
            saved = getattr(self, '_main_screen', None)
            if saved is not None:
                self.__dict__.clear()
                self.__dict__.update(saved)
            modes = tuple(mode for mode in modes if mode != 1049)
        super().reset_mode(*modes, **kwargs)


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='morph-transcript-') as temp:
        directory = Path(temp)
        (directory / 'first.txt').write_text('FIRST_PRIVATE_RESULT\n', encoding='utf-8')
        second_content = ''.join(f'SECOND_LINE_{i:03d}\n' for i in range(200))
        second_content += 'SECOND_PRIVATE_RESULT 中文🙂\n'
        (directory / 'second.txt').write_text(second_content, encoding='utf-8')
        terminal = Terminal(driver, directory, server.server_port)
        terminal.child.setwinsize(24, 120)
        terminal.screen = AlternateScreen(120, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        releases = []
        try:
            terminal.pump(1)
            terminal.send('read both files\r')
            _, release = terminal.request()
            releases.append(release)
            terminal.pump(0.8)
            streaming = terminal.snapshot('stream prefix survives composer redraw')
            assert any(row.startswith('● Live output 0.')
                       for row in streaming.splitlines()), streaming
            release.delta = {'tool_calls': [
                {'index': i, 'id': f'read-{i}', 'type': 'function',
                 'function': {'name': 'file_read', 'arguments': json.dumps({
                     'file_path': str(directory / name)})}}
                for i, name in enumerate(('first.txt', 'second.txt'))]}
            release.set()
            response, final_release = terminal.request()
            releases.append(final_release)
            assert sum(m['role'] == 'tool' for m in response['messages']) == 2
            compact = terminal.snapshot('compact tool results')
            rows = [row for row in compact.splitlines() if '◯ file_read' in row]
            assert len(rows) == 2, compact
            assert all(row.startswith('◯ file_read') for row in rows), compact
            assert all(len(row.rstrip()) <= 118 for row in rows), compact
            assert all(not row[len('◯ file_read '):].startswith(' ')
                       for row in rows), compact
            assert 'FIRST_PRIVATE_RESULT' not in compact, compact
            assert 'SECOND_PRIVATE_RESULT' not in compact, compact
            terminal.send('草稿🙂 tail\x1b[D')
            terminal.send('\x0f')
            expanded = terminal.snapshot('expanded while model running')
            assert 'SECOND_PRIVATE_RESULT 中文🙂' in expanded, expanded
            assert 'Tool details' in expanded, expanded
            assert '草稿🙂 tail' not in expanded, expanded
            terminal.send('\x1b[<64;10;10M')
            wheel = terminal.snapshot('mouse wheel scrolls details')
            assert wheel != expanded, wheel
            burst_start = len(terminal.raw)
            terminal.child.send('\x1b[A' * 200)
            terminal.pump(0.5)
            drained = terminal.snapshot('wheel burst drains without momentum backlog')
            assert 'SECOND_LINE_000' in drained, drained
            assert len(terminal.raw) - burst_start < 30000, terminal.raw[burst_start:]
            terminal.send('\x1b[F')
            assert 'SECOND_PRIVATE_RESULT 中文🙂' in terminal.snapshot(
                'end restores follow'), terminal.raw
            terminal.send('\x1b[5~')
            page = terminal.snapshot('page up through details')
            assert page != expanded, page
            terminal.send('\x1b[H')
            page = terminal.snapshot('home through details')
            assert 'FIRST_PRIVATE_RESULT' in page, page
            terminal.send('\x1b')
            collapsed = terminal.snapshot('collapsed while model running')
            assert 'SECOND_PRIVATE_RESULT' not in collapsed, collapsed
            assert '草稿🙂 tail' in collapsed, collapsed
            terminal.send('X')
            assert '草稿🙂 taiXl' in terminal.snapshot('draft cursor preserved'), terminal.raw
            terminal.send('\x0f')
            switches = terminal.raw.count('\x1b[?1049')
            clears = terminal.raw.count('\x1b[2J')
            final_release.delta = {'tool_calls': [
                {'index': 0, 'id': 'read-in-viewer', 'type': 'function',
                 'function': {'name': 'file_read', 'arguments': json.dumps({
                     'file_path': str(directory / 'first.txt')})}}]}
            final_release.set()
            _, last_release = terminal.request()
            releases.append(last_release)
            assert '3 calls' in terminal.snapshot('new tool result updates in place')
            last_release.set()
            terminal.pump(1)
            assert 'Tool details' in terminal.snapshot('details remain open after completion')
            assert terminal.raw.count('\x1b[?1049') == switches, terminal.raw
            assert terminal.raw.count('\x1b[2J') == clears, terminal.raw
            unchanged = terminal.raw
            terminal.pump(0.6)
            assert terminal.raw == unchanged, 'unchanged details should not repaint'
            terminal.send('\x0f')
            completed = terminal.snapshot('finished process remains visible')
            assert completed.count('UPDATED ANSWER') == 1, completed
            assert 'file_read' in completed, completed
            assert 'succeeded' not in completed, completed
            assert '草稿🙂 taiXl' in completed, completed
            terminal.send('\x0f')
            assert 'FIRST_PRIVATE_RESULT' in terminal.snapshot('expand after completion')
            terminal.send('\x0f')
            assert 'SECOND_PRIVATE_RESULT' not in terminal.snapshot('collapse after completion')
            terminal.child.setwinsize(24, 36)
            terminal.screen.resize(24, 36)
            terminal.pump(0.2)
            terminal.send('\x0f\x0f')
            narrow = terminal.snapshot('narrow transcript toggles')
            assert '草稿🙂 taiXl' in narrow, narrow
            assert 'UPDATED ANSWER' in narrow, narrow
            assert '\x1b[?1049h' in terminal.raw, 'must use a separate details screen'
            assert '\x1b[?1049l' in terminal.raw, 'must restore the main screen'
            assert '\x1b[3J' not in terminal.raw, 'must preserve terminal scrollback'
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            terminal.send('\x15ask a question\r')
            _, ask_release = terminal.request()
            releases.append(ask_release)
            terminal.send('question draft\x0f')
            ask_release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-viewer',
                'type': 'function', 'function': {'name': 'ask_user',
                'arguments': json.dumps({'question': 'Choose a color',
                                         'choices': ['Blue', 'Red']})}}]}
            ask_release.set()
            terminal.pump(1)
            question = terminal.snapshot('question exits viewer without hiding input')
            assert 'Choose a color' in question, question
            assert 'Tool details' not in question, question
            terminal.send('1\r')
            answered, answered_release = terminal.request()
            releases.append(answered_release)
            assert any(m['role'] == 'tool' and 'Blue' in m['content']
                       for m in answered['messages']), answered
            assert 'question draft' in terminal.snapshot('draft restored after question')
            answered_release.set()
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
