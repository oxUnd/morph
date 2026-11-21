"""First-run setup form: real PTY, standard controls, isolated credentials."""
from nbase_status import native_status
import os
from pathlib import Path
import re
import signal
import sys
import tempfile
import time

try:
    import pexpect
    import pyte
except ImportError:
    print('Setup PTY tests require pexpect and pyte')
    sys.exit(77)


TAB = '\t'
ENTER = '\r'
ESC = '\x1b'
CTRL_C = '\x03'
CTRL_U = '\x15'
RIGHT = '\x1b[C'
DOWN = '\x1b[B'


class Terminal:
    def __init__(self, binary, path, ready=False, size=(24, 80), flags=(),
                 production=False):
        env = {k: v for k, v in os.environ.items()
               if not k.endswith('API_KEY') and k != 'NO_COLOR'}
        env.update(TERM='xterm-256color', HOME=str(path.parent))
        if ready:
            env.update(OPENAI_API_KEY='fixture-openai',
                       DEEPSEEK_API_KEY='fixture-deepseek',
                       VOLCENGINE_API_KEY='fixture-ark')
        self.path = path
        self.production = production
        arguments = (["setup", "-c", str(path)] if production else [str(path)])
        self.child = pexpect.spawn(str(binary), arguments + list(flags), env=env,
                                   encoding='utf-8', dimensions=size, timeout=10)
        self.screen = pyte.HistoryScreen(size[1], size[0], history=4000)
        self.stream = pyte.Stream(self.screen)
        self.transcript = ''
        self.pump(0.8)
        self.wait('Setup')

    def pump(self, seconds=0.3):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            try:
                data = self.child.read_nonblocking(65536, timeout=.05)
            except pexpect.TIMEOUT:
                continue
            except pexpect.EOF:
                return
            if data:
                self.transcript += data
                self.stream.feed(data)

    def display(self):
        return '\n'.join(self.screen.display)

    def wait(self, text):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            self.pump(0.2)
            if text in self.display():
                return
        raise AssertionError(f'Missing {text!r}\n' + self.display())

    def send(self, keys, text=None):
        self.child.send(keys)
        self.pump(0.4)
        if text is not None:
            self.wait(text)

    def history_text(self):
        lines = [''.join(cell.data for _, cell in sorted(row.items()))
                 for row in self.screen.history.top]
        return '\n'.join(lines + self.screen.display)

    def assert_inline(self):
        for sequence in ('\x1b[?1049', '\x1b[?1047', '\x1b[2J', '\x1b[3J',
                         '\x1b[H'):
            assert sequence not in self.transcript
        if not self.production:
            history = self.history_text()
            for i in range(32):
                assert f'SHELL_HISTORY_{i:02d}' in history, history

    def finish(self, result=None, production_exit=0):
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            try:
                data = self.child.read_nonblocking(65536, timeout=.2)
            except pexpect.TIMEOUT:
                continue
            except pexpect.EOF:
                break
            self.transcript += data
            self.stream.feed(data)

        try:
            self.child.close(force=True)
        except Exception:
            pass

        self.assert_inline()
        expected = production_exit if self.production else 0
        assert self.child.exitstatus == expected, self.transcript
        assert '\x1b[?2004l' in self.transcript and '\x1b[?25h' in self.transcript
        if self.production:
            if production_exit == 0:
                assert 'Setup complete.' in self.transcript, self.transcript
        else:
            assert f'SETUP_RESULT={result} RAW_RESTORED=1' in self.transcript, self.transcript

    def save(self, tabs=12, result=0, production_exit=0):
        self.child.send(TAB * tabs + ENTER)
        self.finish(result, production_exit)
        return self.path.read_text()

    def close(self):
        if self.child.isalive():
            self.child.send(CTRL_C)
            self.pump(0.3)
            self.child.close(force=True)


def model_section(config, kind):
    return config.split(f'[model.{kind}]', 1)[1].split('\n[', 1)[0]


