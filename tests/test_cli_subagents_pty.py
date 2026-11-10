"""Inline child thread navigation, feed rendering and directed runtime actions."""
import http.server
import json
from pathlib import Path
import queue
import sys
import tempfile
import threading

from cli_inline_test_support import inline_panel, no_screen_switch, restored_composer
from test_cli_pty import Model, Terminal
from test_cli_agents_pty import tool_delta, wait_for


class Reply(threading.Event):
    def __init__(self):
        super().__init__()
        self.chunks = queue.Queue()
        self.delta = {}

    def stream(self, delta):
        self.chunks.put(delta)


class InlineModel(Model):
    """Each SSE fragment is controlled without implicit live text or expiry."""

    def do_POST(self):
        body = json.loads(self.rfile.read(int(self.headers['Content-Length'])))
        reply = Reply()
        if any(message.get('role') == 'system' and message.get('content', '').startswith(
                "Summarize the user's underlying task") for message in body['messages']):
            reply.delta = {'content': '{"title":"Inline agents"}'}
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
    terminal.pump(.08)


def panel(terminal, marker):
    return inline_panel(terminal, 'Subagents', marker, maximum=12,
                        command=terminal.subagents_command)


def find_content(terminal, marker, text):
    """Visit the top and each bounded viewport of the independent transcript."""
    press(terminal, '\x1b[H')
    for _ in range(24):
        rows = panel(terminal, marker)
        start = next(index for index, row in enumerate(terminal.screen.display)
                     if 'Subagents' in row)
        for index in range(start, len(terminal.screen.display)):
            row = terminal.screen.display[index]
            if text in row:
                return index, row.index(text)
        previous = '\n'.join(rows)
        press(terminal, '\x1b[6~')
        if '\n'.join(panel(terminal, marker)) == previous:
            break
    raise AssertionError(f'{text!r} missing from child transcript\n{visible(terminal)}')


def resize(terminal, marker, columns):
    terminal.child.setwinsize(40, columns)
    terminal.screen.resize(lines=40, columns=columns)
    wait_for(terminal, lambda: 'Subagents' in visible(terminal))
    panel(terminal, marker)


