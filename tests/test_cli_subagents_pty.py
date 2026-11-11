"""Composer agent selection, independent full-screen feeds and directed actions."""
import copy
import http.server
import json
from pathlib import Path
import queue
import sys
import tempfile
import threading

from cli_inline_test_support import inline_panel, no_screen_switch, restored_composer, stable_rows
from test_cli_pty import Model, Terminal
from test_cli_agents_pty import tool_delta, wait_for
import pyte


class AlternateScreen(pyte.Screen):
    """Keep the primary screen intact when the child feed uses xterm mode 1049."""

    def set_mode(self, *modes, **kwargs):
        if kwargs.get('private') and 1049 in modes:
            assert not hasattr(self, '_main_screen'), 'nested alternate-screen entry'
            saved = copy.deepcopy(self.__dict__)
            self.reset()
            self._main_screen = saved
            modes = tuple(mode for mode in modes if mode != 1049)
        super().set_mode(*modes, **kwargs)

    def reset_mode(self, *modes, **kwargs):
        if kwargs.get('private') and 1049 in modes:
            saved = getattr(self, '_main_screen', None)
            assert saved is not None, 'alternate-screen exit without entry'
            self.__dict__.clear()
            self.__dict__.update(saved)
            modes = tuple(mode for mode in modes if mode != 1049)
        super().reset_mode(*modes, **kwargs)


class Reply(threading.Event):
    def __init__(self):
        super().__init__()
        self.chunks = queue.Queue()
        self.delta = {}

    def stream(self, delta):
        self.chunks.put(delta)


