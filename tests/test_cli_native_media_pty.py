"""Retained native media: managed logo, Markdown images/math and frame lifetime."""
from loopback_http import LoopbackHTTPServer
import http.server
import os
from pathlib import Path
import re
import struct
import sys
import tempfile
import threading
import time
import zlib

from test_cli_pty import Model, Terminal, pexpect, pyte
from test_cli_retained_chat_pty import wait_visible


class TextStream:
    """Feed text to pyte while retaining graphics/OSC bytes in Terminal.raw."""
    def __init__(self, screen):
        self.stream = pyte.Stream(screen)
        self.pending = ''

    def feed(self, data):
        self.pending += data
        while self.pending:
            start = re.search(r'\x1b[_\]]', self.pending)
            if start is None:
                keep = 1 if self.pending.endswith('\x1b') else 0
                self.stream.feed(self.pending[:-keep] if keep else self.pending)
                self.pending = self.pending[-keep:] if keep else ''
                return
            if start.start():
                self.stream.feed(self.pending[:start.start()])
                self.pending = self.pending[start.start():]
            end = self.pending.find('\x1b\\', 2)
            stop = end + 2 if end >= 0 else None
            if self.pending[1] == ']':
                bell = self.pending.find('\x07', 2)
                if bell >= 0 and (stop is None or bell < end):
                    stop = bell + 1
            if stop is None:
                return
            self.pending = self.pending[stop:]


def png(path, width, height, color):
    def chunk(name, data):
        return (struct.pack('!I', len(data)) + name + data +
                struct.pack('!I', zlib.crc32(name + data) & 0xffffffff))
    rows = b''.join(b'\0' + bytes(color) * width for _ in range(height))
    path.write_bytes(b'\x89PNG\r\n\x1a\n' +
                     chunk(b'IHDR', struct.pack('!IIBBBBB', width, height, 8, 6, 0, 0, 0)) +
                     chunk(b'IDAT', zlib.compress(rows)) + chunk(b'IEND', b''))


def packets(raw, protocol):
    if protocol == 'kitty':
        return re.findall(r'\x1b_Ga=p,[^;]*;\x1b\\', raw)
    return re.findall(r'\x1b\]1337;File=[^\x07]*\x07', raw)


def wait_packets(terminal, protocol, count=1, start=0):
    deadline = time.monotonic() + 8
    while len(packets(terminal.raw[start:], protocol)) < count:
        assert time.monotonic() < deadline, terminal.raw[-4000:]
        terminal.pump(.05)


def draft_row(terminal, text):
    return next(i for i, row in enumerate(terminal.screen.display) if text in row)


