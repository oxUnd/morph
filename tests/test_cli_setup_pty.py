"""First-run wizard: real PTY, rendered-screen assertions, isolated credentials."""
import errno
import os
from pathlib import Path
import signal
import sys
import tempfile
import time
import json
import re

try:
    import pexpect
    import pyte
except ImportError:
    print('Setup PTY tests require pexpect and pyte')
    sys.exit(77)


class Terminal:
    def __init__(self, binary, path, ready=False, size=(24, 80), flags=(),
                 production=False, no_color_env=False, role=1, setup_command=False,
                 accept_role=True):
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
        if accept_role:
            self.child.send(f'{role}\r')
            self.wait('Review your setup')

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
        if self.production:
            assert 'Setup complete.' in self.transcript
        else:
            assert f'SETUP_RESULT={result} RAW_RESTORED=1' in self.transcript, self.transcript
            if key_ready is not None:
                assert f'SESSION_KEY_READY={key_ready}' in self.transcript
        assert '\x1b[?2004l' in self.transcript and '\x1b[?25h' in self.transcript

    def save(self, result=0, key_ready=None):
        self.child.send('8\r' if result == 1 else '\r')
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


def section(config, kind):
    return config.split(f'[model.{kind}]', 1)[1].split('\n[', 1)[0]


def missing(config, kind, key):
    return not re.search(r'^\s*' + re.escape(key) + r'\s*=', section(config, kind), re.M)


