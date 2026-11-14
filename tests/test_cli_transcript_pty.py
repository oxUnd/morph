"""Exercise full-screen tool details through the production TUI event loop."""
from loopback_http import LoopbackHTTPServer
import http.server
import json
from pathlib import Path
import sys
import tempfile
import threading
import time
import copy
import queue

from test_cli_pty import Model, Terminal
import pyte
import pexpect


class PacedMarkdownModel(Model):
    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()
        is_title = any(message.get('role') == 'system' and
                       message.get('content', '').startswith('Summarize the user')
                       for message in body.get('messages', []))
        chunks = queue.Queue()
        if is_title:
            chunks.put('{"title":"Markdown preview"}')
            chunks.put(None)
        else:
            self.requests.put((body, chunks))
        try:
            while True:
                text = chunks.get(timeout=15)
                if text is None:
                    break
                event = {'choices': [{'index': 0, 'delta': (text if isinstance(text, dict)
                                                         else {'content': text})}]}
                self.wfile.write(('data: ' + json.dumps(event) + '\n\n').encode())
                self.wfile.flush()
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},'
                             b'"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError, queue.Empty):
            pass


def check_mutable_markdown_preview(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-markdown-preview-') as temp:
                terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
                terminal.child.setwinsize(24, width)
                terminal.screen = AlternateScreen(width, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                chunks = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('stream Markdown gradually\r')
                    _, chunks = terminal.request()
                    chunks.put('FIRST_PREVIEW')
                    first = terminal.snapshot(f'{width}: partial word before newline or final')
                    assert first.count('FIRST_PREVIEW') == 1, first
                    terminal.send('draft🙂')
                    chunks.put(' **bold** 中文🙂' + ' wrap' * 26)
                    inline = terminal.snapshot(f'{width}: inline Markdown updates in place')
                    assert inline.count('FIRST_PREVIEW') == 1, inline
                    assert 'bold' in inline and '**bold**' not in inline, inline
                    assert 'draft🙂' in inline, inline
                    chunks.put('\n\nSECOND_PREVIEW')
                    second = terminal.snapshot(f'{width}: committed block plus mutable tail')
                    assert second.count('FIRST_PREVIEW') == 1, second
                    assert second.count('SECOND_PREVIEW') == 1, second
                    chunks.put('\n\n| a | b |\n|---|---|\n| LIVE_TABLE | value |\n')
                    table = terminal.snapshot(f'{width}: table previews before its final boundary')
                    assert 'LIVE_TABLE' in table and 'draft🙂' in table, table
                    chunks.put('\n```c\nint value = 1;')
                    code = terminal.snapshot(f'{width}: open code block before closing fence')
                    assert 'int value = 1;' in code, code
                    resized_width = 80 if width == 60 else 60
                    terminal.child.setwinsize(24, resized_width)
                    terminal.screen.resize(24, resized_width)
                    resized = terminal.snapshot(f'{width}: resize with mutable Markdown')
                    assert 'draft🙂' in resized and 'int value = 1;' in resized, resized
                    chunks.put('\n' + ''.join(f'LIVE_ROW_{i:03d}\n' for i in range(50)))
                    growing = terminal.snapshot(f'{width}: tall preview stays inside viewport')
                    assert 'LIVE_ROW_049' in growing and 'draft🙂' in growing, growing
                    terminal.send('\x15' + 'LONG_DRAFT_' * 45 + 'DRAFT_END')
                    long_draft = terminal.snapshot(f'{width}: preview reserves wrapped draft rows')
                    assert 'LIVE_ROW_049' in long_draft and 'DRAFT_END' in long_draft, long_draft
                    terminal.send('\x15draft🙂')
                    chunks.put('```\n\nFINAL_PREVIEW')
                    tail = terminal.snapshot(f'{width}: code commits and last partial tail appears')
                    assert 'FINAL_PREVIEW' in tail and 'draft🙂' in tail, tail
                    chunks.put(None)
                    terminal.pump(.5)
                    completed = terminal.snapshot(f'{width}: final keeps one composer and tail')
                    assert completed.count('FINAL_PREVIEW') == 1, completed
                    assert 'draft🙂' in completed, completed
                    combined = collect_retained_history(terminal, 'draft🙂')
                    for marker in ('FIRST_PREVIEW', 'SECOND_PREVIEW', 'LIVE_TABLE',
                                   'int value = 1;', 'FINAL_PREVIEW',
                                   *(f'LIVE_ROW_{index:03d}' for index in range(50))):
                        assert combined.count(marker) == 1, (marker, combined)
                    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                    assert '\x1b[3J' not in terminal.raw, terminal.raw
                    if width == 80:
                        terminal.send('\x15cancel a partial answer\r')
                        _, chunks = terminal.request()
                        chunks.put('CANCEL_PREVIEW')
                        partial = terminal.snapshot('partial response is visible before cancellation')
                        assert partial.count('CANCEL_PREVIEW') == 1, partial
                        terminal.send('\x1b')
                        chunks.put(None)
                        terminal.pump(1)
                        cancelled = terminal.snapshot('cancellation commits the partial answer once')
                        assert cancelled.count('CANCEL_PREVIEW') == 1, cancelled
                finally:
                    if chunks is not None:
                        chunks.put(None)
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


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
                lines, columns = self.lines, self.columns
                self.__dict__.clear()
                self.__dict__.update(saved)
                self.resize(lines=lines, columns=columns)
            modes = tuple(mode for mode in modes if mode != 1049)
        super().reset_mode(*modes, **kwargs)


def collect_retained_history(terminal, draft, final_marker='FINAL_PREVIEW'):
    """Read logical history through overlapping viewports, retaining duplicates."""
    rows = terminal.screen.display
    composer = next(index for index, row in enumerate(rows) if draft in row)
    cursor = (terminal.screen.cursor.x, terminal.screen.cursor.y)
    history = rows[:composer - 2]
    previous = history

    def viewport(label):
        terminal.snapshot(label)
        assert draft in terminal.screen.display[composer], '\n'.join(terminal.screen.display)
        assert (terminal.screen.cursor.x, terminal.screen.cursor.y) == cursor
        return terminal.screen.display[:composer - 2]

    def prepend(page):
        nonlocal history
        overlap = next((count for count in range(len(page), 0, -1)
                        if page[-count:] == history[:count]), 0)
        assert overlap, 'scrolling skipped the overlapping history row'
        history = page[:-overlap] + history

    for pages in range(20):
        # PgUp moves one viewport; one wheel-down row creates a shared boundary.
        terminal.send('\x1b[5~\x1b[<65;1;1M')
        page = viewport('retained history page up with one-row overlap')
        if page == previous:
            break
        prepend(page)
        previous = page
    else:
        raise AssertionError('retained history did not reach its beginning')
    terminal.send('\x1b[<64;1;1M')
    first = viewport('retained history begins at the first physical row')
    if first != previous:
        prepend(first)
    terminal.send('\x1b[6~' * (pages + 2))
    bottom = viewport('retained history returns to the draft and final tail')
    assert any(final_marker in row for row in bottom), '\n'.join(bottom)
    return '\n'.join(history)


def check_details_follow(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width in (120, 80, 60):
            with tempfile.TemporaryDirectory(prefix='morph-details-follow-') as temp:
                terminal = Terminal(driver, Path(temp), server.server_port, hooks_enabled=False)
                terminal.child.setwinsize(24, width)
                terminal.screen = AlternateScreen(width, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                chunks = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('stream reasoning for scroll checks\r')
                    _, chunks = terminal.request()
                    chunks.put({'reasoning_content': ''.join(
                        f'DETAIL_ROW_{i:03d}\n' for i in range(60))})
                    terminal.send('kept draft\x14')
                    bottom = terminal.snapshot(f'{width}: details initially follow')
                    assert 'DETAIL_ROW_059' in bottom, bottom
                    terminal.send('\x1b[5~')
                    paused = terminal.snapshot(f'{width}: details pause above bottom')
                    chunks.put({'reasoning_content': 'DETAIL_PAUSED_OUTPUT\n'})
                    unchanged = terminal.snapshot(f'{width}: new reasoning preserves viewport')
                    assert unchanged == paused, unchanged
                    terminal.send('\x1b[6~' * 20)
                    resumed = terminal.snapshot(f'{width}: page down reaches bottom')
                    assert 'DETAIL_PAUSED_OUTPUT' in resumed, resumed
                    chunks.put({'reasoning_content': 'DETAIL_FOLLOW_RESUMED\n'})
                    followed = terminal.snapshot(f'{width}: details resume automatic following')
                    assert 'DETAIL_FOLLOW_RESUMED' in followed, followed
                    terminal.child.setwinsize(20, width)
                    terminal.screen.resize(20, width)
                    terminal.pump(.2)
                    chunks.put({'reasoning_content': 'DETAIL_FOLLOW_AFTER_RESIZE\n'})
                    resized = terminal.snapshot(f'{width}: resize keeps bottom following')
                    assert 'DETAIL_FOLLOW_AFTER_RESIZE' in resized, resized
                    terminal.send('\x1b')
                    assert 'kept draft' in terminal.snapshot(f'{width}: draft restored')
                finally:
                    if chunks is not None:
                        chunks.put(None)
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


def check_feed_replay(driver):
    fixture = Path(__file__).parent / 'fixtures' / 'cli_tool_feed.jsonl'
    for width in (100, 60, 40, 20, 12):
        child = pexpect.spawn(str(driver), ['--replay-events', str(fixture)],
                              encoding='utf-8', dimensions=(42, width))
        raw = ''
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                try:
                    raw += child.read_nonblocking(65536, timeout=.2)
                except pexpect.TIMEOUT:
                    if raw:
                        break
            screen = pyte.Screen(width, 42)
            pyte.Stream(screen).feed(raw)
            text = '\n'.join(screen.display)
            if width < 26:
                assert 'TIMEOUT' in text, text
                if width == 20:
                    assert 'TIMEOUT Exec' in text, text
                    assert 'Running…' in text, text
                else:
                    assert 'Widen term…' in text, text
                assert not any(row.strip() == 's' for row in screen.display), text
                assert '\x1b[2J' not in raw, raw
                print(f'checked: {width} columns: bounded tool states')
                continue
            assert sum(row.startswith(('● Exec ', '◉ Exec ', '✖ Exec ')) for row in screen.display) == 6, text
            assert '    $ ' not in text, text
            assert text.count('● Exec ') == 4 and text.count('✖ Exec ') == 1, text
            assert text.count('◉ Exec ') == 1, text
            assert text.count('  └ ') == 5, text
            assert text.count('Running…') == 1, text
            assert 'TIMEOUT' in text and '10.0s' in text, text
            reference = pyte.Screen(10, 2)
            pyte.Stream(reference).feed('\x1b[1;31m✖\x1b[0m \x1b[1mExec\r\n'
                                        '\x1b[0;36m◉\x1b[0m \x1b[1mExec')
            timeout_row = next(index for index, row in enumerate(screen.display)
                               if row.startswith('✖ Exec ') and 'TIMEOUT ·' in row)
            running_row = next(index for index, row in enumerate(screen.display)
                               if row.startswith('◉ Exec '))
            for actual_row, expected_row in ((timeout_row, 0), (running_row, 1)):
                assert screen.buffer[actual_row][0].fg == reference.buffer[expected_row][0].fg, text
                assert screen.buffer[actual_row][0].bold == reference.buffer[expected_row][0].bold, text
                assert all(screen.buffer[actual_row][column].bold for column in range(2, 6)), text
            assert '\x1b[2J' not in raw, raw
            assert all(len(row.rstrip()) <= width - 2 for row in screen.display), text
            if width == 100:
                assert '● Exec dig +short mcp.exa.ai  0.3s' in text, text
                assert '104.18.6.160, 104.18.7.160' in text, text
            print(f'checked: {width} columns: append feed replay')
        finally:
            child.close(force=True)


def check_tool_spacing(driver):
    events = []
    preambles = [
        ('PARAGRAPH_BEFORE_TOOL\n\n\n', False),
        ('- LIST_BEFORE_TOOL\n\n', True),
        ('```text\nCODE_FIRST\n\nCODE_LAST\n```\n\n', True),
        ('Wrapped content ' * 12 + 'WRAPPED_END', False),
    ]
    for index, (text, streaming) in enumerate(preambles):
        if index == 2:
            events.append({'name': 'react.reasoning.delta',
                           'data': {'text': 'REASONING_BETWEEN_TOOLS'}})
        if streaming:
            events.append({'name': 'react.content.delta', 'data': {'text': text}})
        events.append({'name': 'react.content.end', 'data': {'text': text}})
        events.append({'name': 'tool.call', 'data': {
            'tool': 'exec', 'tool_call_id': str(index),
            'args': {'command': f'printf TOOL_SPACING_{index}'}}})
        if index < len(preambles) - 1:
            events.append({'name': 'tool.result', 'data': {
                'tool': 'exec', 'tool_call_id': str(index),
                'result': json.dumps({'status': 'exited', 'exit_code': 0,
                                      'stdout': f'RESULT_{index}'})}})
    with tempfile.TemporaryDirectory(prefix='morph-tool-spacing-') as temp:
        fixture = Path(temp) / 'events.jsonl'
        fixture.write_text(''.join(json.dumps(event) + '\n' for event in events))
        for width in (120, 80, 60):
            child = pexpect.spawn(str(driver), ['--replay-events', str(fixture)],
                                  encoding='utf-8', dimensions=(48, width))
            try:
                child.expect('TOOL_SPACING_3')
                raw = child.before + child.after
                child.expect(pexpect.TIMEOUT, timeout=.2)
                raw += child.before
                screen = pyte.Screen(width, 48)
                pyte.Stream(screen).feed(raw)
                rows = screen.display
                text = '\n'.join(rows)
                for index in range(len(preambles)):
                    header = next(i for i, row in enumerate(rows)
                                  if f'Exec printf TOOL_SPACING_{index}' in row)
                    previous = max(i for i in range(header) if rows[i].strip())
                    assert header - previous == 2, text
                first = next(i for i, row in enumerate(rows) if 'CODE_FIRST' in row)
                last = next(i for i, row in enumerate(rows) if 'CODE_LAST' in row)
                assert last - first == 2, text
                assert 'WRAPPED_END' in text, text
                print(f'checked: {width} columns: content/tool boundary spacing')
            finally:
                child.close(force=True)


def check_agent_spacing(driver):
    events = []

    def narration(text, key):
        events.extend([
            {'name': 'react.reasoning.delta', 'data': {'text': text}},
            {'name': 'tool.call', 'data': {
                'tool': 'list_agents', 'tool_call_id': key, 'args': {}}},
            {'name': 'tool.result', 'data': {
                'tool': 'list_agents', 'tool_call_id': key, 'result': '{}'}},
        ])

    def completed(name):
        events.append({'name': 'agent.completed', 'data': {
            'parent_session_id': 0, 'agent_id': '/root/' + name,
            'status': 'completed'}})

    narration('BEFORE_AGENT_RECEIPT', 'before')
    completed('first-worker')
    narration('AFTER_AGENT_RECEIPT', 'after')
    events.extend([
        {'name': 'tool.call', 'data': {
            'tool': 'wait_agent', 'tool_call_id': 'wait', 'args': {'timeout_ms': 1}}},
        {'name': 'tool.result', 'data': {
            'tool': 'wait_agent', 'tool_call_id': 'wait', 'result': '{}'}},
        {'name': 'react.content.delta', 'data': {'text': 'PARENT_COMMENTARY'}},
    ])
    completed('deferred-worker')
    completed('batched-worker')
    events.append({'name': 'react.content.end', 'data': {'text': 'PARENT_COMMENTARY'}})
    narration('AFTER_DEFERRED_RECEIPT', 'end')
    with tempfile.TemporaryDirectory(prefix='morph-agent-spacing-') as temp:
        fixture = Path(temp) / 'events.jsonl'
        fixture.write_text(''.join(json.dumps(event) + '\n' for event in events))
        for width in (120, 80, 60):
            child = pexpect.spawn(str(driver), ['--replay-events', str(fixture)],
                                  encoding='utf-8', dimensions=(48, width))
            try:
                child.expect('AFTER_DEFERRED_RECEIPT')
                raw = child.before + child.after
                child.expect(pexpect.TIMEOUT, timeout=.2)
                raw += child.before
                screen = pyte.Screen(width, 48)
                pyte.Stream(screen).feed(raw)
                rows = screen.display
                text = '\n'.join(rows)
                previous = None
                for label in ('BEFORE_AGENT_RECEIPT', 'Finished /root/first-worker',
                              'AFTER_AGENT_RECEIPT', 'Finished waiting',
                              'PARENT_COMMENTARY', 'Finished /root/deferred-worker',
                              'Finished /root/batched-worker', 'AFTER_DEFERRED_RECEIPT'):
                    row = next(i for i, line in enumerate(rows) if label in line)
                    if previous is not None:
                        expected = 1 if 'batched-worker' in label else 2
                        assert row - previous == expected, text
                    assert not rows[row].startswith(' '), text
                    previous = row
                print(f'checked: {width} columns: reasoning/agent/wait component spacing')
            finally:
                child.close(force=True)


def check_plan_and_notice_spacing(driver):
    events = [{'name': 'react.content.end', 'data': {'text': 'BEFORE_PLAN'}}]
    events.append({'name': 'plan.updated', 'data': {'plans': [{'steps': [
        {'description': 'PLAN_DONE', 'status': 'completed'},
        {'description': 'PLAN_ACTIVE 中文🙂', 'status': 'in_progress'},
        {'description': 'PLAN_PENDING', 'status': 'pending'},
    ]}]}})
    events.extend([
        {'name': 'react.content.end', 'data': {'text': 'AFTER_PLAN'}},
        {'name': 'hook.failed', 'message': 'NOTICE_HOOK', 'data': {'event': 'PostToolUse'}},
        {'name': 'react.content.end', 'data': {'text': 'AFTER_HOOK'}},
        {'name': 'history.warning', 'phase': 'warning', 'message': 'NOTICE_WARNING'},
        {'name': 'react.content.end', 'data': {'text': 'AFTER_WARNING'}},
        {'name': 'react.compaction.completed', 'data': {'before_tokens': 1000, 'after_tokens': 100}},
        {'name': 'react.content.end', 'data': {'text': 'AFTER_COMPACTION'}},
        {'name': 'task.created', 'phase': 'end', 'message': 'NOTICE_TASK',
         'turn_active': False},
        {'name': 'react.content.end', 'data': {'text': 'AFTER_TASK'}, 'turn_active': True},
    ])
    with tempfile.TemporaryDirectory(prefix='morph-plan-spacing-') as temp:
        fixture = Path(temp) / 'events.jsonl'
        fixture.write_text(''.join(json.dumps(event) + '\n' for event in events))
        for width in (120, 80, 60):
            child = pexpect.spawn(str(driver), ['--replay-events', str(fixture)],
                                  encoding='utf-8', dimensions=(48, width))
            try:
                child.expect('AFTER_TASK')
                raw = child.before + child.after
                child.expect(pexpect.TIMEOUT, timeout=.2)
                raw += child.before
                screen = pyte.Screen(width, 48)
                pyte.Stream(screen).feed(raw)
                rows = screen.display
                text = '\n'.join(rows)
                labels = ('BEFORE_PLAN', '◉ Plan', 'PLAN_DONE', 'PLAN_ACTIVE', 'PLAN_PENDING',
                          'AFTER_PLAN', 'NOTICE_HOOK', 'AFTER_HOOK', 'NOTICE_WARNING',
                          'AFTER_WARNING', 'Context compacted', 'AFTER_COMPACTION',
                          'NOTICE_TASK', 'AFTER_TASK')
                previous = None
                for label in labels:
                    row = next(i for i, line in enumerate(rows) if label in line)
                    if previous is not None:
                        assert row - previous == (1 if label.startswith('PLAN_') else 2), text
                    previous = row
                for label, marker in (('NOTICE_HOOK', '✖'), ('NOTICE_WARNING', '⚠'),
                                      ('Context compacted', '●'), ('NOTICE_TASK', '●')):
                    row = next(line for line in rows if label in line)
                    assert row.startswith(marker + ' '), text
                assert '├ ● PLAN_DONE' in text and '└ ◯ PLAN_PENDING' in text, text
                assert '\x1b[2J' not in raw and '\x1b[3J' not in raw, raw
                print(f'checked: {width} columns: plan/notice widget boundaries and markers')
            finally:
                child.close(force=True)


def check_live_plan_draft(driver, port):
    for width in (120, 80, 60):
        with tempfile.TemporaryDirectory(prefix='morph-plan-draft-') as temp:
            terminal = Terminal(driver, Path(temp), port, hooks_enabled=False)
            releases = []
            try:
                terminal.child.setwinsize(24, width)
                terminal.screen = pyte.Screen(width, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                terminal.wait_for_composer()
                terminal.send('show a plan\r')
                _, release = terminal.request()
                releases.append(release)
                draft = 'DRAFT_' * (width // 5) + 'END'
                terminal.send(draft)
                before = terminal.snapshot(f'{width}: wrapped unsent draft before plan')
                prompt = max(i for i, row in enumerate(before.splitlines()) if row.startswith('› '))
                release.delta = {'tool_calls': [{'index': 0, 'id': 'plan-widget',
                    'type': 'function', 'function': {'name': 'update_plan',
                    'arguments': json.dumps({'plan': [
                        {'step': 'LIVE_PLAN_DONE', 'status': 'completed'},
                        {'step': 'LIVE_PLAN_ACTIVE', 'status': 'in_progress'},
                        {'step': 'LIVE_PLAN_PENDING', 'status': 'pending'},
                    ]})}}]}
                release.set()
                _, follow = terminal.request()
                releases.append(follow)
                after = terminal.snapshot(f'{width}: plan keeps wrapped unsent draft anchored')
                rows = after.splitlines()
                assert max(i for i, row in enumerate(rows) if row.startswith('› ')) == prompt, after
                assert 'END' in after and after.count('LIVE_PLAN_ACTIVE') == 1, after
                assert any(row.startswith('◉ Plan') for row in rows), after
                assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                follow.set()
                terminal.pump(.5)
            finally:
                for release in releases:
                    release.set()
                terminal.child.close(force=True)


def check_final_spacing(driver, port):
    replies = [
        'FINAL_SENTINEL plain answer.',
        'First paragraph.\n\nFINAL_SENTINEL final paragraph.\n\n\n',
        '```text\nfirst line\n\nFINAL_SENTINEL code line\n```\n\n',
        '- first item\n- FINAL_SENTINEL last item\n\n',
        '| key | value |\n| --- | --- |\n| final | FINAL_SENTINEL |\n\n',
    ]
    for index, reply in enumerate(replies):
        with tempfile.TemporaryDirectory(prefix='morph-final-spacing-') as temp:
            terminal = Terminal(driver, Path(temp), port, hooks_enabled=False)
            release = None
            try:
                width = (120, 80, 60, 80, 60)[index]
                terminal.child.setwinsize(24, width)
                terminal.screen = pyte.Screen(width, 24)
                terminal.stream = pyte.Stream(terminal.screen)
                terminal.pump(1)
                terminal.send('final spacing check\r')
                _, release = terminal.request()
                release.delta = {'content': reply}
                release.set()
                terminal.pump(.8)
                text = terminal.snapshot(f'final spacing case {index}')
                rows = text.splitlines()
                assert 'FINAL_SENTINEL' in text, text
                footer = next(i for i, row in enumerate(rows) if 'Worked for' in row)
                last_content = max(i for i, row in enumerate(rows[:footer]) if row.strip())
                assert footer - last_content == 2, text
                if index == 1:
                    assert rows[last_content - 1].strip() == '', text
                if index == 2:
                    first = next(i for i, row in enumerate(rows) if 'first line' in row)
                    last = next(i for i, row in enumerate(rows) if 'FINAL_SENTINEL code' in row)
                    assert last - first == 2, text
                prompt = max(i for i, row in enumerate(rows) if row.startswith('› '))
                assert prompt >= footer + 3, text
                assert terminal.screen.buffer[prompt - 1][0].bg == '3a3a3a', text
                for row in range(footer + 1, prompt - 1):
                    assert not rows[row].strip(), text
                    assert terminal.screen.buffer[row][0].bg != '3a3a3a', text
                terminal.send('/share\r')
                text = terminal.snapshot(f'share after final case {index}')
                rows = text.splitlines()
                footer = next(i for i, row in enumerate(rows) if 'Worked for' in row)
                command = next(i for i, row in enumerate(rows) if row.rstrip() == '/share')
                assert command == footer + 2, text
                receipt = next(i for i, row in enumerate(rows)
                               if 'saved conversation to ' in row)
                assert receipt == command + 2, text
                prompt = max(i for i, row in enumerate(rows) if row.startswith('› '))
                last_receipt = max(i for i in range(receipt, prompt)
                                   if rows[i].strip())
                assert prompt == last_receipt + 3, text
                assert terminal.screen.buffer[last_receipt + 1][0].bg != '3a3a3a', text
                assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            finally:
                if release:
                    release.set()
                terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    check_mutable_markdown_preview(driver)
    check_details_follow(driver)
    check_feed_replay(driver)
    check_tool_spacing(driver)
    check_agent_spacing(driver)
    check_plan_and_notice_spacing(driver)
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    check_final_spacing(driver, server.server_port)
    check_live_plan_draft(driver, server.server_port)
    with tempfile.TemporaryDirectory(prefix='morph-transcript-') as temp:
        directory = Path(temp)
        (directory / 'first.txt').write_text('FIRST_PRIVATE_RESULT\n', encoding='utf-8')
        second_content = ''.join(f'SECOND_LINE_{i:03d}\n' for i in range(200))
        second_content += 'SECOND_PRIVATE_RESULT 中文🙂\n'
        (directory / 'second.txt').write_text(second_content, encoding='utf-8')
        terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
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
            rows = [row for row in compact.splitlines() if '● file_read' in row]
            assert len(rows) == 2, compact
            assert all(row.startswith('● file_read') for row in rows), compact
            assert all(len(row.rstrip()) <= 118 for row in rows), compact
            assert all(not row[len('● file_read '):].startswith(' ')
                       for row in rows), compact
            assert 'FIRST_PRIVATE_RESULT' in compact, compact
            assert 'SECOND_PRIVATE_RESULT' not in compact, compact
            lines = compact.splitlines()
            working = next(row for row in lines if 'Working' in row)
            assert working.rstrip().endswith('Ctrl+T transcript'), compact
            assert '\x1b[38;5;141m · Ctrl+T transcript\x1b[0m' in terminal.raw
            assert all('Ctrl+T' not in row for row in lines
                       if row.startswith(('› ', '> '))), compact
            terminal.send('\x0f')
            assert 'Turn details' not in terminal.snapshot('old shortcut is unbound')
            first_tool = next(i for i, row in enumerate(lines) if '● file_read' in row)
            previous = max(i for i in range(first_tool) if lines[i].strip())
            assert first_tool - previous == 2, compact
            terminal.send('草稿🙂 tail\x1b[D')
            terminal.send('\x14')
            expanded = terminal.snapshot('expanded while model running')
            assert 'SECOND_PRIVATE_RESULT 中文🙂' in expanded, expanded
            assert 'Turn details' in expanded, expanded
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
            terminal.send('\x14')
            switches = terminal.raw.count('\x1b[?1049')
            clears = terminal.raw.count('\x1b[2J')
            final_release.delta = {'content': 'DETAILS_PREAMBLE', 'tool_calls': [
                {'index': 0, 'id': 'read-in-viewer', 'type': 'function',
                 'function': {'name': 'file_read', 'arguments': json.dumps({
                     'file_path': str(directory / 'first.txt')})}}]}
            final_release.set()
            _, last_release = terminal.request()
            releases.append(last_release)
            assert '3 calls' in terminal.snapshot('new tool result updates in place')
            last_release.set()
            terminal.pump(1)
            assert 'Turn details' in terminal.snapshot('details remain open after completion')
            assert terminal.raw.count('\x1b[?1049') == switches, terminal.raw
            assert terminal.raw.count('\x1b[2J') == clears, terminal.raw
            unchanged = terminal.raw
            terminal.pump(0.6)
            assert terminal.raw == unchanged, 'unchanged details should not repaint'
            terminal.send('\x14')
            completed = terminal.snapshot('finished process remains visible')
            assert completed.count('UPDATED ANSWER') == 1, completed
            assert 'file_read' in completed, completed
            assert 'succeeded' not in completed, completed
            assert '草稿🙂 taiXl' in completed, completed
            lines = completed.splitlines()
            preamble = next(i for i, row in enumerate(lines) if 'DETAILS_PREAMBLE' in row)
            tool = next(i for i in range(preamble + 1, len(lines))
                        if '● file_read' in lines[i])
            assert tool - preamble == 2, completed
            terminal.send('\x14')
            assert 'FIRST_PRIVATE_RESULT' in terminal.snapshot('expand after completion')
            terminal.send('\x14')
            assert 'SECOND_PRIVATE_RESULT' not in terminal.snapshot('collapse after completion')
            terminal.child.setwinsize(24, 36)
            terminal.screen.resize(24, 36)
            terminal.pump(0.2)
            terminal.send('\x14\x14')
            narrow = terminal.snapshot('narrow transcript toggles')
            assert '草稿🙂 taiXl' in narrow, narrow
            assert 'UPDATED ANSWER' in narrow, narrow
            assert terminal.raw.count('\x1b[?1049h') == 1, 'application owns its alternate screen'
            assert '\x1b[3J' not in terminal.raw, 'must preserve terminal scrollback'
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            terminal.send('\x15ask a question\r')
            _, ask_release = terminal.request()
            releases.append(ask_release)
            terminal.send('question draft\x14')
            ask_release.delta = {'tool_calls': [{'index': 0, 'id': 'ask-viewer',
                'type': 'function', 'function': {'name': 'ask_user',
                'arguments': json.dumps({'question': 'Choose a color',
                                         'choices': ['Blue', 'Red']})}}]}
            ask_release.set()
            terminal.pump(1)
            question = terminal.snapshot('question exits viewer without hiding input')
            assert 'Choose a color' in question, question
            assert 'Turn details' not in question, question
            terminal.send('1\r')
            answered, answered_release = terminal.request()
            releases.append(answered_release)
            assert any(m['role'] == 'tool' and 'Blue' in m['content']
                       for m in answered['messages']), answered
            assert 'question draft' in terminal.snapshot('draft restored after question')
            answered_release.set()
            terminal.pump(1)
            terminal.send('\x15/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=10)
            terminal.pump(.05)
            assert '\x1b[?1049l' in terminal.raw, 'shutdown must restore the main screen'
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            server.shutdown()


if __name__ == '__main__':
    main()
