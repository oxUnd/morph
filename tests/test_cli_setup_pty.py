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
    from test_cli_transcript_pty import AlternateScreen
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


class SetupScreen(AlternateScreen, pyte.HistoryScreen):
    pass


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
        self.screen = SetupScreen(size[1], size[0], history=4000)
        self.stream = pyte.Stream(self.screen)
        self.transcript = ''
        self.pump(0.8)
        self.wait('Set up morph')

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

    def assert_native(self):
        assert '\x1b[?1049h' in self.transcript, self.transcript
        assert '\x1b[?1000h' in self.transcript, self.transcript
        assert self.display().count('Set up morph') <= 1, self.display()
        if not self.production:
            for i in range(32):
                assert f'SHELL_HISTORY_{i:02d}' in self.transcript

    def click(self, text, occurrence=0):
        matches = [(row, line.index(text)) for row, line in enumerate(self.screen.display)
                   if text in line]
        assert len(matches) > occurrence, f'Missing click target {text!r}\n' + self.display()
        row, column = matches[occurrence]
        self.send(f'\x1b[<0;{column + 2};{row + 1}M'
                  f'\x1b[<0;{column + 2};{row + 1}m')

    def field(self, label):
        pattern = re.compile(r'^[ │]*' + re.escape(label) + r'(?:\s|$)')
        matches = [(row, line.index(label)) for row, line in enumerate(self.screen.display)
                   if pattern.search(line)]
        assert matches, f'Missing field {label!r}\n' + self.display()
        row, column = matches[0]
        if min(self.screen.columns, 104) - 4 >= 64:
            column += 16
        else:
            row += 1
            column += 1
        self.send(f'\x1b[<0;{column + 1};{row + 1}M'
                  f'\x1b[<0;{column + 1};{row + 1}m')

    def edit(self, label, value):
        self.field(label)
        self.send('\x1b[F' + CTRL_U + value)

    def wheel(self, down=True, steps=3):
        row = next(i for i, line in enumerate(self.screen.display)
                   if 'Images' in line and 'Video' in line) + 3
        self.send(f'\x1b[<{65 if down else 64};8;{row + 1}M' * steps)

    def resize(self, rows, columns):
        self.child.setwinsize(rows, columns)
        self.screen.resize(rows, columns)
        self.pump(.4)
        self.assert_native()

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

        self.assert_native()
        expected = production_exit if self.production else 0
        assert self.child.exitstatus == expected, self.transcript
        assert '\x1b[?2004l' in self.transcript and '\x1b[?25h' in self.transcript
        assert '\x1b[?1000l' in self.transcript and '\x1b[?1049l' in self.transcript
        if self.production:
            if production_exit == 0:
                assert 'Setup complete.' in self.transcript, self.transcript
        else:
            assert f'SETUP_RESULT={result} RAW_RESTORED=1' in self.transcript, self.transcript
            history = self.history_text()
            for i in range(32):
                assert f'SHELL_HISTORY_{i:02d}' in history, history

    def save(self, result=0, production_exit=0):
        self.child.send('\x1b[13;5u')
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
        for height, width in ((40, 120), (24, 80), (24, 60), (14, 40)):
            t = start(f'layout-{height}x{width}', ready=True, size=(height, width))
            screen = t.display()
            assert 'Role' in screen and 'Text model' in screen, screen
            assert 'Save' in screen and 'Cancel' in screen, screen
            assert 'API base' not in screen, screen
            row = next(i for i, line in enumerate(t.screen.display) if 'F10 Save' in line)
            column = t.screen.display[row].index('F10')
            assert t.screen.buffer[row][column].bold, screen
            t.assert_native()
            t.click('Cancel')
            t.finish(cancelled_status)
            assert not t.path.exists()

        t = start('save-legacy-f10', ready=True)
        t.child.send('\x1b[21~')
        t.finish(0)
        assert t.path.exists()

        t = start('defaults-mouse', ready=True)
        t.click('Save and exit')
        t.finish(0)
        config = t.path.read_text()
        assert 'role = "general"' in config and 'model = "gpt-4o"' in config

        t = start('role-mouse', ready=True, size=(32, 120))
        t.click('General assistant')
        t.wait('Choose one')
        assert t.screen.cursor.hidden, t.display()
        t.click('Software developer')
        t.wait('Build, debug, test and review software.')
        t.click('Software developer')
        t.send('\x1b[27u')
        assert 'Choose one' not in t.display(), t.display()
        assert 'Build, debug, test and review software.' in t.display()
        t.click('Software developer')
        t.click('Set up morph')
        assert 'Choose one' not in t.display(), t.display()
        assert 'role = "developer"' in t.save()

        for name, enter, down in (('legacy', ENTER, DOWN),
                                  ('kitty', '\x1b[13u', '\x1b[57353u\x1b[57353;1:3u')):
            t = start('role-' + name, ready=True)
            t.send(enter)
            t.send(down + enter, 'Software developer')
            assert 'role = "developer"' in t.save()

        t = start('later', ready=False)
        assert 'needs a key' in t.display(), t.display()
        config = t.save(result=1)
        assert 'api_key_env = "OPENAI_API_KEY"' in config
        assert ('export OPENAI_API_KEY' in t.transcript or
                'Set the text model key' in t.transcript)

        t = start('vision-mouse', ready=True, size=(32, 80))
        t.click('Vision')
        t.wait('Vision model')
        assert 'Optional.' in t.display(), t.display()
        t.click('Enable this model')
        t.wait('[x] Enable this model')
        t.edit('Model', 'vision-fixture')
        t.send(ENTER)
        assert not t.path.exists(), 'Enter in a field must not submit the page'
        t.click('Text')
        t.wait('Text model')
        t.click('Vision')
        t.wait('vision-fixture')
        config = t.save()
        assert 'model = "vision-fixture"' in model_section(config, 'vision')
        assert 'model = "gpt-4o"' in model_section(config, 'text')

        secret = 'fixture-session-secret-123'
        t = start('secret-mouse', ready=False, size=(32, 120))
        t.click('Use environment variable')
        t.click('Save key to config')
        t.edit('API key', secret)
        t.send(ENTER)
        assert not t.path.exists()
        config = t.save()
        assert secret in model_section(config, 'text')
        assert secret not in t.transcript
        assert 'api_key_env' not in model_section(config, 'text')
        assert t.path.stat().st_mode & 0o777 == 0o600

        t = start('empty-key-later', ready=False)
        t.click('Use environment variable')
        t.click('Save key to config')
        t.click('Save for later')
        t.finish(1)
        assert 'api_key =' not in model_section(t.path.read_text(), 'text')

        t = start('provider-mouse', ready=True, size=(40, 80))
        t.edit('Model', '')
        t.click('OpenAI')
        t.click('DeepSeek')
        t.wait('deepseek-flash')
        assert 'Enter a model name.' not in t.display(), t.display()
        t.click('Advanced settings')
        t.wait('1048576')
        t.wait('Role defaults')
        assert 'Integrations:' in t.display(), t.display()
        config = t.save()
        assert 'provider = "deepseek"' in config and 'context_limit = 1048576' in config

        t = start('invalid-url', ready=True, size=(40, 80))
        t.click('Advanced settings')
        t.edit('API base', 'ftp://example')
        t.click('Save and exit')
        t.wait('Use an http:// or https:// URL.')
        assert 'ftp://example' in t.display(), t.display()
        assert not t.path.exists()
        t.edit('API base', 'http://localhost:1234/v1')
        assert 'api_base = "http://localhost:1234/v1"' in t.save()

        t = start('wheel', ready=True)
        t.click('Advanced settings')
        thumb = [(row, line.index('█')) for row, line in enumerate(t.screen.display)
                 if '█' in line]
        assert thumb, t.display()
        row, column = thumb[len(thumb) // 2]
        bottom = next(i for i, line in enumerate(t.screen.display)
                      if 'Ready to save' in line) - 1
        t.send(f'\x1b[<0;{column + 1};{row + 1}M'
               f'\x1b[<32;{column + 1};{bottom + 1}M'
               f'\x1b[<0;{column + 1};{bottom + 1}m')
        t.wait('Max output')
        t.wheel(down=False, steps=20)
        t.wait('Text model')
        t.wheel(steps=8)
        t.wait('Max output')
        assert 'Text model' not in t.display(), t.display()
        before = t.display()
        t.pump(.2)
        assert t.display() == before, 'scroll must remain independent of focus'
        t.wheel(down=False, steps=20)
        t.wait('Text model')
        t.click('Cancel')
        t.finish(cancelled_status)

        t = start('resize-draft', ready=True, size=(32, 80))
        t.edit('Model', '中文-draft')
        for rows, columns in ((24, 60), (14, 40), (24, 80), (40, 120)):
            t.resize(rows, columns)
            t.wait('中文-draft')
        t.send('尾')
        t.wait('中文-draft尾')
        assert 'model = "中文-draft尾"' in t.save()
        assert '\ufffd' not in t.transcript

        t = start('invalid-draft-resize', ready=True, size=(40, 80))
        t.click('Advanced settings')
        t.edit('API base', 'ftp://keep-draft')
        t.click('Save and exit')
        t.wait('Use an http:// or https:// URL.')
        for rows, columns in ((40, 60), (40, 80)):
            t.resize(rows, columns)
            t.wait('ftp://keep-draft')
        t.click('Cancel')
        t.finish(cancelled_status)
        assert not t.path.exists()

        t = start('no-color', ready=True, flags=('--no-color',))
        t.save()
        assert not re.search(r'\x1b\[[0-9;]*(?:38;|48;|3[0-7](?:;|m)|4[0-7](?:;|m))',
                             t.transcript)

        t = Terminal(production, directory / 'production.toml',
                     production=True, ready=True)
        terminals.append(t)
        t.save()
        assert t.path.exists()
        assert not (t.path.parent / '.morph/data.db').exists()

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

        path = directory / 'concurrent.toml'
        path.write_text("[model.text]\nprovider='openai'\nmodel='gpt-4o'\n"
                        "api_base='https://api.openai.com/v1'\n"
                        "api_key='fixture-key'\n")
        t = Terminal(production, path, production=True)
        terminals.append(t)
        changed = path.read_text() + '\n# external edit\n'
        path.write_text(changed)
        t.save(production_exit=1)
        assert path.read_text() == changed

        for name, cancel in (('signal', None), ('cancel-draft', CTRL_C), ('cancel-esc', ESC)):
            t = start(name, ready=True)
            t.edit('Model', 'unsaved-draft')
            if cancel is None:
                os.kill(t.child.pid, signal.SIGTERM)
            else:
                t.child.send(cancel)
            t.finish(cancelled_status)
            assert not t.path.exists()

        t = start('cancel-popup', ready=True)
        t.click('General assistant')
        t.wait('Choose one')
        t.child.send(CTRL_C)
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
