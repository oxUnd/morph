"""First-run wizard: real PTY, rendered-screen assertions, isolated credentials."""
import errno
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import json

try:
    import pexpect
    import pyte
except ImportError:
    print('Setup PTY tests require pexpect and pyte')
    sys.exit(77)


class Terminal:
    def __init__(self, binary, path, ready=False, size=(24, 80), flags=(),
                 production=False, no_color_env=False, role=1, setup_command=False):
        env = {k: v for k, v in os.environ.items()
               if not k.endswith('API_KEY') and k != 'NO_COLOR'}
        if no_color_env:
            env['NO_COLOR'] = '1'
        env.update(TERM='xterm-256color', HOME=str(path.parent))
        if ready:
            env.update(OPENAI_API_KEY='fixture-openai', DEEPSEEK_API_KEY='fixture-deepseek',
                       VOLCENGINE_API_KEY='fixture-ark')
        self.path = path
        arguments = (["setup", "-c", str(path)] if setup_command else
                     ["-c", str(path)] if production else [str(path)])
        self.child = pexpect.spawn(str(binary),
                                   arguments + list(flags), env=env,
                                   encoding='utf-8', dimensions=size, timeout=5)
        self.screen = pyte.HistoryScreen(size[1], size[0], history=2000)
        self.stream = pyte.Stream(self.screen)
        self.transcript = ''
        self.production = production
        self.wait('Choose your role')
        self.child.send(f'{role}\r')
        if path.exists():
            self.wait('Prompt instructions')
            self.child.send('1\r')
        self.wait('Choose your provider')

    def wait(self, text):
        # A resize must actually be rendered before an old matching label can
        # satisfy the assertion; pyte cannot restore text clipped on shrinking.
        fresh = not getattr(self, 'needs_redraw', False)
        deadline = time.monotonic() + 6
        while time.monotonic() < deadline:
            try:
                data = self.child.read_nonblocking(65536, timeout=.03)
            except pexpect.TIMEOUT:
                # Read a complete burst of redraws before inspecting the screen.
                if fresh and text in '\n'.join(self.screen.display):
                    return
                continue
            except pexpect.EOF:
                if text in '\n'.join(self.screen.display):
                    return
                break
            self.transcript += data
            self.stream.feed(data)
            fresh = True
            self.needs_redraw = False
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
        self.needs_redraw = True

    def send(self, keys, text):
        self.child.send(keys)
        self.wait(text)

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


def thinking_value(config, kind):
    section = config.split(f'[model.{kind}]')[1].split('\n[')[0]
    line = next(line for line in section.splitlines() if line.startswith('extra_body_json ='))
    return json.loads(json.loads(line.split('=', 1)[1].strip()))['thinking']['type']


def active_text(t, title):
    return '\n'.join(t.screen.display).rsplit(title, 1)[1]


def assert_cards(t):
    edges = []
    for row in t.screen.display:
        if '┌' in row:
            assert row.index('┌') == 2, row
            edges.append(row.index('┐'))
        if '│' in row:
            assert row.index('│') == 2, row
            edges.append(row.rindex('│'))
        if '└' in row:
            assert row.index('└') == 2, row
            edges.append(row.index('┘'))
    assert edges and len(set(edges)) == 1, '\n'.join(t.screen.display)
    selected = next(i for i, row in enumerate(t.screen.display) if '│ ›' in row)
    cell = t.screen.buffer[selected][4]
    assert cell.fg == 'cyan' and cell.bg != 'default', cell


def skip_extras(t):
    t.send('1\r', 'Create images with morph?')
    t.send('1\r', 'Create videos with morph?')
    t.send('1\r', '4/4  Save')


