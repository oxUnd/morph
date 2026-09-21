"""First-run wizard: real PTY, rendered-screen assertions, isolated credentials."""
import errno
import os
from pathlib import Path
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


class Terminal:
    def __init__(self, binary, path, ready=False, size=(24, 80), flags=(), production=False):
        env = {k: v for k, v in os.environ.items() if not k.endswith('API_KEY')}
        env.update(TERM='xterm-256color', HOME=str(path.parent))
        if ready:
            env.update(OPENAI_API_KEY='fixture-openai', DEEPSEEK_API_KEY='fixture-deepseek',
                       VOLCENGINE_API_KEY='fixture-ark')
        self.path = path
        self.child = pexpect.spawn(str(binary),
                                   (["-c", str(path)] if production else [str(path)]) + list(flags), env=env,
                                   encoding='utf-8', dimensions=size, timeout=5)
        self.screen = pyte.HistoryScreen(size[1], size[0], history=2000)
        self.stream = pyte.Stream(self.screen)
        self.transcript = ''
        self.production = production
        self.wait('Choose your provider')

    def wait(self, text):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            try:
                data = self.child.read_nonblocking(65536, timeout=.03)
            except pexpect.TIMEOUT:
                # Read a complete burst of redraws before inspecting the screen.
                if text in '\n'.join(self.screen.display):
                    return
                continue
            except pexpect.EOF:
                if text in '\n'.join(self.screen.display):
                    return
                break
            self.transcript += data
            self.stream.feed(data)
        raise AssertionError(f'Missing {text!r}\n' + '\n'.join(self.screen.display))

    def history_text(self):
        lines = [''.join(cell.data for _, cell in sorted(row.items()))
                 for row in self.screen.history.top]
        return '\n'.join(lines + self.screen.display)

    def assert_inline(self):
        assert '\x1b[?1049' not in self.transcript
        assert '\x1b[?1047' not in self.transcript
        assert '\x1b[2J' not in self.transcript
        assert '\x1b[3J' not in self.transcript
        assert '\x1b[H' not in self.transcript
        if not self.production:
            history = self.history_text()
            for i in range(32):
                assert f'SHELL_HISTORY_{i:02d}' in history, history

    def resize(self, rows, columns):
        # pyte clips top rows on resize without moving them into history.
        # Preserve those rows, as a normal scrollback terminal does.
        for i in range(max(0, self.screen.lines - rows)):
            self.screen.history.top.append(self.screen.buffer[i].copy())
        self.screen.resize(rows, columns)
        self.child.setwinsize(rows, columns)

    def send(self, keys, text):
        self.child.send(keys)
        self.wait(text)

    def skip_vision(self, keys):
        self.send(keys, 'Understand images with morph?')
        self.send('\r', 'Create images with morph?')

    def required_tokens(self, next_title, context='64000', output='8000'):
        self.send('\r', 'Context window (tokens)')
        self.send(context + '\r', 'Max output (tokens)')
        self.send(output + '\r', next_title)

    def finish(self, result, key_ready=None):
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            try:
                data = self.child.read_nonblocking(65536, timeout=.2)
                self.transcript += data
                self.stream.feed(data)
            except pexpect.TIMEOUT:
                continue
            except pexpect.EOF:
                break
        self.child.close()
        self.assert_inline()
        assert self.child.exitstatus == 0, self.transcript
        assert f'SETUP_RESULT={result} RAW_RESTORED=1' in self.transcript, self.transcript
        if key_ready is not None:
            assert f'SESSION_KEY_READY={key_ready}' in self.transcript
        assert '\x1b[?2004l' in self.transcript and '\x1b[?25h' in self.transcript

    def save(self, result=0, key_ready=None):
        self.child.send('\r')
        self.finish(result, key_ready)
        return self.path.read_text()

    def close(self):
        if self.child.isalive():
            self.child.send('\x03')
            self.child.close(force=True)


