"""Active streaming, process execution and approval survive repeated window resizing."""
from pathlib import Path
import queue
import sys
import tempfile
import threading

from cli_active_resize_fixture import ActiveResizeModel
from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal, pexpect
from test_cli_slash_pty import wait_for, editor_input


def visible(terminal):
    return '\n'.join(terminal.screen.display)


def finished(terminal, marker):
    wait_for(terminal, lambda: marker in visible(terminal) and
             'Working ·' not in visible(terminal))


def resize_burst(terminal):
    geometries = ((40, 120), (18, 60), (10, 40), (6, 20), (1, 1), (45, 160), (28, 80))
    for _ in range(3):
        for rows, columns in geometries:
            terminal.child.setwinsize(rows, columns)
            terminal.screen.resize(rows, columns)
            terminal.pump(.04)
            assert terminal.child.isalive(), terminal.raw[-4000:]
    terminal.pump(.2)


def main():
    server = LoopbackHTTPServer(('127.0.0.1', 0), ActiveResizeModel)
    server.requests = queue.Queue()
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-active-resize-') as temporary:
            directory = Path(temporary)
            terminal = Terminal(Path(sys.argv[1]).resolve(), directory, server.server_port,
                                production=True, hooks_enabled=False, extra_env={'NO_COLOR': ''})
            terminal.wait_for_composer()
            terminal.send('RESIZE_STREAM\r')
            wait_for(terminal, lambda: 'RESIZE_STREAM_START' in visible(terminal))
            terminal.send('RESIZE_UNSENT_DRAFT')
            resize_burst(terminal)
            finished(terminal, 'RESIZE_STREAM_DONE')
            editor_input(terminal, 'RESIZE_UNSENT_DRAFT')

            terminal.send('\x15/permissions mode full\r')
            wait_for(terminal, lambda: 'perm: full' in visible(terminal))
            terminal.send('RESIZE_TOOL\r')
            wait_for(terminal, lambda: 'Exec' in visible(terminal))
            terminal.send('RESIZE_TOOL_DRAFT')
            resize_burst(terminal)
            finished(terminal, 'RESIZE_TOOL_DONE')
            editor_input(terminal, 'RESIZE_TOOL_DRAFT')

            terminal.send('\x15/permissions mode ask\r')
            wait_for(terminal, lambda: 'perm: ask' in visible(terminal))
            terminal.send('RESIZE_APPROVAL\r')
            terminal.send('RESIZE_APPROVAL_DRAFT')
            wait_for(terminal, lambda: 'Enter confirm' in visible(terminal))
            resize_burst(terminal)
            terminal.send('\x1b[57353;1:1u\x1b[57353;1:3u')
            focused = [row.strip() for row in terminal.screen.display
                       if row.lstrip().startswith('› ●')]
            assert focused == ['› ● 2. Allow for this session'], visible(terminal)
            terminal.send('\x1b')
            finished(terminal, 'RESIZE_APPROVAL_DONE')
            assert not (directory / 'resize-approval.txt').exists()
            editor_input(terminal, 'RESIZE_APPROVAL_DRAFT')
            terminal.send('\r')
            finished(terminal, 'RESIZE_APPROVAL_DRAFT_DONE')
            requests = []
            while not server.requests.empty():
                requests.append(server.requests.get_nowait())
            assert any(body['messages'][-1].get('content') == 'RESIZE_APPROVAL_DRAFT'
                       for body in requests), 'resizing changed the restored draft'
            terminal.send('/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            terminal.child.close()
            assert terminal.child.exitstatus == 0, terminal.raw[-4000:]
            print('PASS: active streaming, tool execution, approval, draft and tiny resize bursts')
    finally:
        if terminal is not None:
            terminal.child.close(force=True)
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