def tests(driver, directory, production):
    terminals = []

    def start(name, **kwargs):
        print('Setup scenario:', name, flush=True)
        term = Terminal(driver, directory / f'{name}.toml', **kwargs)
        terminals.append(term)
        return term

    try:
        # Main flow is approachable, technical fields stay behind Advanced.
        t = start('append-cards', ready=True)
        assert_cards(t)
        t.send('1\r', 'Make morph yours')
        main = active_text(t, 'Make morph yours')
        assert 'Advanced settings' in main
        assert 'https://' not in main and '128000' not in main and '16384' not in main
        assert_cards(t)
        t.send('\r', 'Understand images with morph?')
        assert_cards(t)
        skip_extras(t)
        assert '4/4  Save' in '\n'.join(t.screen.display)
        assert not t.path.exists()
        review = active_text(t, "You're ready.")
        assert 'ctx=' not in review and str(t.path) not in review
        config = t.save()
        assert config.count('model = ""') == 3
        history = t.history_text()
        assert history.count('✓ Text · OpenAI') == 1, history
        assert '✓ Images · Skip for now' in history
        assert '✓ Text · Continue' not in history
        assert '↑↓ Choose' not in history

        # Exported keys skip account entry, but thinking is explicitly confirmed.
        t = start('thinking-on', ready=True)
        t.send('2\r', 'Make morph yours')
        t.send('\r', 'How should morph think?')
        assert_cards(t)
        t.send('\t', '› On')
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        assert 'Text  Thinking On' in '\n'.join(t.screen.display)
        assert thinking_value(t.save(), 'text') == 'enabled'

        # No key: account comes first, no save until the final confirmation.
        t = start('account-later')
        t.send('2\r', 'Connect your account')
        assert '2/4  Account' in '\n'.join(t.screen.display)
        assert_cards(t)
        t.send('3\r', 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('1\r', 'Understand images with morph?')
        skip_extras(t)
        config = t.save(1)
        assert thinking_value(config, 'text') == 'disabled'
        assert "export DEEPSEEK_API_KEY='<your-api-key>'" in t.transcript

        # Hidden paste cannot submit on embedded newline. No secret in output.
        secret = 'fixture-session-secret-123'
        t = start('secret')
        t.send('1\r', 'Connect your account')
        t.send('\r', 'Paste your API key')
        t.send('\r', 'Enter a value, or press Esc to go back.')
        t.send('\x1b[200~' + secret + '\n\x1b[201~', '••••••••')
        assert 'Paste your API key' in '\n'.join(t.screen.display)
        t.send('\r', 'Make morph yours')
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        config = t.save()
        assert f'api_key = "{secret}"' in config and secret not in t.transcript
        assert t.path.stat().st_mode & 0o777 == 0o600

        # Invalid env names are rejected; account-back keeps the selected provider.
        t = start('env-key')
        t.send('2\r', 'Connect your account')
        t.send('\x1b', 'Choose your provider')
        t.send('\r', 'Connect your account')
        t.send('2\r', 'API key environment variable')
        t.send('\x159INVALID\r', 'Start with a letter or underscore')
        t.send('\x15SETUP_MISSING_API_KEY\r', 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('2\r', 'Understand images with morph?')
        skip_extras(t)
        config = t.save(1)
        assert 'api_key_env = "SETUP_MISSING_API_KEY"' in config

        # Unicode model editing, URL validation, limits, and review edits.
        t = start('advanced', ready=True)
        t.send('2\r', 'Make morph yours')
        t.send('4\r', 'Advanced settings')
        t.send('2\r', 'Model name / endpoint ID')
        t.send('\x15模型甲\x7f乙\r', 'Advanced settings')
        assert '模型乙' in '\n'.join(t.screen.display)
        t.send('3\r', 'API base URL')
        t.send('\x15invalid\r', 'Enter a URL starting')
        t.send('\x15http://localhost:1234/v1\r', 'Advanced settings')
        t.send('5\r', 'Context window (tokens)')
        t.send('\x150\r', 'Invalid token count')
        t.send('\x1564000\r', 'Advanced settings')
        t.send('6\r', 'Max output (tokens)')
        t.send('\x1564000\r', 'Invalid token count')
        t.send('\x154096\r', 'Advanced settings')
        t.send('\r', 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('2\r', 'Understand images with morph?')
        skip_extras(t)
        t.send('2\r', 'Choose your provider')
        t.send('\r', 'Make morph yours')
        assert 'Thinking · On' in active_text(t, 'Make morph yours')
        t.send('4\r', 'Advanced settings')
        advanced = active_text(t, 'Advanced settings')
        assert '模型乙' in advanced and '64000 tokens' in advanced and '4096 tokens' in advanced
        t.send('\x1b', 'Make morph yours')
        t.send('\r', "You're ready.")
        config = t.save()
        assert 'model = "模型乙"' in config
        assert 'api_base = "http://localhost:1234/v1"' in config
        assert 'context_limit = 64000' in config and 'max_tokens = 4096' in config

        # Custom endpoint cannot be continued until required fields are filled.
        t = start('custom')
        t.send('4\r', 'Connect your account')
        t.send('3\r', 'Make morph yours')
        t.send('\r', 'Advanced settings')
        t.send('\r', 'Model name / endpoint ID')
        model = 'custom-' + 'x' * 75 + '-tail'
        t.send(model, '-tail')
        t.send('\r', 'Advanced settings')
        t.send('\r', 'API base URL')
        t.send('https://example.test/v1\r', 'Advanced settings')
        t.send('\r', 'Make morph yours')
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        config = t.save(1)
        assert f'model = "{model}"' in config

        # Independent text / vision reasoning, back cancels a highlighted choice.
        t = start('review-thinking', ready=True)
        t.send('2\r\r', 'How should morph think?')
        t.send('2\r', 'Understand images with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('2\r\r', 'How should morph think?')
        t.send('1\r', 'Create images with morph?')
        t.send('1\r', 'Create videos with morph?')
        t.send('1\r', "You're ready.")
        screen = '\n'.join(t.screen.display)
        assert 'Text  Thinking On' in screen and 'Vision  Thinking Off' in screen
        t.send('2\r', 'Choose your provider')
        t.send('\r', 'Make morph yours')
        t.send('3\r', 'How should morph think?')
        assert '› On' in '\n'.join(t.screen.display)
        t.send('\x1b[A\x1b', 'Make morph yours')
        assert 'Thinking · On' in active_text(t, 'Make morph yours')
        t.send('3\r', 'How should morph think?')
        t.send('1\r', 'Make morph yours')
        t.send('\r', "You're ready.")
        t.send('3\r', 'Understand images with morph?')
        t.send('\r', 'Choose your provider')
        t.send('\r', 'Make morph yours')
        t.send('3\r', 'How should morph think?')
        t.send('2\r', 'Make morph yours')
        t.send('\r', "You're ready.")
        config = t.save()
        assert thinking_value(config, 'text') == 'disabled'
        assert thinking_value(config, 'vision') == 'enabled'

        # Provider changes reset provider-specific state and don't leak controls.
        t = start('provider-switch', ready=True)
        t.send('2\r', 'Make morph yours')
        t.send('3\r', 'How should morph think?')
        t.send('2\r', 'Make morph yours')
        t.send('\x1b', 'Choose your provider')
        t.send('1\r', 'Make morph yours')
        assert 'Thinking' not in active_text(t, 'Make morph yours')
        t.send('\x1b', 'Choose your provider')
        t.send('2\r', 'Make morph yours')
        assert 'Thinking · Off' in active_text(t, 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('1\r', 'Understand images with morph?')
        skip_extras(t)
        assert thinking_value(t.save(), 'text') == 'disabled'

        # Moving between an editor and a compact viewport preserves the draft;
        # cancelled edits never replace a previously accepted value.
        t = start('edit-resize', ready=True)
        t.send('1\r', 'Make morph yours')
        t.send('3\r', 'Advanced settings')
        t.send('2\r', 'Model name / endpoint ID')
        t.send('\x15中文-draft', '中文-draft')
        t.resize(12, 36)
        t.wait('中文-draft')
        t.resize(24, 80)
        t.wait('中文-draft')
        t.send('\x1b', 'Advanced settings')
        assert 'gpt-4o' in active_text(t, 'Advanced settings')
        t.send('\x1b', 'Make morph yours')
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        assert 'model = "gpt-4o"' in t.save()

        # Known model ceilings and the relationship between context / output are
        # validated, and choosing a different model resets edited token limits.
        t = start('model-limits', ready=True)
        t.send('1\r', 'Make morph yours')
        t.send('3\r', 'Advanced settings')
        t.send('5\r', 'Context window (tokens)')
        t.send('\x15200000\r', 'Invalid token count')
        t.send('\x158192\r', 'Advanced settings')
        t.send('6\r', 'Max output (tokens)')
        t.send('8192\r', 'Invalid token count')
        t.send('\x152048\r', 'Advanced settings')
        t.send('2\r', 'Model name / endpoint ID')
        t.send('\x15my-deployment\r', 'Advanced settings')
        assert '128000 tokens' in active_text(t, 'Advanced settings')
        t.send('\r', 'Make morph yours')
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        config = t.save()
        assert 'model = "my-deployment"' in config
        assert 'context_limit = 128000' in config and 'max_tokens = 16384' in config

        # Image and video accounts / models remain independent from text.
        t = start('media', ready=True)
        t.send('1\r\r', 'Understand images with morph?')
        t.send('1\r', 'Create images with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('2\r', 'Make morph yours')
        t.send('\r', 'Create videos with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('1\r\r', "You're ready.")
        config = t.save()
        assert 'adapter = "volcengine-images"' in config
        assert 'adapter = "volcengine-videos"' in config
        assert 'model = "doubao-seedance-2-0-mini-260615"' in config

        # Optional capability can be removed from the final review.
        t = start('skip-vision', ready=True)
        t.send('1\r\r', 'Understand images with morph?')
        t.send('2\r', 'Choose your provider')
        t.send('1\r\r', 'Create images with morph?')
        t.send('1\r', 'Create videos with morph?')
        t.send('1\r', "You're ready.")
        t.send('3\r', 'Understand images with morph?')
        t.send('1\r', "You're ready.")
        config = t.save()
        assert config.count('model = ""') == 3

        # Wide cards shrink into compact choices without losing selection, drafts,
        # or shell scrollback. Grow back to bordered cards before continuing.
        t = start('resize', ready=True)
        t.send('2\r\r', 'How should morph think?')
        t.send('\t', '› On')
        for rows, columns in [(14, 40), (10, 32), (6, 28), (24, 80)]:
            t.resize(rows, columns)
            t.wait('› On')
            assert 'Off · Faster responses' in '\n'.join(t.screen.display)
        assert_cards(t)
        t.send('\r', 'Understand images with morph?')
        skip_extras(t)
        assert thinking_value(t.save(), 'text') == 'enabled'

        t = start('narrow', ready=True, size=(14, 40), flags=('--no-color',))
        t.send('2\r', 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('\x1b', 'Make morph yours')
        assert 'Thinking · Off' in active_text(t, 'Make morph yours')
        t.send('\r', 'How should morph think?')
        t.send('1\r', 'Understand images with morph?')
        skip_extras(t)
        assert 'Text  Thinking Off' in '\n'.join(t.screen.display)
        config = t.save()
        assert thinking_value(config, 'text') == 'disabled'
        import re
        assert not re.search(r'\x1b\[[0-9;]*m', t.transcript)

        # NO_COLOR is respected independently from the --no-color flag.
        t = start('no-color-env', ready=True, no_color_env=True)
        t.send('1\r\r', 'Understand images with morph?')
        skip_extras(t)
        t.save()
        assert not re.search(r'\x1b\[[0-9;]*m', t.transcript)

        # Bracketed paste in a selection cannot activate a hidden save action.
        t = start('menu-paste', ready=True)
        t.send('\x1b[200~2\n\x1b[201~', 'Choose your provider')
        assert not t.path.exists()
        t.child.send('\x03')
        t.finish(-errno.ECANCELED)

        for name, key, view in [('cancel-card', '\x03', 'Make morph yours'),
                                ('cancel-edit', '\x04', 'Model name / endpoint ID')]:
            t = start(name, ready=True)
            t.send('1\r', 'Make morph yours')
            if name == 'cancel-edit':
                t.send('3\r', 'Advanced settings')
                t.send('2\r', view)
            t.child.send(key)
            t.finish(-errno.ECANCELED)
            assert not t.path.exists()

        t = start('cancel-review', ready=True)
        t.send('1\r\r', 'Understand images with morph?')
        skip_extras(t)
        t.child.send('7\r')
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()

        t = start('signal')
        os.kill(t.child.pid, signal.SIGTERM)
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()

        # A video role requires all four models; there are no capability skip pages.
        for columns in (120, 80, 60):
            t = start(f'video-{columns}', ready=True, role=7, size=(24, columns))
            assert 'Text model required' in '\n'.join(t.screen.display)
            t.send('1\r\r', 'Vision model required')
            t.send('1\r\r', 'Image model required')
            t.send('1\r\r', 'Video model required')
            t.send('1\r\r', "You're ready.")
            config = t.save()
            assert 'role = "video_creator"' in config and 'model = ""' not in config
            assert 'Understand images with morph?' not in t.transcript
            assert 'Create images with morph?' not in t.transcript
            assert 'Create videos with morph?' not in t.transcript

        # Switching the role on review rechecks the stronger model requirements.
        t = start('change-role', ready=True)
        t.send('1\r\r', 'Understand images with morph?')
        skip_extras(t)
        t.send('6\r', 'Choose your role')
        t.send('7\r', 'Text model required')
        t.send('1\r\r', 'Vision model required')
        t.send('1\r\r', 'Image model required')
        t.send('1\r\r', 'Video model required')
        t.send('1\r\r', "You're ready.")
        assert 'model = ""' not in t.save()

        # Explicit setup and first-run setup exit even when all keys are present.
        for explicit in (False, True):
            path = directory / f'production-ready-{explicit}.toml'
            t = Terminal(production, path, production=True, ready=True,
                         setup_command=explicit)
            terminals.append(t)
            t.send('1\r\r', 'Understand images with morph?')
            skip_extras(t)
            t.send('\r', 'Setup complete.')
            t.child.expect(pexpect.EOF)
            t.child.close()
            assert t.child.exitstatus == 0 and path.exists()
            assert not (path.parent / '.morph/data.db').exists()
            t.assert_inline()

        # Re-running the explicit command loads saved defaults and keeps the prompt.
        original = path.read_text()
        t = Terminal(production, path, production=True, ready=True, setup_command=True)
        terminals.append(t)
        t.send('1\r\r', 'Understand images with morph?')
        skip_extras(t)
        t.send('\r', 'Setup complete.')
        t.child.expect(pexpect.EOF)
        t.child.close()
        assert t.child.exitstatus == 0
        assert any(p.read_text() == original for p in path.parent.glob(path.name + '.backup-*'))

        # Concurrent edits cannot be overwritten by a wizard opened earlier.
        t = Terminal(production, path, production=True, ready=True, setup_command=True)
        terminals.append(t)
        t.send('1\r\r', 'Understand images with morph?')
        skip_extras(t)
        changed = path.read_text() + '\n# external edit during setup\n'
        path.write_text(changed)
        t.child.send('\r')
        t.child.expect(pexpect.EOF)
        t.child.close()
        assert t.child.exitstatus == 1 and path.read_text() == changed

        # Production entrypoint, both providers, no real keys or network calls.
        for provider in ('1', '2'):
            print('Setup scenario: production', provider, flush=True)
            t = Terminal(production, directory / f'production-{provider}.toml', production=True)
            terminals.append(t)
            t.send(provider + '\r', 'Connect your account')
            t.send('3\r', 'Make morph yours')
            t.send('\r', 'How should morph think?' if provider == '2' else
                   'Understand images with morph?')
            if provider == '2':
                t.send('2\r', 'Understand images with morph?')
            skip_extras(t)
            t.send('\r', 'Configuration saved')
            t.child.expect(pexpect.EOF)
            t.child.close()
            assert t.child.exitstatus == 0 and t.path.exists()
            t.assert_inline()
            if provider == '2':
                assert thinking_value(t.path.read_text(), 'text') == 'enabled'
        print(f'Setup PTY: {len(terminals)} scenarios passed, including production', flush=True)
    finally:
        for term in terminals:
            term.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='morph-setup-pty-') as tmp:
        tests(Path(sys.argv[1]).resolve(), Path(tmp), Path(sys.argv[2]).resolve())
