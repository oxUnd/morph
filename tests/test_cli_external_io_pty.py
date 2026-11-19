"""External GUI processes and local confirmations must preserve terminal ownership."""
from loopback_http import LoopbackHTTPServer
import base64
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import threading

from test_cli_pty import Model, Terminal, pexpect
from test_cli_slash_pty import wait_for
from test_cli_permissions_pty import composer
from test_cli_transcript_pty import AlternateScreen
import pyte


def grant_count(directory):
    with sqlite3.connect(directory / 'data.db') as database:
        return database.execute('SELECT count(*) FROM permission_grants').fetchone()[0]


def exercise(driver, port, width):
    with tempfile.TemporaryDirectory(prefix='morph-external-io-') as temporary:
        directory = Path(temporary)
        binaries = directory / 'bin'
        binaries.mkdir()
        player = binaries / 'mpv'
        player.write_text("#!/bin/sh\nif read answer; then exit 9; fi\n"
                          "printf '\\033[2JPLAYER_STDOUT'\n"
                          "printf '\\033[HPLAYER_STDERR' >&2\n"
                          'printf played >> "$EXTERNAL_IO_PLAYED"\n')
        player.chmod(0o755)
        editor = binaries / 'editor'
        editor.write_text("#!/bin/sh\nif read answer; then exit 9; fi\n"
                          "printf '\\033[2JEDITOR_STDERR' >&2\n"
                          "printf '{\"images\":[],\"bboxes\":[],\"arrows\":[]}'\n")
        editor.chmod(0o755)
        terminal = Terminal(
            driver, directory, port, hooks_enabled=False,
            extra_env={'PATH': str(binaries) + os.pathsep + os.environ['PATH'],
                       'EXTERNAL_IO_PLAYED': str(directory / 'played'),
                       'MORPH_EDITOR': str(editor)},
            extra_config='\n[permissions]\naccess = "full"\napproval = "never"\n'
                         'reviewer = "user"\n')
        terminal.child.setwinsize(32, width)
        terminal.screen = AlternateScreen(width, 32)
        terminal.stream = pyte.Stream(terminal.screen)
        release = None
        try:
            terminal.wait_for_composer()
            clip = directory / 'sample.mp4'
            clip.touch()
            for command, receipt in ((f'/render {clip}', 'video: '),
                                     (str(clip), 'video loaded: ')):
                terminal.send(command + '\r')
                wait_for(terminal, lambda: receipt in '\n'.join(terminal.screen.display))
                composer(terminal)
            assert (directory / 'played').read_text() == 'playedplayed'
            assert 'PLAYER_STDOUT' not in terminal.raw and 'PLAYER_STDERR' not in terminal.raw
            with sqlite3.connect(directory / 'data.db') as database:
                database.execute(
                    'INSERT INTO permission_grants'
                    '(subject,resource_kind,resource,project_root,created_at) '
                    "VALUES('fixture','program','fixture','/other-project',1)")
            for answer in ('\r', '\x1b', '\x1b[B\r'):
                terminal.send('/permissions clear\r')
                wait_for(terminal, lambda: 'Confirm this action?' in
                         '\n'.join(terminal.screen.display))
                terminal.send(answer)
                composer(terminal)
                assert grant_count(directory) == 1
            terminal.send('/permissions clear --all-projects\r')
            wait_for(terminal, lambda: 'Type "clear all" to confirm' in
                     '\n'.join(terminal.screen.display))
            terminal.send('wrong\r')
            composer(terminal)
            assert grant_count(directory) == 1
            terminal.send('/permissions clear --all-projects\r')
            wait_for(terminal, lambda: 'Type "clear all" to confirm' in
                     '\n'.join(terminal.screen.display))
            terminal.send('clear all\r')
            composer(terminal)
            assert grant_count(directory) == 0
            image = directory / 'image.png'
            image.write_bytes(base64.b64decode(
                'iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAQAAAC1HAwCAAAAC0lEQVR42mP8/'
                'x8AAwMCAO+jRZkAAAAASUVORK5CYII='))
            terminal.send('Annotate the fixture image\r')
            _, release = terminal.request()
            release.delta = {'tool_calls': [{
                'index': 0, 'id': 'annotate-io', 'type': 'function',
                'function': {'name': 'img_annotate',
                             'arguments': json.dumps({'path': str(image)})}}]}
            terminal.send('draft中文')
            release.set()
            body, release = terminal.request()
            result = next(message['content'] for message in reversed(body['messages'])
                          if message['role'] == 'tool')
            assert 'bboxes' in result, result
            release.delta = {'content': 'ANNOTATION_COMPLETE'}
            release.set()
            wait_for(terminal, lambda: 'ANNOTATION_COMPLETE' in
                     '\n'.join(terminal.screen.display))
            composer(terminal, 'draft中文')
            assert 'EDITOR_STDERR' not in terminal.raw
            terminal.send('\x15/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            print(f'PASS: player/editor streams, native confirmations and drafts at {width} columns')
        except Exception:
            print('\n'.join(terminal.screen.display))
            raise
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)


def main():
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width in (120, 80, 60):
            exercise(Path(sys.argv[1]).resolve(), server.server_port, width)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
