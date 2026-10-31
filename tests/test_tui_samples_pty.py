"""Exercise the public TUI application API through real sample executables."""
import codecs
import fcntl
import os
import pty
import select
import signal
import struct
import subprocess
import sys
import termios
import time

try:
    import pyte
except ImportError:
    sys.exit(77)


class Sample:
    def __init__(self, binary):
        self.master, self.slave = pty.openpty()
        self.resize(24, 80)
        self.before = termios.tcgetattr(self.slave)
        self.screen = pyte.Screen(80, 24)
        self.stream = pyte.Stream(self.screen)
        self.decoder = codecs.getincrementaldecoder('utf-8')()
        self.child = subprocess.Popen([binary], stdin=self.slave, stdout=self.slave,
                                      stderr=self.slave, env={**os.environ, 'TERM': 'xterm-256color'})

    def resize(self, rows, columns):
        fcntl.ioctl(self.slave, termios.TIOCSWINSZ, struct.pack('HHHH', rows, columns, 0, 0))
        if hasattr(self, 'screen'):
            self.screen.resize(rows, columns)

    def pump(self):
        if select.select([self.master], [], [], .03)[0]:
            self.stream.feed(self.decoder.decode(os.read(self.master, 65536)))

    def until(self, predicate):
        deadline = time.monotonic() + 5
        while True:
            self.pump()
            text = '\n'.join(self.screen.display)
            if predicate(text) and 2026 << 5 not in self.screen.mode:
                return text
            assert self.child.poll() is None, text
            assert time.monotonic() < deadline, text

    def send(self, text):
        os.write(self.master, text.encode())

    def finish(self, key=None, number=None):
        if key is not None:
            self.send(key)
        if number is not None:
            self.child.send_signal(number)
        deadline = time.monotonic() + 5
        while self.child.poll() is None:
            self.pump()
            assert time.monotonic() < deadline, '\n'.join(self.screen.display)
        assert self.child.returncode == 0, self.child.returncode
        assert termios.tcgetattr(self.slave) == self.before, 'terminal modes were not restored'

    def close(self):
        if self.child.poll() is None:
            self.child.kill()
            self.child.wait()
        os.close(self.master)
        os.close(self.slave)


def minimal(binary):
    app = Sample(binary)
    try:
        app.until(lambda text: 'Selected: none' in text)
        app.send('j\r')
        app.until(lambda text: app.screen.display[-1].strip() == 'Beta')
        app.send('\x1b[200~qj\r\x1b[201~')
        app.send('\x1b[B\r')
        app.until(lambda text: app.screen.display[-1].strip() == 'Gamma')
        app.resize(8, 40)
        app.until(lambda text: app.screen.display[-1].strip() == 'Gamma' and
                  len(app.screen.display) == 8)
        app.finish('q')
    finally:
        app.close()
    for key, number in [('\x1b', None), ('\x03', None), (None, signal.SIGTERM)]:
        app = Sample(binary)
        try:
            app.until(lambda text: 'Selected: none' in text)
            app.finish(key, number)
        finally:
            app.close()


def controls(binary):
    app = Sample(binary)
    try:
        app.until(lambda text: 'morph-tui controls' in text)
        app.send('q你好')
        app.until(lambda text: app.screen.display[-1].strip() == 'q你好')
        app.send('\x1b[200~j\r\n\x1b[201~')
        app.until(lambda text: app.screen.display[-1].strip() == 'q你好j')
        app.send('\t\x1b[B\r')
        app.until(lambda text: 'Selection' in text and 'Beta' in text)
        app.send('\x1b')
        app.until(lambda text: 'morph-tui controls' in text)
        app.send('\r')
        app.until(lambda text: 'Selection' in text and 'Beta' in text)
        app.send('\x1b')
        app.until(lambda text: 'morph-tui controls' in text)
        app.send('\x1b[ZZ')
        app.until(lambda text: app.screen.display[-1].strip() == 'q你好jZ')
        app.finish('\x03')
    finally:
        app.close()


if __name__ == '__main__':
    minimal(sys.argv[1])
    controls(sys.argv[2])
    for binary in sys.argv[1:]:
        result = subprocess.run([binary], input=b'', stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        assert result.returncode != 0
        assert b'TUI application failed:' in result.stderr
    print('sample selection, focus, paste, Unicode, modal restoration, resize, and cleanup passed')