def tests(driver, directory, production, cancelled_status):
    terminals = []

    def start(name, **kwargs):
        print('Setup scenario:', name, flush=True)
        term = Terminal(driver, directory / f'{name}.toml', **kwargs)
        terminals.append(term)
        return term

    try:
        # The form is a single page at every supported terminal size.
        for height, width in ((40, 120), (24, 80), (24, 60), (14, 40)):
            t = start(f'layout-{height}x{width}', ready=True, size=(height, width))
            screen = t.display()
            assert 'Role' in screen and 'Text model' in screen, screen
            assert 'Save' in screen and 'Cancel' in screen, screen
            if height >= 14 and width >= 46:
                assert 'morph setup' in screen, screen
            t.assert_inline()
            t.close()

        # Ready credentials: one form, then save.
        t = start('defaults', ready=True)
        config = t.save()
        assert 'role = "general"' in config
        assert 'model = "gpt-4o"' in config

        # Role is a standard drop-down in the form.
        t = start('role-change', ready=True, size=(40, 120))
        t.send(ENTER)
        t.send(DOWN + ENTER, 'Software developer')
        config = t.save()
        assert 'role = "developer"' in config

        # A missing key is a warning, not a blocked save.
        t = start('later', ready=False)
        assert 'needs a key' in t.display()
        config = t.save(result=1)
        assert 'api_key_env = "OPENAI_API_KEY"' in config
        assert ('export OPENAI_API_KEY' in t.transcript or
                'Set the text model key' in t.transcript)

        # Optional models are standard check boxes.
        t = start('vision', ready=True)
        t.send(TAB * 8 + ' ', '[x] Vision model')
        t.send(TAB * 11 + ENTER)
        t.finish(0)
        assert 'model = "gpt-4o"' in model_section(t.path.read_text(), 'vision')

        # Direct keys use the standard password field.
        secret = 'fixture-session-secret-123'
        t = start('secret', ready=False, size=(40, 120))
        t.send(TAB * 3)
        t.send(RIGHT, 'Save key to config')
        t.send(TAB)
        t.send(secret + ENTER)
        t.send(TAB * 7 + ENTER)
        t.finish(0)
        config = t.path.read_text()
        assert secret in config
        assert secret not in t.transcript
        assert 'api_key_env' not in model_section(config, 'text')
        assert t.path.stat().st_mode & 0o777 == 0o600

        # Invalid URLs stay on the field and show the form error.
        t = start('invalid-url', ready=True)
        t.send(TAB * 7)
        t.send(CTRL_U + 'ftp://example')
        t.send(TAB, 'Use an http:// or https:// URL.')
        t.child.send(CTRL_C)
        t.finish(cancelled_status)
        assert not t.path.exists()

        # No-color output remains a form and never emits SGR.
        t = start('no-color', ready=True, flags=('--no-color',))
        t.save()
        assert not re.search(r'\x1b\[[0-9;]*m', t.transcript)

        # Explicit setup saves before the runtime starts.
        t = Terminal(production, directory / 'production.toml',
                     production=True, ready=True)
        terminals.append(t)
        t.save()
        assert t.path.exists()
        assert not (t.path.parent / '.morph/data.db').exists()

        # Reconfiguration preserves prompt settings and backs up the old file.
        path = directory / 'reconfigure.toml'
        path.write_text("[model.text]\nprovider='openai'\nmodel='gpt-4o'\n"
                        "api_base='https://api.openai.com/v1'\n"
                        "api_key='fixture-old-secret'\n"
                        "[prompt]\nmode='append'\n"
                        "system_prompt_file='/keep/custom.md'\n")
        original = path.read_text()
        t = Terminal(production, path, production=True, size=(40, 120))
        terminals.append(t)
        t.wait('Keep existing instructions')
        t.save()
        assert any(p.read_text() == original
                   for p in directory.glob(path.name + '.backup-*'))

        # A concurrent external edit is not overwritten.
        path = directory / 'concurrent.toml'
        path.write_text("[model.text]\nprovider='openai'\nmodel='gpt-4o'\n"
                        "api_base='https://api.openai.com/v1'\n"
                        "api_key='fixture-key'\n")
        t = Terminal(production, path, production=True)
        terminals.append(t)
        changed = path.read_text() + '\n# external edit\n'
        path.write_text(changed)
        t.child.send(TAB * 12 + ENTER)
        t.finish(production_exit=1)
        assert path.read_text() == changed

        # Signals cancel without writing.
        t = start('signal', ready=True)
        os.kill(t.child.pid, signal.SIGTERM)
        t.finish(cancelled_status)
        assert not t.path.exists()

        print(f'Setup PTY: {len(terminals)} scenarios passed', flush=True)
    finally:
        for t in terminals:
            t.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='morph-setup-pty-') as tmp:
        cancelled_status = native_status('NB_ERR_CANCELLED', Path(sys.argv[3]))
        tests(Path(sys.argv[1]).resolve(), Path(tmp), Path(sys.argv[2]).resolve(),
              cancelled_status)