class FeedModel(Model):
    """Each SSE fragment is controlled without implicit live text or expiry."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        reply = Reply()
        if any(message.get('role') == 'system' and message.get('content', '').startswith(
                "Summarize the user's underlying task") for message in body['messages']):
            reply.delta = {'content': '{"title":"Independent agents"}'}
            reply.set()
        else:
            Model.requests.put((body, reply))
        self.send_response(200)
        self.send_header('Content-Type', 'text/event-stream')
        self.end_headers()

        def write(delta):
            event = {'choices': [{'index': 0, 'delta': delta, 'finish_reason': None}]}
            self.wfile.write(('data: ' + json.dumps(event) + '\n\n').encode())
            self.wfile.flush()

        try:
            self.wfile.write(b': ready\n\n')
            self.wfile.flush()
            while not reply.is_set():
                try:
                    write(reply.chunks.get(timeout=.05))
                except queue.Empty:
                    pass
            while not reply.chunks.empty():
                write(reply.chunks.get_nowait())
            if reply.delta:
                write(reply.delta)
            self.wfile.write(b'data: {"choices":[{"index":0,"delta":{},'
                             b'"finish_reason":"stop"}]}\n\ndata: [DONE]\n\n')
            self.wfile.flush()
        except (BrokenPipeError, ConnectionResetError):
            pass


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def press(terminal, text):
    terminal.child.send(text)
    # A lone Escape needs to outlast the terminal parser's 80 ms sequence timeout.
    terminal.pump(.16 if text.endswith('\x1b') else .08)
    if 2026 << 5 in terminal.screen.mode:
        stable_rows(terminal)


def panel(terminal, marker):
    rows = inline_panel(terminal, 'Subagents', marker, maximum=12,
                        command=terminal.subagents_command)
    assert len(rows) <= 8, '\n'.join(rows)
    selected = next(index for index, row in enumerate(terminal.screen.display)
                    if row.startswith('>'))
    assert terminal.screen.buffer[selected][terminal.screen.columns - 2].bg == 'blue', (
        '\n'.join(terminal.screen.display))
    assert not hasattr(terminal.screen, '_main_screen'), 'selector changed screen buffers'
    return rows


def feed(terminal, marker):
    rows = stable_rows(terminal)
    diagnostic = '\n'.join(rows)
    assert hasattr(terminal.screen, '_main_screen'), diagnostic
    assert 'Subagents · /root/worker' in rows[0], diagnostic
    assert marker not in diagnostic, diagnostic
    assert 'Esc' in rows[-1], diagnostic
    assert not any(row.lstrip().startswith('morph ·') for row in rows), diagnostic
    return rows


def find_content(terminal, marker, text):
    """Visit the top and each full-screen viewport of the child transcript."""
    press(terminal, '\x1b[H')
    for _ in range(24):
        rows = feed(terminal, marker)
        for index, row in enumerate(rows):
            if text in row:
                return index, row.index(text)
        previous = '\n'.join(rows)
        press(terminal, '\x1b[6~')
        if '\n'.join(feed(terminal, marker)) == previous:
            break
    raise AssertionError(f'{text!r} missing from child transcript\n{visible(terminal)}')


def resize(terminal, marker, columns):
    terminal.child.setwinsize(40, columns)
    terminal.screen.resize(lines=40, columns=columns)
    wait_for(terminal, lambda: 'Subagents' in visible(terminal))
    if hasattr(terminal.screen, '_main_screen'):
        feed(terminal, marker)
    else:
        panel(terminal, marker)


def run(driver, port, columns):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-subagents-feed-') as temp:
        directory = Path(temp)
        (directory / 'child.txt').write_text('CHILD_TOOL_OUTPUT\nCHILD_TOOL_DETAIL_END\n')
        terminal = Terminal(driver, directory, port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n'
                                         'max_total_tokens=0\n')
        terminal.screen = AlternateScreen(80, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            terminal.child.setwinsize(40, columns)
            terminal.screen.resize(lines=40, columns=columns)
            marker = f'ROOT_FEED_SENTINEL_{columns}'
            terminal.send('Keep the parent conversation visible\r')
            _, seed = terminal.request()
            releases.append(seed)
            seed.delta = {'content': marker + ' visible/' * 9}
            seed.set()
            wait_for(terminal, lambda: marker in visible(terminal)
                     and 'Working ·' not in visible(terminal))
            terminal.send('inspect independent thread\r')
            _, root = terminal.request()
            releases.append(root)
            root.delta = tool_delta('spawn_agent', {
                'task_name': 'worker', 'fork_turns': 'none',
                'message': 'CHILD_ASSIGNMENT: inspect child.txt independently',
            }, 'spawn_worker')
            root.delta['tool_calls'].extend(tool_delta('spawn_agent', {
                'task_name': 'observer', 'fork_turns': 'none',
                'message': 'OBSERVER_ASSIGNMENT: inspect a separate scope',
            }, 'spawn_observer')['tool_calls'])
            root.delta['tool_calls'][1]['index'] = 1
            root.set()
            pair = [terminal.request(), terminal.request(), terminal.request()]
            releases.extend(reply for _, reply in pair)
            child = next(reply for body, reply in pair if any(
                'You are `/root/worker`' in message.get('content', '')
                for message in body['messages'] if message['role'] == 'system'))
            observer = next(reply for body, reply in pair if any(
                'You are `/root/observer`' in message.get('content', '')
                for message in body['messages'] if message['role'] == 'system'))
            primary = next(reply for _, reply in pair if reply not in (child, observer))
            child.stream({'content': 'CHILD_SNAPSHOT_ONCE\n\n'})
            observer.stream({'content': 'OBSERVER_SEPARATE_FEED\n\n'})
            terminal.pump(.3)
            start = len(terminal.raw)
            terminal.subagents_command = '/subagents'
            press(terminal, '/subagents\r')
            wait_for(terminal, lambda: 'Main conversation' in visible(terminal))
            assert 'worker' in '\n'.join(panel(terminal, marker))
            for width in (60, 120, 80, columns):
                resize(terminal, marker, width)
            press(terminal, '\t')
            assert 'Main conversation' in '\n'.join(panel(terminal, marker))
            assert 'Subagents ·' not in visible(terminal), visible(terminal)
            press(terminal, '\t')
            assert 'Subagents ·' not in visible(terminal), visible(terminal)
            no_screen_switch(terminal.raw[start:])
            choices = [row.lstrip('> ') for row in stable_rows(terminal)
                       if row.lstrip('> ').startswith('/root')]
            worker_index = next(index for index, row in enumerate(choices)
                                if row.startswith('/root/worker '))
            observer_index = next(index for index, row in enumerate(choices)
                                  if row.startswith('/root/observer '))
            to_observer = '\t' if worker_index < observer_index else '\x1b[Z'
            to_worker = '\x1b[Z' if worker_index < observer_index else '\t'
            to_main = '\x1b[Z' if worker_index == 1 else '\t'
            press(terminal, '\x1b[H' + '\x1b[B' * worker_index + '\r')
            wait_for(terminal, lambda: 'Subagents · /root/worker' in visible(terminal))
            row, _ = find_content(terminal, marker, 'CHILD_ASSIGNMENT')
            assert terminal.screen.buffer[row][columns - 3].bg == '3a3a3a', visible(terminal)
            assert visible(terminal).count('CHILD_SNAPSHOT_ONCE') == 1, visible(terminal)
            child.stream({'content': 'CHILD_LIVE_ONCE\n\n'})
            wait_for(terminal, lambda: 'CHILD_LIVE_ONCE' in visible(terminal))
            assert visible(terminal).count('CHILD_SNAPSHOT_ONCE') == 1, visible(terminal)
            assert visible(terminal).count('CHILD_LIVE_ONCE') == 1, visible(terminal)
            press(terminal, 'fWORKER_SWITCH_DRAFT\x1b')
            press(terminal, to_observer)
            wait_for(terminal, lambda: 'Subagents · /root/observer' in visible(terminal))
            assert 'OBSERVER_SEPARATE_FEED' in visible(terminal), visible(terminal)
            assert 'CHILD_SNAPSHOT_ONCE' not in visible(terminal), visible(terminal)
            press(terminal, 'fOBSERVER_SWITCH_DRAFT\x1b')
            press(terminal, to_worker)
            wait_for(terminal, lambda: 'Subagents · /root/worker' in visible(terminal))
            assert 'OBSERVER_SEPARATE_FEED' not in visible(terminal), visible(terminal)
            press(terminal, 'f')
            assert 'Follow up › WORKER_SWITCH_DRAFT' in visible(terminal), visible(terminal)
            press(terminal, '\x7f' * len('WORKER_SWITCH_DRAFT') + '\x1b')
            press(terminal, to_observer)
            wait_for(terminal, lambda: 'Subagents · /root/observer' in visible(terminal))
            press(terminal, 'f')
            assert 'Follow up › OBSERVER_SWITCH_DRAFT' in visible(terminal), visible(terminal)
            press(terminal, '\x1b')
            press(terminal, to_worker)
            wait_for(terminal, lambda: 'Subagents · /root/worker' in visible(terminal))
            observer.delta = {'content': 'OBSERVER_COMPLETED'}
            observer.set()
            primary.stream({'content': 'ROOT_DEFERRED_SENTINEL\n\n'})
            terminal.pump(.4)
            assert 'ROOT_DEFERRED_SENTINEL' not in visible(terminal), visible(terminal)
            assert 'ROOT_DEFERRED_SENTINEL' not in terminal.raw[start:], terminal.raw[start:]

            child.stream({'reasoning_content': 'CHILD_REASONING_SENTINEL'})
            wait_for(terminal, lambda: 'CHILD_REASONING_SENTINEL' in visible(terminal))
            row, column = find_content(terminal, marker, 'CHILD_REASONING_SENTINEL')
            assert terminal.screen.buffer[row][column].italics, visible(terminal)
            assert 'Reasoning' not in '\n'.join(feed(terminal, marker))
            child.stream({'content': '## CHILD_HEADING\n\n**CHILD_BOLD**\n\n- CHILD_ITEM\n'})
            press(terminal, '\x1b[F')
            wait_for(terminal, lambda: 'CHILD_HEADING' in visible(terminal))
            row, column = find_content(terminal, marker, 'CHILD_HEADING')
            assert terminal.screen.buffer[row][column].bold, visible(terminal)
            assert '●' in '\n'.join(terminal.screen.display[max(row - 1, 0):row + 1]), visible(terminal)
            assert '## CHILD_HEADING' not in visible(terminal)
            row, column = find_content(terminal, marker, 'CHILD_BOLD')
            assert terminal.screen.buffer[row][column].bold, visible(terminal)
            assert '**CHILD_BOLD**' not in visible(terminal)
            find_content(terminal, marker, 'CHILD_ITEM')
            press(terminal, '\x1b[F')
            child.stream({'content': '\n```c\nint child_code = 7;\n```\n\nCHILD_STREAM_TAIL'})
            wait_for(terminal, lambda: 'CHILD_STREAM_TAIL' in visible(terminal))
            row, column = find_content(terminal, marker, 'int child_code = 7;')
            colors = {terminal.screen.buffer[row][index].fg
                      for index in range(column, column + 19)
                      if terminal.screen.buffer[row][index].data.strip()}
            assert len(colors) > 1, visible(terminal)
            assert '```' not in visible(terminal)
            child.stream({'content': '\n\n' + '\n\n'.join(
                f'CHILD_VIEWPORT_ROW_{index:03d}' for index in range(45)) + '\n\n'})
            wait_for(terminal, lambda: 'CHILD_VIEWPORT_ROW_044' in visible(terminal))
            rows = feed(terminal, marker)
            occupied = [index for index, row in enumerate(rows) if 'CHILD_VIEWPORT_ROW_' in row]
            assert min(occupied) < 10 and max(occupied) >= len(rows) - 7, '\n'.join(rows)
            press(terminal, '\x1b[5~')
            paused = feed(terminal, marker)[3:-2]
            child.stream({'content': 'CHILD_PAUSED_OUTPUT\n\n'})
            terminal.pump(.7)
            assert feed(terminal, marker)[3:-2] == paused, visible(terminal)
            assert 'CHILD_PAUSED_OUTPUT' not in visible(terminal), visible(terminal)
            press(terminal, '\x1b[6~' * 20)
            wait_for(terminal, lambda: 'CHILD_PAUSED_OUTPUT' in visible(terminal))
            child.stream({'content': 'CHILD_RESUMED_FOLLOW\n\n'})
            wait_for(terminal, lambda: 'CHILD_RESUMED_FOLLOW' in visible(terminal))
            for width in (60, 120, 80, columns):
                resize(terminal, marker, width)
            child.delta = tool_delta('file_read', {
                'file_path': 'child.txt',
            }, 'read_child')
            child.set()
            _, child_final = terminal.request()
            releases.append(child_final)
            press(terminal, '\x1b[F')
            wait_for(terminal, lambda: 'child.txt' in '\n'.join(feed(terminal, marker)))
            compact = '\n'.join(feed(terminal, marker))
            assert '"file_path"' not in compact and '"ok"' not in compact, compact
            press(terminal, 'd')
            find_content(terminal, marker, 'CHILD_TOOL_DETAIL_END')
            switches = terminal.raw.count('\x1b[?1049')
            press(terminal, 'd')
            assert 'Tool details' not in visible(terminal), visible(terminal)
            press(terminal, '\x14')
            wait_for(terminal, lambda: 'Tool details' in '\n'.join(feed(terminal, marker)))
            find_content(terminal, marker, 'CHILD_TOOL_DETAIL_END')
            assert terminal.raw.count('\x1b[?1049') == switches, terminal.raw[start:]
            press(terminal, 'fKEPT_CHILD_DRAFT')
            assert 'Follow up › KEPT_CHILD_DRAFT' in visible(terminal), visible(terminal)
            press(terminal, '\x1b')
            assert 'Follow up ›' not in visible(terminal), visible(terminal)
            cached_body = feed(terminal, marker)[3:-2]
            press(terminal, '\x1b')
            wait_for(terminal, lambda: 'Subagents' not in visible(terminal))
            assert not hasattr(terminal.screen, '_main_screen'), visible(terminal)
            wait_for(terminal, lambda: 'ROOT_DEFERRED_SENTINEL' in visible(terminal))
            restored_composer(terminal, marker, command=terminal.subagents_command)
            assert 'CHILD_TOOL_DETAIL_END' not in visible(terminal), visible(terminal)
            terminal.subagents_command = '/subagents worker'
            press(terminal, '/subagents worker\r')
            wait_for(terminal, lambda: 'Subagents · /root/worker' in visible(terminal))
            assert 'Tool details' in visible(terminal), visible(terminal)
            assert feed(terminal, marker)[3:-2] == cached_body, visible(terminal)
            press(terminal, 'f')
            assert 'Follow up › KEPT_CHILD_DRAFT' in visible(terminal), visible(terminal)
            press(terminal, '\x7f' * len('KEPT_CHILD_DRAFT') + '\x1b')

            child_final.stream({'content': 'x' * 20000 + '\nCHILD_UTF8_TAIL 中文'})
            press(terminal, '\x1b[F')
            wait_for(terminal, lambda: 'CHILD_UTF8_TAIL 中文' in visible(terminal))
            assert 'completed' not in '\n'.join(feed(terminal, marker))
            child_final.set()
            wait_for(terminal, lambda: 'completed' in '\n'.join(feed(terminal, marker)))
            assert 'CHILD_UTF8_TAIL 中文' in visible(terminal)
            press(terminal, 'f尾部X\x7f')
            press(terminal, '\x1b[200~FOLLOWUP_DIRECTED_SENTINEL\n第二行\n')
            press(terminal, '\x1b')
            assert 'Follow up' in '\n'.join(feed(terminal, marker))
            assert Model.requests.empty(), 'paste sent before its end marker'
            press(terminal, '\x1b[201~')
            assert Model.requests.empty(), 'paste sent without an explicit Enter'
            for width in (60, 120, columns):
                resize(terminal, marker, width)
                assert 'Follow up' in '\n'.join(feed(terminal, marker))
            assert Model.requests.empty(), 'resize submitted a follow-up draft'
            press(terminal, '\r')
            followup, child_again = terminal.request()
            releases.append(child_again)
            assert any('尾部FOLLOWUP_DIRECTED_SENTINEL\n第二行' in message.get('content', '')
                       for message in followup['messages']), followup
            press(terminal, 'k')
            child_again.delta = {'content': 'INTERRUPTED_CHILD_ONLY'}
            child_again.set()
            wait_for(terminal, lambda: 'interrupted' in '\n'.join(feed(terminal, marker)))
            press(terminal, '\x1b')
            wait_for(terminal, lambda: 'Subagents' not in visible(terminal))
            restored_composer(terminal, marker, command=terminal.subagents_command)
            for text in ('CHILD_UTF8_TAIL', 'CHILD_TOOL_OUTPUT', 'CHILD_HEADING'):
                assert text not in visible(terminal), visible(terminal)
            press(terminal, 'PRESERVED_ROOT_DRAFT')
            primary.delta = {'content': 'ROOT_INTEGRATED_SENTINEL'}
            primary.set()
            _, integrated = terminal.request()
            releases.append(integrated)
            integrated.delta = {'content': 'ROOT_VERIFIED_FINAL'}
            integrated.set()
            wait_for(terminal, lambda: 'ROOT_VERIFIED_FINAL' in terminal.raw
                     and 'Working ·' not in visible(terminal))
            restored_composer(terminal, 'ROOT_VERIFIED_FINAL', 'PRESERVED_ROOT_DRAFT',
                              command=terminal.subagents_command)
            terminal.subagents_command = '/subagents worker'
            press(terminal, '\x15/subagents worker\r')
            wait_for(terminal, lambda: 'Subagents · /root/worker' in visible(terminal))
            find_content(terminal, marker, 'CHILD_ASSIGNMENT')
            assert visible(terminal).count('CHILD_SNAPSHOT_ONCE') == 1, visible(terminal)
            assert visible(terminal).count('CHILD_LIVE_ONCE') == 1, visible(terminal)
            press(terminal, to_main)
            wait_for(terminal, lambda: 'Subagents' not in visible(terminal))
            restored_composer(terminal, 'ROOT_VERIFIED_FINAL',
                              command=terminal.subagents_command)
            assert terminal.raw[start:].count('\x1b[?1049h') == 3, terminal.raw[start:]
            assert terminal.raw[start:].count('\x1b[?1049l') == 3, terminal.raw[start:]
            assert '\x1b[3J' not in terminal.raw[start:], 'agent switch erased scrollback'
            print(f'PASS: independent subagent feed, styles, details, streaming and actions at {columns}',
                  flush=True)
        except Exception:
            print(visible(terminal), flush=True)
            raise
        finally:
            for reply in releases:
                reply.set()
            terminal.child.close(force=True)
            while not Model.requests.empty():
                _, reply = Model.requests.get_nowait()
                reply.set()


def main():
    driver = Path(sys.argv[1]).resolve()
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), FeedModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (200, 120, 80, 60):
            run(driver, server.server_port, columns)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