def run_case(binary, server, protocol):
    with tempfile.TemporaryDirectory(prefix='morph-native-media-') as temp:
        directory = Path(temp)
        first = directory / 'wide.png'
        second = directory / 'tall.png'
        video = directory / 'clip.mp4'
        png(first, 64, 32, (180, 70, 30, 255))
        png(second, 32, 64, (30, 130, 200, 255))
        video.write_bytes(b'local video fixture')
        binaries = directory / 'bin'
        binaries.mkdir()
        ffmpeg = binaries / 'ffmpeg'
        ffmpeg.write_text('#!/bin/sh\nfor target in "$@"; do :; done\n'
                          'cp "$MORPH_PTY_VIDEO_FRAME" "$target"\n')
        ffmpeg.chmod(0o755)
        environment = {'NO_COLOR': '', 'MORPH_PTY_VIDEO_FRAME': str(first),
                       'PATH': str(binaries) + os.pathsep + os.environ['PATH']}
        if protocol == 'kitty':
            environment.update(TERM='xterm-kitty', KITTY_WINDOW_ID='1')
        else:
            environment.update(TERM='xterm-256color', TERM_PROGRAM='iTerm.app',
                               ITERM_PROFILE='native-test')
        terminal = Terminal(binary, directory, server.server_port,
                            extra_env=environment, hooks_enabled=False)
        terminal.stream = TextStream(terminal.screen)
        releases = []
        try:
            terminal.wait_for_composer()
            wait_packets(terminal, protocol)
            assert terminal.screen.display[1].index('morph') == 5, '\n'.join(
                terminal.screen.display)
            if protocol == 'kitty':
                assert re.search(r'\x1b\[2;3H\x1b_Ga=p,[^;]*c=2,r=1,', terminal.raw)
            else:
                assert 'width=2;height=1;' in terminal.raw
            terminal.send('/clear\r')
            start = len(terminal.raw)
            terminal.send('/render ' + str(first) + '\r')
            wait_visible(terminal, 'image:')
            wait_packets(terminal, protocol, start=start)
            direct = packets(terminal.raw[start:], protocol)
            assert any(('c=40,r=10,' if protocol == 'kitty' else 'width=40;height=10;')
                       in packet for packet in direct), direct
            terminal.send('/clear\r')
            terminal.send('render native fixture\r')
            _, release = terminal.request()
            releases.append(release)
            terminal.send('protected native draft')
            start = len(terminal.raw)
            release.delta = {'content': (
                f'INLINE before ![wide]({first}) after image.\n\n'
                '| Picture | Description |\n|---|---|\n'
                f'| ![tall]({second}) | table image |\n\n'
                '$$x^2 + y^2 = z^2$$\n\n'
                f'![clip]({video})\n\n'
                f'[Open fixture](file://{first})\n\nMEDIA_DONE')}
            release.set()
            completed = wait_visible(terminal, 'MEDIA_DONE')
            assert 'protected native draft' in completed, completed
            assert 'morph-video-' not in completed, completed
            if protocol == 'kitty':
                assert 'x^2 + y^2 = z^2' not in completed, completed
            wait_visible(terminal, 'Worked for')
            wait_visible(terminal, 'Open video')
            cursor = (terminal.screen.cursor.y, terminal.screen.cursor.x)
            row = draft_row(terminal, 'protected native draft')
            terminal.send('\x1b[5~' * 10)
            scrolled = wait_visible(terminal, 'INLINE before')
            assert 'after image.' in scrolled, scrolled
            assert draft_row(terminal, 'protected native draft') == row, scrolled
            terminal.send('\x1b[6~' * 10)
            wait_visible(terminal, 'MEDIA_DONE')
            assert len(packets(terminal.raw[start:], protocol)) >= 2, terminal.raw[start:]
            if protocol == 'kitty':
                assert '\x1b_Ga=t,f=100,' in terminal.raw[start:]
                assert '\x1b_Ga=d,d=i,' in terminal.raw[start:]
            assert '\x1b]8;' in terminal.raw[start:]
            terminal.send('\x14')
            details = wait_visible(terminal, 'Turn details')
            assert 'protected native draft' not in details, details
            terminal.send('\x1b')
            returned = wait_visible(terminal, 'protected native draft')
            assert 'MEDIA_DONE' in returned, returned
            assert (terminal.screen.cursor.y, terminal.screen.cursor.x) == cursor
            terminal.child.setwinsize(24, 60)
            terminal.screen.resize(24, 60)
            wait_visible(terminal, 'protected native draft')
            terminal.send('\x1b[5~' * 10)
            wait_visible(terminal, 'INLINE before')
            terminal.send('\x1b[6~' * 10)
            wait_visible(terminal, 'MEDIA_DONE')
            clear_start = len(terminal.raw)
            terminal.send('\x15/clear\r')
            terminal.pump(.3)
            clear = '\n'.join(terminal.screen.display)
            assert 'MEDIA_DONE' not in clear and 'INLINE before' not in clear, clear
            if protocol == 'kitty':
                assert '\x1b_Ga=d,d=i,' in terminal.raw[clear_start:]
            terminal.pump(1.2)
            idle = len(packets(terminal.raw, protocol))
            terminal.pump(.5)
            assert len(packets(terminal.raw, protocol)) == idle
            terminal.send('/quit\r')
            terminal.child.expect(pexpect.EOF, timeout=8)
            print(f'PASS retained native media: {protocol}', flush=True)
        finally:
            for release in releases:
                release.set()
            terminal.child.close(force=True)
            artifacts = Path(tempfile.gettempdir()) / ('morph-native-pty-' + protocol)
            artifacts.mkdir(exist_ok=True)
            (artifacts / 'screen.txt').write_text('\n'.join(terminal.screen.display))
            (artifacts / 'transcript.ansi').write_text(terminal.raw)


def main():
    for key in ('TMUX', 'STY'):
        os.environ.pop(key, None)
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for protocol in ('kitty', 'iterm'):
            run_case(binary, server, protocol)
    finally:
        server.shutdown()
        server.server_close()


if __name__ == '__main__':
    main()