def tests(driver, directory, production):
    terminals = []

    def start(name, **kwargs):
        print('Setup scenario:', name, flush=True)
        term = Terminal(driver, directory / f'{name}.toml', **kwargs)
        terminals.append(term)
        return term

    def context(t, name):
        assert name + ' model ·' in '\n'.join(t.screen.display)

    try:
        # Role selection fits in a compact list; only the highlighted role has details.
        labels = ('General assistant', 'Software developer', 'Photographer', 'Photo editor',
                  'Writer', 'Designer', 'Video creator', 'Researcher')
        for width, height in ((120, 24), (80, 24), (60, 24), (40, 14)):
            t = start(f'role-list-{width}', ready=True, size=(height, width), accept_role=False)
            screen = '\n'.join(t.screen.display)
            assert not any(border in screen for border in ('┌', '┐', '│', '└', '┘')), screen
            if height >= 20:
                assert all(label in screen for label in labels), screen
                assert screen.count('Models:') == 1, screen
            assert '› 1  General assistant  (default)' in screen
            assert not t.path.exists()
            t.send('\x1b[200~7\n\x1b[201~', 'Choose your role')
            assert '› 1  General assistant' in '\n'.join(t.screen.display)
            t.send('7', 'Models: Text + Vision')
            screen = '\n'.join(t.screen.display)
            assert 'Models:Text+Vision+Image+Video' in re.sub(r'\s+', '', screen), screen
            assert '› 7  Video creator' in screen
            assert 'Everyday questions' not in screen
            assert not t.path.exists()
            t.resize(6, 28)
            t.wait('Models: Text + Vision')
            assert 'Image + Video' in '\n'.join(t.screen.display)
            t.resize(height, width)
            t.wait('Models: Text + Vision')
            screen = '\n'.join(t.screen.display)
            assert 'Models:Text+Vision+Image+Video' in re.sub(r'\s+', '', screen), screen
            t.send('\r', 'Review your setup')
            config = t.save()
            assert 'model = ""' not in config
            assert len(re.findall(r'✓ Role\s+· Video creator', t.history_text())) == 1

        t = start('role-current', ready=True)
        t.send('6\r', 'Change your role')
        assert 'General assistant  (current)' in '\n'.join(t.screen.display)
        t.send('5', 'Develop ideas, write drafts and revise prose.')
        assert '› 5  Writer' in '\n'.join(t.screen.display)
        assert 'General assistant  (current)' in '\n'.join(t.screen.display)
        t.send('\x1b', 'Review your setup')
        assert 'Vision model · Off' in '\n'.join(t.screen.display)
        t.send('6\r', 'Change your role')
        t.send('\r', 'Review your setup')
        config = t.save()
        assert 'role = "general"' in config
        assert len(re.findall(r'✓ Role\s+· General assistant', t.history_text())) == 1

        # Available credentials make first-run setup exactly role -> overview -> save.
        t = start('defaults', ready=True)
        screen = '\n'.join(t.screen.display)
        assert '2/2  Review' in screen
        assert 'Text model · gpt-4o' in screen and 'Vision model · Off' in screen
        assert 'MCP: Exa' in screen and 'Host exec: on' in screen
        assert 'Choose your provider' not in t.transcript
        assert 'How should morph think?' not in t.transcript
        assert not t.path.exists()
        config = t.save()
        assert config.count('model = ""') == 3
        assert len(re.findall(r'✓ Role\s+· General assistant', t.history_text())) == 1
        assert '✓ Vision · Skip' not in t.history_text()

        # No credentials: saving a draft is one explicit overview action.
        t = start('later')
        assert 'Connect accounts and save' in '\n'.join(t.screen.display)
        config = t.save(1)
        assert 'export OPENAI_API_KEY=' in t.transcript
        assert missing(config, 'text', 'api_key')

        # Only the necessary account is requested on Finish; there is no model tour.
        secret = 'fixture-session-secret-123'
        t = start('secret')
        t.send('\r', 'Connect your account')
        context(t, 'Text')
        assert_cards(t)
        t.send('\r', 'Paste your API key')
        context(t, 'Text')
        t.send('\r', 'Enter a value, or press Esc to go back.')
        t.send('\x1b[200~' + secret + '\n\x1b[201~', '••••••••')
        t.child.send('\r')
        t.finish(0)
        config = t.path.read_text()
        assert secret in config and secret not in t.transcript
        assert missing(config, 'text', 'api_key_env')
        assert t.path.stat().st_mode & 0o777 == 0o600

        t = start('env-key')
        t.send('\r', 'Connect your account')
        t.send('2\r', 'API key environment variable')
        context(t, 'Text')
        t.send('\x159INVALID\r', 'Start with a letter or underscore')
        t.child.send('\x15SETUP_MISSING_API_KEY\r')
        t.finish(1)
        assert missing(t.path.read_text(), 'text', 'api_key')

        # Four required model defaults are visible, with no four-provider wizard.
        for width in (120, 80, 60):
            t = start(f'video-{width}', ready=True, role=7, size=(24, width))
            screen = '\n'.join(t.screen.display)
            for kind in ('Text', 'Vision', 'Image', 'Video'):
                assert f'{kind} model ·' in screen, screen
            config = t.save()
            assert 'model = ""' not in config
            assert 'How should morph think?' not in t.transcript

        # OpenAI text/vision/image use one account; video keeps its own provider.
        t = start('video-accounts', role=7)
        t.send('\r', 'Connect your account')
        context(t, 'Text')
        t.send('\r', 'Paste your API key')
        t.send('fixture-openai-shared\r', 'Connect your account')
        context(t, 'Video')
        assert 'doubao-seedance' in '\n'.join(t.screen.display)
        t.send('\r', 'Paste your API key')
        t.child.send('fixture-video-key\r')
        t.finish(0)
        config = t.path.read_text()
        for kind in ('text', 'vision', 'image'):
            assert 'fixture-openai-shared' in section(config, kind)
            assert missing(config, kind, 'api_key_env')
        assert 'fixture-video-key' in section(config, 'video')
        assert 'fixture-openai-shared' not in t.transcript
        assert 'fixture-video-key' not in t.transcript

        # Every edit retains an unambiguous model category and current model.
        t = start('contexts', ready=True, role=7)
        for number, kind in ((2, 'Text'), (3, 'Vision'), (4, 'Image'), (5, 'Video')):
            t.send(f'{number}\r', 'Model settings')
            context(t, kind)
            t.send('3\r', 'Model name / endpoint ID')
            context(t, kind)
            t.send('\x1b', 'Model settings')
            t.send('5\r', 'Advanced settings')
            context(t, kind)
            t.send('3\r', 'API base URL')
            context(t, kind)
            t.send('\x1b', 'Advanced settings')
            t.send('\x1b', 'Model settings')
            t.send('\x1b', 'Review your setup')
        t.save()

        # Thinking is an optional setting, not a forced confirmation.
        t = start('thinking', ready=True)
        t.send('2\r', 'Model settings')
        t.send('2\r', 'Choose your provider')
        context(t, 'Text')
        t.send('2\r', 'Model settings')
        assert 'Thinking · Off' in '\n'.join(t.screen.display)
        t.send('6\r', 'How should morph think?')
        context(t, 'Text')
        t.send('2\r', 'Model settings')
        t.send('\r', 'Review your setup')
        assert thinking_value(t.save(), 'text') == 'enabled'

        # Custom endpoints edit only the selected field and validate URLs/limits.
        t = start('custom', ready=True)
        t.send('2\r', 'Model settings')
        t.send('2\r', 'Choose your provider')
        t.send('4\r', 'Model settings')
        t.send('3\r', 'Model name / endpoint ID')
        t.send('模型甲\x7f乙\r', 'Model settings')
        t.send('5\r', 'Advanced settings')
        t.send('3\r', 'API base URL')
        t.send('invalid\r', 'Enter a URL starting')
        t.send('\x15http://localhost:1234/v1\r', 'Advanced settings')
        t.send('5\r', 'Context window (tokens)')
        t.send('\x150\r', 'Invalid token count')
        t.send('\x1564000\r', 'Advanced settings')
        t.send('6\r', 'Max output (tokens)')
        t.send('\x1564000\r', 'Invalid token count')
        t.send('\x154096\r', 'Advanced settings')
        t.send('\r', 'Model settings')
        t.send('\r', 'Review your setup')
        config = t.save(1)
        assert 'model = "模型乙"' in config
        assert 'context_limit = 64000' in config and 'max_tokens = 4096' in config

        # Saving cannot bypass metadata required by a custom provider.
        t = start('complete-custom', ready=True)
        t.send('2\r', 'Model settings')
        t.send('2\r', 'Choose your provider')
        t.send('4\r', 'Model settings')
        t.send('\r', 'Review your setup')
        t.send('8\r', 'Advanced settings')
        context(t, 'Text')
        assert not t.path.exists()
        t.send('\r', 'Model name / endpoint ID')
        t.send('custom-deployment\r', 'Advanced settings')
        t.send('\r', 'API base URL')
        t.send('http://localhost:1234/v1\r', 'Advanced settings')
        t.send('\r', 'Review your setup')
        assert 'custom-deployment' in t.save(1)

        # Optional capabilities stay off until explicitly enabled and can be removed.
        t = start('optional', ready=True)
        t.send('3\r', 'Enable this model?')
        context(t, 'Vision')
        t.send('\x1b', 'Review your setup')
        t.send('3\r', 'Enable this model?')
        t.send('\r', 'Model settings')
        t.send('\r', 'Review your setup')
        assert 'Vision model · gpt-4o' in '\n'.join(t.screen.display)
        t.send('3\r', 'Model settings')
        t.send('6\r', 'Review your setup')
        assert 'Vision model · Off' in '\n'.join(t.screen.display)
        assert 'model = ""' in section(t.save(), 'vision')

        t = start('vision-isolation', ready=True)
        t.send('3\r', 'Enable this model?')
        t.send('\r', 'Model settings')
        t.send('3\r', 'Model name / endpoint ID')
        context(t, 'Vision')
        t.send('\x15vision-fixture\r', 'Model settings')
        t.send('\r', 'Review your setup')
        config = t.save()
        assert 'model = "vision-fixture"' in section(config, 'vision')
        assert 'model = "gpt-4o"' in section(config, 'text')

        t = start('change-role', ready=True)
        t.send('6\r', 'Change your role')
        t.send('7\r', 'Review your setup')
        for kind in ('Text', 'Vision', 'Image', 'Video'):
            assert f'{kind} model ·' in '\n'.join(t.screen.display)
        assert 'model = ""' not in t.save()

        # A small terminal keeps category and draft visible through resizing.
        t = start('resize', ready=True)
        t.send('2\r', 'Model settings')
        t.send('3\r', 'Model name / endpoint ID')
        t.send('\x15中文-draft', '中文-draft')
        for rows, columns in ((12, 36), (6, 28), (24, 80)):
            t.resize(rows, columns)
            t.wait('中文-draft')
            context(t, 'Text')
        t.send('\x1b', 'Model settings')
        t.send('\x1b', 'Review your setup')
        assert 'model = "gpt-4o"' in t.save()

        for name, kwargs in (('no-color', {'flags': ('--no-color',)}),
                             ('no-color-env', {'no_color_env': True})):
            t = start(name, ready=True, **kwargs)
            t.save()
            assert not re.search(r'\x1b\[[0-9;]*m', t.transcript)

        t = start('menu-paste', ready=True)
        t.send('\x1b[200~1\n\x1b[201~', 'Review your setup')
        assert not t.path.exists()
        t.child.send('\x03')
        t.finish(-errno.ECANCELED)

        for name, keys in (('cancel-review', '9\r'), ('cancel-key', '\x03')):
            t = start(name, ready=True)
            t.child.send(keys)
            t.finish(-errno.ECANCELED)
            assert not t.path.exists()

        t = start('cancel-edit', ready=True)
        t.send('2\r', 'Model settings')
        t.send('3\r', 'Model name / endpoint ID')
        t.child.send('\x04')
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()

        t = start('signal', ready=True)
        os.kill(t.child.pid, signal.SIGTERM)
        t.finish(-errno.ECANCELED)
        assert not t.path.exists()

        # Reconfiguration keeps custom instructions without another prompt dialog.
        path = directory / 'reconfigure.toml'
        path.write_text("[model.text]\nprovider='openai'\nmodel='gpt-4o'\n"
                        "api_base='https://api.openai.com/v1'\napi_key='fixture-old-secret'\n"
                        "[prompt]\nmode='append'\nsystem_prompt_file='/keep/custom.md'\n")
        original = path.read_text()
        t = Terminal(production, path, production=True, setup_command=True)
        terminals.append(t)
        assert 'Instructions · Keep existing' in '\n'.join(t.screen.display)
        assert 'Prompt instructions' not in t.transcript
        t.send('2\r', 'Model settings')
        t.send('4\r', 'Connect your account')
        context(t, 'Text')
        t.send('2\r', 'API key environment variable')
        t.send('SETUP_MISSING_API_KEY\r', 'Model settings')
        t.send('\r', 'Review your setup')
        config = t.save(1)
        assert missing(config, 'text', 'api_key')
        assert 'fixture-old-secret' not in t.transcript
        assert any(p.read_text() == original for p in directory.glob(path.name + '.backup-*'))

        t = Terminal(production, path, production=True, setup_command=True)
        terminals.append(t)
        t.send('2\r', 'Model settings')
        t.send('4\r', 'Connect your account')
        t.send('1\r', 'Paste your API key')
        t.send('fixture-new-secret\r', 'Model settings')
        t.send('\r', 'Review your setup')
        config = t.save()
        assert missing(config, 'text', 'api_key_env')
        assert 'fixture-new-secret' not in t.transcript

        # Prompt replacement is opt-in from the overview, without an extra first-run step.
        t = Terminal(production, path, production=True, setup_command=True)
        terminals.append(t)
        t.send('6\r', 'Change your role')
        t.send('5\r', 'Review your setup')
        t.send('7\r', 'Prompt instructions')
        t.send('2\r', 'Review your setup')
        config = t.save()
        assert 'mode="replace"' in config or 'mode = "replace"' in config
        assert '/keep/custom.md' not in config

        # Saving after a concurrent change reports failure and keeps the external edit.
        t = Terminal(production, path, production=True, ready=True, setup_command=True)
        terminals.append(t)
        changed = path.read_text() + '\n# external edit\n'
        path.write_text(changed)
        t.child.send('\r')
        t.child.expect(pexpect.EOF)
        t.child.close()
        assert t.child.exitstatus == 1 and path.read_text() == changed

        # Production first run and explicit setup both save and exit before runtime init.
        for explicit in (False, True):
            path = directory / f'production-{explicit}.toml'
            t = Terminal(production, path, production=True, ready=True, setup_command=explicit)
            terminals.append(t)
            t.save()
            assert not (path.parent / '.morph/data.db').exists()
        # Receipts, completion notes and wrapped Unicode paths share a stable margin.
        for width in (120, 80, 60):
            path = directory / (f'alignment-{width}-配置-' + 'x' * 70 + '.toml')
            path.write_text("[model.text]\nprovider='openai'\nmodel='gpt-4o'\n"
                            "api_base='https://api.openai.com/v1'\napi_key='fixture-key'\n")
            t = Terminal(production, path, production=True, setup_command=True, size=(30, width))
            terminals.append(t)
            t.send('2\r', 'Model settings')
            t.send('3\r', 'Model name / endpoint ID')
            t.send('\x15long-model-' + 'x' * 70 + '-tail\r', 'Model settings')
            t.send('\r', 'Review your setup')
            t.save()
            lines = t.history_text().splitlines()
            receipts = [line for line in lines if re.search(r'✓ (Role|Text|Review)\s+·', line)]
            assert len(receipts) == 3, t.history_text()
            assert {line.index('·') for line in receipts} == {11}, receipts
            review = next(i for i, line in enumerate(lines) if '✓ Review' in line)
            completion = [line for line in lines[review + 1:] if line.strip()]
            assert completion and all(line.startswith('  ') for line in completion), completion
            assert any(line.startswith('  Previous configuration') for line in completion)
            assert any(line.startswith('  Configuration saved') for line in completion)
            if width < 100:
                assert any(line.startswith(' ' * 13) and '-tail' in line for line in lines)
            assert '\ufffd' not in t.transcript
        print(f'Setup PTY: {len(terminals)} scenarios passed, including production', flush=True)
    finally:
        for t in terminals:
            t.close()


if __name__ == '__main__':
    with tempfile.TemporaryDirectory(prefix='morph-setup-pty-') as tmp:
        tests(Path(sys.argv[1]).resolve(), Path(tmp), Path(sys.argv[2]).resolve())