def tests(driver, directory, production):
    terminals = []

    def start(name, **kwargs):
        term = Terminal(driver, directory / f'{name}.toml', **kwargs)
        terminals.append(term)
        return term

    try:
        # Defaults, credentials deferred, each optional capability skipped.
        t = start('skip')
        t.send('\r', 'Make it yours')
        t.send('\r', 'Connect your account')
        t.skip_vision('\x1b[B\x1b[B\r')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        assert 'Images  Skipped' in '\n'.join(t.screen.display)
        assert not t.path.exists(), 'No writes before final confirmation'
        config = t.save(1)
        assert config.count('model = ""') == 3
        history = t.history_text()
        assert history.count('✓ Text · OpenAI') == 1, history
        assert '✓ Images · Skip for now' in history
        assert '✓ Video · Skip for now' in history
        assert 'Text  gpt-4o · Key needed' in history
        assert '↑↓ Choose' not in history, 'Finished prompts must collapse to answers'


        # Arrow selection, Unicode editing/backspace, review edits preserve state.
        t = start('edit', ready=True)
        t.send('\x1b[B\r', 'Make it yours')
        t.send('\x1b[B\r', 'Model name / endpoint ID')
        t.send('\x15模型甲\x7f乙\r', 'Make it yours')
        assert '模型乙' in '\n'.join(t.screen.display)
        t.required_tokens('Understand images with morph?')
        t.send('\r', 'Create images with morph?')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        t.send('\x1b[B\r', 'Choose your provider')
        t.send('\r', 'Make it yours')
        assert '模型乙' in '\n'.join(t.screen.display)
        t.send('\r', 'Ready when you are')
        config = t.save()
        assert 'model = "模型乙"' in config and 'adapter = "deepseek"' in config

        # Both optional capabilities configured independently.
        t = start('both', ready=True)
        t.skip_vision('\r\r')
        t.send('\x1b[B\r', 'Choose your provider')
        t.send('\x1b[B\r', 'Make it yours')
        t.send('\r', 'Create videos with morph?')
        t.send('\x1b[B\r', 'Choose your provider')
        t.send('\r\r', 'Ready when you are')
        config = t.save()
        assert 'adapter = "volcengine-images"' in config
        assert 'adapter = "volcengine-videos"' in config

        # Hidden bracketed paste does not submit on pasted newline; a key supplied
        # for images is shared by the text model without being written to disk.
        secret = 'fixture-session-secret-123'
        t = start('secret')
        t.send('\r\r', 'Connect your account')
        t.skip_vision('\x1b[B\x1b[B\r')
        t.send('\x1b[B\r', 'Choose your provider')
        t.send('\r\r', 'Connect your account')
        t.send('\r', 'Paste your API key')
        t.send('\x1b[200~' + secret + '\n\x1b[201~', '••••••••')
        assert 'Paste your API key' in '\n'.join(t.screen.display), '\n'.join(t.screen.display)
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        config = t.save(0, key_ready=1)
        assert secret not in config and secret not in t.transcript
        assert 'Text  gpt-4o · Session key' in t.transcript

        # Custom endpoint, URL validation, input cancel, and horizontal scrolling.
        t = start('custom', ready=True)
        t.send('4\r', 'Make it yours')
        t.send('\r', 'Model name / endpoint ID')
        model = 'custom-' + 'x' * 75 + '-tail'
        t.send(model, '-tail')
        t.send('\r', 'Make it yours')
        t.send('\r', 'API base URL')
        t.send('invalid\r', 'Enter a URL starting')
        t.send('\x15http://localhost:1234/v1\r', 'Make it yours')
        t.required_tokens('Connect your account')
        t.send('\x1b[B\r', 'API key environment variable')
        t.send('\x159INVALID\r', 'Start with a letter or underscore')
        t.skip_vision('\x15OPENAI_API_KEY\r')
        t.send('\x1b[B\r', 'Choose your provider')
        t.send('\x1b', 'Create images with morph?')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        config = t.save()
        assert f'model = "{model}"' in config
        assert 'api_base = "http://localhost:1234/v1"' in config

        # Vision is independent, uses chat APIs, and can be edited from review.
        t = start('vision', ready=True)
        t.send('2\r\r', 'Understand images with morph?')
        t.send('2\r', 'Choose your provider')
        assert 'DeepSeek' not in '\n'.join(t.screen.display[-10:])
        t.send('\r\r', 'Create images with morph?')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        assert 'Vision  gpt-4o' in '\n'.join(t.screen.display)
        t.send('3\r', 'Understand images with morph?')
        t.send('\r', 'Choose your provider')
        t.send('\r', 'Make it yours')
        t.send('2\r', 'Model name / endpoint ID')
        t.send('\x15vision-edited\r', 'Make it yours')
        t.required_tokens('Ready when you are', '32000', '4096')
        config = t.save()
        vision = config.split('[model.vision]')[1].split('[model.image]')[0]
        assert 'model = "vision-edited"' in vision
        assert 'adapter = "openai-chat-compatible"' in vision
        assert config.count('model = ""') == 2

        # Other vision endpoints can be configured and then skipped from review.
        t = start('vision-other', ready=True)
        t.send('\r\r', 'Understand images with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('3\r', 'Make it yours')
        t.send('\r', 'Model name / endpoint ID')
        t.send('custom-vision\r', 'Make it yours')
        t.send('\r', 'API base URL')
        t.send('https://example.test/v1\r', 'Make it yours')
        t.required_tokens('Connect your account')
        t.send('3\r', 'Create images with morph?')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        assert 'Vision  custom-vision' in '\n'.join(t.screen.display)
        t.send('3\r', 'Understand images with morph?')
        t.send('1\r', 'Ready when you are')
        config = t.save()
        vision = config.split('[model.vision]')[1].split('[model.image]')[0]
        assert 'model = ""' in vision
        assert 'api_key_env = ""' in vision

        # Per-model fields can be overridden, invalid limits are rejected,
        # and replacing the model invalidates previously entered limits.
        t = start('token-limits', ready=True)
        t.send('\r', 'Make it yours')
        screen = '\n'.join(t.screen.display)
        assert '128000 tokens' in screen and '16384 tokens' in screen
        t.send('5\r', 'Context window (tokens)')
        t.send('\x15200000\r', 'Invalid token count')
        t.send('\x150\r', 'Invalid token count')
        t.send('\x1532000\r', 'Make it yours')
        t.send('6\r', 'Max output (tokens)')
        t.send('\x1520000\r', 'Invalid token count')
        t.send('\x154096\r', 'Make it yours')
        t.send('2\r', 'Model name / endpoint ID')
        t.send('\x15my-deployment\r', 'Make it yours')
        assert 'Required for this model' in '\n'.join(t.screen.display)
        t.send('\r', 'Context window (tokens)')
        t.send('8192\r', 'Max output (tokens)')
        t.send('8192\r', 'Invalid token count')
        t.send('\x152048\r', 'Understand images with morph?')
        t.send('\r', 'Create images with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('\r', 'Make it yours')
        assert 'Context window' not in '\n'.join(t.screen.display[-8:])
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        assert 'ctx=8192 out=2048' in '\n'.join(t.screen.display)
        config = t.save()
        assert 'context_limit = 8192' in config and 'max_tokens = 2048' in config
        image = config.split('[model.image]')[1].split('[model.video]')[0]
        assert 'max_tokens' not in image and 'context_limit' not in image

        # Resize, no color, back navigation, and cancellation restore terminal.
        t = start('narrow', ready=True, flags=('--no-color',))
        t.resize(14, 40)
        t.wait('Enter OK  Esc Back')
        t.send('\r', 'Make it yours')
        t.send('\x1b', 'Choose your provider')
        t.child.send('\x03')
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()
        import re
        assert not re.search(r'\x1b\[[0-9;]*m', t.transcript)

        # Cancellation while editing never creates a file or leaves raw mode.
        t = start('cancel-edit', ready=True)
        t.send('\r', 'Make it yours')
        t.send('\x1b[B\r', 'Model name / endpoint ID')
        t.child.send('\x04')
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()

        t = start('signal')
        os.kill(t.child.pid, signal.SIGTERM)
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()
        # Actual executable: first-run dispatch, no credentials, save then exit.
        t = Terminal(production, directory / 'production.toml', production=True)
        terminals.append(t)
        t.send('\r\r', 'Connect your account')
        t.skip_vision('3\r')
        t.send('\r', 'Create videos with morph?')
        t.send('\r', 'Ready when you are')
        t.send('\r', 'Configuration saved')
        t.child.expect(pexpect.EOF)
        t.child.close()
        assert t.child.exitstatus == 0 and t.path.exists()
        t.assert_inline()
        print('Setup PTY: 12 scenarios passed, including the production CLI')
    finally:
        for term in terminals:
            term.close()


with tempfile.TemporaryDirectory(prefix='morph-setup-pty-') as tmp:
    tests(Path(sys.argv[1]).resolve(), Path(tmp), Path(sys.argv[2]).resolve())
