"""Recover live previews and terminal receipts after runtime event overflow."""

import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading
import time

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal
from test_cli_subagents_pty import FeedModel
from test_cli_agents_pty import wait_for
from test_cli_sessions_pty import visible


def eventually(predicate, timeout=10):
    deadline = time.monotonic() + timeout
    while not predicate():
        assert time.monotonic() < deadline
        time.sleep(.01)


def main():
    server = LoopbackHTTPServer(('127.0.0.1', 0), FeedModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    replies = []
    try:
        with tempfile.TemporaryDirectory(prefix='morph-resync-pty-') as temporary:
            directory = Path(temporary)
            binaries = directory / 'bin'
            binaries.mkdir()
            started = directory / 'player-started'
            release_player = directory / 'player-release'
            player = binaries / 'mpv'
            player.write_text('#!/bin/sh\nprintf x >> "$RESYNC_STARTED"\n'
                              'while [ ! -e "$RESYNC_RELEASE" ]; do sleep .01; done\n')
            player.chmod(0o755)
            clip = directory / 'clip.mp4'
            clip.touch()
            terminal = Terminal(Path(sys.argv[1]).resolve(), directory, server.server_port,
                                production=True, hooks_enabled=False,
                                extra_env={'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
                                           'RESYNC_STARTED': str(started),
                                           'RESYNC_RELEASE': str(release_player)})
            try:
                terminal.wait_for_composer()
                terminal.send('/new ResyncAlpha\r')
                terminal.send('/new ResyncBeta\r')
                terminal.send('/switch ResyncAlpha\r')
                terminal.send('RESYNC_WORK\r')
                _, alpha = terminal.request()
                replies.append(alpha)
                alpha.stream({'content': 'RESYNC_INITIAL\n\n'})
                wait_for(terminal, lambda: 'RESYNC_INITIAL' in visible(terminal))
                terminal.send('RESYNC_DRAFT')
                terminal.send('\x1b[6;5~')

                def hold_player(count):
                    release_player.unlink(missing_ok=True)
                    terminal.child.send(f'/render {clip}\r')
                    eventually(lambda: started.exists() and len(started.read_text()) == count)

                hold_player(1)
                alpha.stream({'content': 'RESYNC_EARLY_GAP '})
                for _ in range(1200):
                    alpha.stream({'content': 'x'})
                alpha.stream({'content': ' RESYNC_MIDDLE'})
                eventually(alpha.chunks.empty)
                time.sleep(.3)
                release_player.touch()
                wait_for(terminal, lambda: 'video:' in visible(terminal))
                terminal.send('\x1b[5;5~')
                wait_for(terminal, lambda: 'RESYNC_MIDDLE' in visible(terminal))
                assert 'RESYNC_DRAFT' in visible(terminal), visible(terminal)
                for _ in range(10):
                    if 'RESYNC_EARLY_GAP' in visible(terminal):
                        break
                    terminal.send('\x1b[5~')
                wait_for(terminal, lambda: 'RESYNC_EARLY_GAP' in visible(terminal))
                for _ in range(10):
                    if 'RESYNC_MIDDLE' in visible(terminal):
                        break
                    terminal.send('\x1b[6~')
                wait_for(terminal, lambda: 'RESYNC_MIDDLE' in visible(terminal))
                alpha.stream({'content': '\n\nRESYNC_AFTER_RESUBSCRIBE'})
                wait_for(terminal, lambda: 'RESYNC_AFTER_RESUBSCRIBE' in visible(terminal))

                terminal.send('\x1b[6;5~')
                hold_player(2)
                for _ in range(1200):
                    alpha.stream({'content': 'y'})
                alpha.delta = {'content': '\n\nRESYNC_TERMINAL'}
                alpha.set()
                database = directory / 'home/.morph/data.db'

                def completed():
                    with sqlite3.connect(database) as connection:
                        row = connection.execute(
                            'SELECT t.status FROM runtime_turns t JOIN sessions s '
                            "ON s.id=t.session_id WHERE s.name='ResyncAlpha'").fetchone()
                        return row and row[0] == 'completed'

                eventually(completed)
                release_player.touch()
                terminal.pump(.5)
                terminal.send('\x1b[5;5~')
                wait_for(terminal, lambda: 'RESYNC_TERMINAL' in visible(terminal) and
                         'Working ·' not in visible(terminal))
                assert 'RESYNC_DRAFT' in visible(terminal), visible(terminal)
                terminal.send('\x15AFTER_OVERFLOW\r')
                body, after = terminal.request()
                replies.append(after)
                assert 'RESYNC_WORK' in str(body['messages']), body
                after.delta = {'content': 'RESYNC_RECOVERED'}
                after.set()
                wait_for(terminal, lambda: 'RESYNC_RECOVERED' in visible(terminal))
                terminal.send('\x04')
                terminal.child.expect(__import__('pexpect').EOF, timeout=15)
                terminal.child.close()
                assert terminal.child.exitstatus == 0
                print('PASS: overflow recovery restores full preview, draft, completion and subscription')
            finally:
                release_player.touch()
                for reply in replies:
                    reply.set()
                terminal.child.close(force=True)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