def run(driver, port, columns):
    releases = []
    with tempfile.TemporaryDirectory(prefix='morph-subagents-inline-') as temp:
        directory = Path(temp)
        (directory / 'child.txt').write_text('CHILD_TOOL_OUTPUT\nCHILD_TOOL_DETAIL_END\n')
        terminal = Terminal(driver, directory, port, hooks_enabled=False,
                            extra_config='[agents]\nmulti_agent_mode="proactive"\n'
                                         'max_total_tokens=0\n')
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
            root.set()
            pair = [terminal.request(), terminal.request()]
            releases.extend(reply for _, reply in pair)
            child = next(reply for body, reply in pair if any(
                'You are `/root/worker`' in message.get('content', '')
                for message in body['messages'] if message['role'] == 'system'))
            primary = next(reply for _, reply in pair if reply is not child)
            start = len(terminal.raw)
            terminal.subagents_command = '/subagents'
            press(terminal, '/subagents\r')
            wait_for(terminal, lambda: 'Main conversation' in visible(terminal))
            assert 'worker' in '\n'.join(panel(terminal, marker))
            for width in (60, 120, 80, columns):
                resize(terminal, marker, width)
            press(terminal, '\x1b[B\r')
            wait_for(terminal, lambda: 'Subagents / worker' in visible(terminal))
            find_content(terminal, marker, 'CHILD_ASSIGNMENT')

            child.stream({'reasoning_content': 'CHILD_REASONING_SENTINEL'})
            wait_for(terminal, lambda: 'CHILD_REASONING_SENTINEL' in visible(terminal))
            row, column = find_content(terminal, marker, 'CHILD_REASONING_SENTINEL')
            assert terminal.screen.buffer[row][column].italics, visible(terminal)
            assert 'Reasoning' not in '\n'.join(panel(terminal, marker))
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
            for width in (60, 120, 80, columns):
                resize(terminal, marker, width)
            child.delta = tool_delta('file_read', {
                'file_path': 'child.txt',
            }, 'read_child')
            child.set()
            _, child_final = terminal.request()
            releases.append(child_final)
            press(terminal, '\x1b[F')
            wait_for(terminal, lambda: 'child.txt' in '\n'.join(panel(terminal, marker)))
            compact = '\n'.join(panel(terminal, marker))
            assert '"file_path"' not in compact and '"ok"' not in compact, compact
            press(terminal, 'd')
            find_content(terminal, marker, 'CHILD_TOOL_DETAIL_END')
            no_screen_switch(terminal.raw[start:])
            press(terminal, '\x1b')
            wait_for(terminal, lambda: 'Subagents / worker' in visible(terminal))
            press(terminal, '\x0f')
            wait_for(terminal, lambda: 'Tool details' in '\n'.join(panel(terminal, marker)))
            find_content(terminal, marker, 'CHILD_TOOL_DETAIL_END')
            press(terminal, '\x1b')

            child_final.stream({'content': 'x' * 20000 + '\nCHILD_UTF8_TAIL 中文'})
            press(terminal, '\x1b[F')
            wait_for(terminal, lambda: 'CHILD_UTF8_TAIL 中文' in visible(terminal))
            assert 'completed' not in '\n'.join(panel(terminal, marker))
            child_final.set()
            wait_for(terminal, lambda: 'completed' in '\n'.join(panel(terminal, marker)))
            assert 'CHILD_UTF8_TAIL 中文' in visible(terminal)
            press(terminal, 'f尾部X\x7f')
            press(terminal, '\x1b[200~FOLLOWUP_DIRECTED_SENTINEL\n第二行\n')
            press(terminal, '\x1b')
            assert 'Follow up' in '\n'.join(panel(terminal, marker))
            assert Model.requests.empty(), 'paste sent before its end marker'
            press(terminal, '\x1b[201~')
            assert Model.requests.empty(), 'paste sent without an explicit Enter'
            for width in (60, 120, columns):
                resize(terminal, marker, width)
                assert 'Follow up' in '\n'.join(panel(terminal, marker))
            assert Model.requests.empty(), 'resize submitted a follow-up draft'
            press(terminal, '\r')
            followup, child_again = terminal.request()
            releases.append(child_again)
            assert any('尾部FOLLOWUP_DIRECTED_SENTINEL\n第二行' in message.get('content', '')
                       for message in followup['messages']), followup
            press(terminal, 'k')
            child_again.delta = {'content': 'INTERRUPTED_CHILD_ONLY'}
            child_again.set()
            wait_for(terminal, lambda: 'interrupted' in '\n'.join(panel(terminal, marker)))
            press(terminal, '\x1b')
            wait_for(terminal, lambda: 'Main conversation' in visible(terminal))
            listing = '\n'.join(panel(terminal, marker))
            for text in ('CHILD_HEADING', 'CHILD_BOLD', 'child_code', 'CHILD_UTF8_TAIL'):
                assert text not in listing, listing
            press(terminal, '\x1b[H\r')
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
            restored_composer(terminal, marker, 'PRESERVED_ROOT_DRAFT',
                              command=terminal.subagents_command)
            terminal.subagents_command = '/subagents worker'
            press(terminal, '\x15/subagents worker\r')
            wait_for(terminal, lambda: 'Subagents / worker' in visible(terminal))
            find_content(terminal, marker, 'CHILD_ASSIGNMENT')
            press(terminal, '\t\r')
            wait_for(terminal, lambda: 'Subagents' not in visible(terminal))
            restored_composer(terminal, marker, command=terminal.subagents_command)
            no_screen_switch(terminal.raw[start:])
            print(f'PASS: inline subagent feed, styles, details, streaming and actions at {columns}',
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
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), InlineModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for columns in (120, 80, 60):
            run(driver, server.server_port, columns)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
