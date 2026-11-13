"""Background command lifecycle through the real CLI, model, and terminal."""
import http.server
import json
from pathlib import Path
import queue
import sqlite3
import sys
import tempfile
import threading
import time

from test_cli_pty import Model, Terminal
from test_cli_transcript_pty import AlternateScreen
import pyte


def until(terminal, predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        terminal.pump(.05)
        text = '\n'.join(terminal.screen.display)
        # A marker may arrive before the synchronized frame repaints the draft.
        if predicate(text) and 2026 << 5 not in terminal.screen.mode:
            return text
    raise AssertionError('\n'.join(terminal.screen.display))


class StableComposerStream:
    """Observe every published frame, including brief contractions between snapshots."""

    def __init__(self, terminal):
        self.terminal = terminal
        self.stream = terminal.stream
        self.pending = ''
        self.positions = {}
        self.draft = None
        self.requested_draft = None
        self.frames = []

    def release_draft_guard(self):
        self.draft = None
        self.requested_draft = None
        self.positions.clear()

    def feed(self, text):
        self.pending += text
        ending = '\x1b[?2026l'
        while ending in self.pending:
            frame, self.pending = self.pending.split(ending, 1)
            self.terminal.screen.written_rows.clear()
            self.stream.feed(frame + ending)
            screen = self.terminal.screen
            rows = screen.display
            indicators = [(index, screen.marker_dim.get((index, 0)), screen.buffer[index][0].fg)
                          for index, row in enumerate(rows) if row.startswith('◉ ') and
                          any(label in row for label in ('Working ·', 'background job',
                                                         'Waiting for job ·'))]
            self.frames.append({'time': time.monotonic(), 'rows': tuple(rows),
                                'written': set(screen.written_rows), 'indicators': indicators,
                                'cursor': (screen.cursor.x, screen.cursor.y), 'raw': frame})
            for draft in ('keep my draft', 'idle draft', 'draft during wait'):
                if draft != self.requested_draft:
                    continue
                prompts = [index for index, row in enumerate(rows)
                           if row.startswith('› ' + draft)]
                key = (draft, self.terminal.screen.columns, self.terminal.screen.lines)
                if not prompts:
                    assert self.draft != draft or not any('perm: ' in row for row in rows), \
                        'draft disappeared from a published frame\n' + '\n'.join(rows)
                    continue
                assert len(prompts) == 1, '\n'.join(rows)
                self.draft = draft
                previous = self.positions.get(key, prompts[0])
                assert prompts[0] >= previous, (f'composer moved upward: {previous} -> '
                                                 f'{prompts[0]}\n' + '\n'.join(rows))
                self.positions[key] = prompts[0]
        if '\x1b[?2026h' not in self.pending:
            # Preserve possible partial escape sequences until the next read.
            split = self.pending.rfind('\x1b')
            if split < 0:
                self.stream.feed(self.pending)
                self.pending = ''
            elif self.pending[split:] not in ('\x1b', '\x1b[', '\x1b[?', '\x1b[?2',
                                              '\x1b[?20', '\x1b[?202', '\x1b[?2026'):
                self.stream.feed(self.pending)
                self.pending = ''


class PulseScreen(AlternateScreen):
    """Keep SGR faint state, which pyte deliberately omits from its cell type."""

    def reset(self):
        super().reset()
        self.faint = False
        self.marker_dim = {}
        self.written_rows = set()

    def select_graphic_rendition(self, *attrs):
        super().select_graphic_rendition(*attrs)
        codes = attrs or (0,)
        index = 0
        while index < len(codes):
            code = codes[index]
            if code in (0, 22):
                self.faint = False
            elif code == 2:
                self.faint = True
            elif code in (38, 48) and index + 1 < len(codes):
                index += 2 if codes[index + 1] == 5 else 4
            index += 1

    def draw(self, data):
        parts = data.split('◉')
        for index, part in enumerate(parts):
            if part:
                first = self.cursor.y
                super().draw(part)
                self.written_rows.update(range(min(first, self.cursor.y),
                                               max(first, self.cursor.y) + 1))
            if index + 1 < len(parts):
                super().draw('◉')
                self.written_rows.add(self.cursor.y)
                self.marker_dim[(self.cursor.y, self.cursor.x - 1)] = self.faint

    def erase_in_line(self, how=0, private=False):
        self.written_rows.add(self.cursor.y)
        super().erase_in_line(how, private)

    def erase_in_display(self, how=0, *args, **kwargs):
        self.written_rows.update(range(self.lines))
        super().erase_in_display(how, *args, **kwargs)


class JobsTerminal(Terminal):
    def send(self, text):
        if '\x15' in text or '\r' in text:
            self.stream.release_draft_guard()
        for draft in ('keep my draft', 'idle draft', 'draft during wait'):
            if draft in text:
                self.stream.requested_draft = draft
        super().send(text)


def check_collapsed(terminal):
    rows = terminal.screen.display
    for index, row in enumerate(rows):
        if 'background job' in row and '/jobs' in row and 'Waiting' not in row:
            assert row.startswith('◉ '), '\n'.join(rows)
            assert index + 1 == len(rows) or not rows[index + 1].startswith('  └ '), \
                '\n'.join(rows)


def check_pulse(terminal, marker_count, draft=None):
    # The fixture sends three initial preview chunks before becoming quiet.
    # Inspect animation after those legitimate content updates have settled.
    terminal.pump(.7)
    baseline = terminal.stream.frames[-1]
    baseline_phases = {dim for _, dim, _ in baseline['indicators']}
    assert len(baseline['indicators']) == marker_count and len(baseline_phases) == 1, baseline
    assert None not in baseline_phases, baseline
    phases = [(time.monotonic(), baseline_phases.pop())]
    start = len(terminal.stream.frames)
    terminal.pump(1.25)
    frames = terminal.stream.frames[start:]
    assert frames, 'running markers did not publish an animation update'
    for frame in frames:
        rows = frame['rows']
        diagnostic = '\n'.join(rows) + '\n' + repr(frame['raw'])
        assert len(frame['indicators']) == marker_count, diagnostic
        faint = {dim for _, dim, _ in frame['indicators']}
        assert len(faint) == 1 and None not in faint, diagnostic
        assert all(color in ('cyan', '00cdcd') for _, _, color in frame['indicators']), diagnostic
        phase = faint.pop()
        if not phases or phase != phases[-1][1]:
            phases.append((frame['time'], phase))
        status_rows = {index for index, _, _ in frame['indicators']}
        # Retained rendering may clear and redraw an individual changed row.
        # Only live status rows may be written during this quiet pulse window.
        assert frame['written'] <= status_rows, diagnostic
        assert '\x1b[2J' not in frame['raw'], diagnostic
        composer = frame['cursor'][1]
        assert not ({composer - 1, composer, composer + 1} & frame['written']), diagnostic
        if draft:
            assert any(row.startswith('› ' + draft) for row in rows), diagnostic
    assert {phase for _, phase in phases} == {False, True}, phases
    assert len(phases) >= 3, phases
    for (previous, _), (current, _) in zip(phases[1:], phases[2:]):
        assert .35 <= current - previous <= .7, phases
    text = '\n'.join(terminal.screen.display)
    status_rows = [row for row in terminal.screen.display
                   if 'Working ·' in row or 'background job' in row
                   or 'Waiting for job ·' in row]
    assert status_rows and all(row.startswith('◉ ') for row in status_rows), text
    if draft:
        assert draft in text, text
    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
    return text


def main():
    driver = Path(sys.argv[1]).resolve()
    artifacts = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    server = http.server.ThreadingHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    releases = []
    terminal = None
    try:
        with tempfile.TemporaryDirectory(prefix='morph-jobs-pty-') as temp:
            directory = Path(temp)
            terminal = JobsTerminal(driver, directory, server.server_port,
                                    extra_env={'NO_COLOR': ''})
            terminal.child.setwinsize(30, 100)
            terminal.screen = PulseScreen(100, 30)
            terminal.stream = pyte.Stream(terminal.screen)
            terminal.stream = StableComposerStream(terminal)
            until(terminal, lambda text: 'perm: ' in text)
            terminal.pump(.2)
            terminal.send('Build and inspect background work\r')
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'tool_calls': []}
            commands = [
                {'command': 'printf BUILD_STARTED; while [ ! -f release-build ]; do sleep .05; done; '
                            'printf BUILD_FINISHED; exit 7',
                 'description': '编译项目', 'background': True},
                {'command': 'printf SERVICE_STARTED; while :; do sleep .1; done',
                 'service': True, 'background': True},
            ]
            for index, args in enumerate(commands):
                release.delta['tool_calls'].append({
                    'index': index, 'id': f'background_job_{index}', 'type': 'function',
                    'function': {'name': 'exec', 'arguments': json.dumps(args)}})
            release.set()
            until(terminal, lambda text: 'Yes once' in text or '2 background jobs running' in text)
            if 'Yes once' in '\n'.join(terminal.screen.display):
                terminal.send('\x1b[B\r')
            body, release = terminal.request()
            releases.append(release)
            text = until(terminal, lambda text: '2 background jobs running · /jobs' in text)
            check_collapsed(terminal)
            check_pulse(terminal, 2)
            if artifacts:
                (artifacts / 'live.txt').write_text(text)
                cells = [[{'text': terminal.screen.buffer[row][col].data,
                           'fg': terminal.screen.buffer[row][col].fg,
                           'bg': terminal.screen.buffer[row][col].bg}
                          for col in range(100)] for row in range(30)]
                (artifacts / 'live.json').write_text(json.dumps(cells, ensure_ascii=False))
            for width in (120, 80, 60):
                terminal.child.setwinsize(30, width)
                terminal.screen.resize(lines=30, columns=width)
                text = until(terminal, lambda text: '2 background jobs running · /jobs' in text)
                check_collapsed(terminal)
            terminal.child.setwinsize(30, 100)
            terminal.screen.resize(lines=30, columns=100)
            terminal.pump(.4)
            with sqlite3.connect(directory / 'data.db') as db:
                records = db.execute('SELECT process_id,data_json FROM process_jobs').fetchall()
            assert len(records) == 2, records
            build_id = next(pid for pid, data in records if json.loads(data)['description'])
            service_id = next(pid for pid, data in records if json.loads(data)['service'])
            terminal.send('/jobs\r')
            text = until(terminal, lambda text: 'morph / jobs' in text)
            assert 'running' in text, text
            assert not any(row.startswith('› /jobs') for row in terminal.screen.display), text
            terminal.send('\x1b')
            terminal.send('/jobs ' + service_id[5:13] + '\r')
            until(terminal, lambda text: 'morph / jobs / output' in text)
            terminal.send('k')
            text = until(terminal, lambda text: 'killed' in text)
            assert 'Stop requested' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            text = terminal.snapshot('jobs panel cancellation restores the composer')
            assert 'morph / jobs' not in text, text
            terminal.send('keep my draft')
            text = terminal.snapshot('draft remains editable above the footer')
            assert 'keep my draft' in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            release.delta = {'content': 'Premature completion candidate.'}
            release.set()
            terminal.pump(.6)
            assert Model.requests.empty(), 'finite job unexpectedly accepted as completed'
            text = until(terminal, lambda text: 'Waiting for background job' in text)
            assert 'Esc to interrupt wait' in text and '/jobs' in text, text
            assert any(row.startswith('  └ ') for row in terminal.screen.display), text
            assert 'Working ·' not in text, text
            assert 'Worked for' not in '\n'.join(terminal.screen.display)
            (directory / 'release-build').touch()
            body, release = terminal.request()
            releases.append(release)
            completion = [m['content'] for m in body['messages']
                          if 'Runtime process completion records' in str(m.get('content', ''))]
            assert completion and 'BUILD_FINISHED' in completion[0], body
            assert '"exit_code":7' in completion[0], completion
            text = until(terminal, lambda text: '编译项目 · exited 7' in text and
                         'Waiting for background job' not in text)
            check_collapsed(terminal)
            assert 'keep my draft' in text, text
            assert 'Worked for' not in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            release.delta = {'content': 'Verified build failure: exit 7.'}
            release.set()
            text = until(terminal, lambda text: 'Worked for' in text)
            assert 'Verified build failure' in text, text
            assert 'keep my draft' in text, text
            assert text.count('exited 7 · /jobs') == 1, text
            with sqlite3.connect(directory / 'data.db') as db:
                data = json.loads(db.execute('SELECT data_json FROM process_jobs WHERE process_id=?',
                                             (build_id,)).fetchone()[0])
            assert data['exit_code'] == 7 and 'BUILD_FINISHED' in data['stdout'], data
            terminal.send('\x15/jobs ' + build_id[5:13] + '\r')
            text = until(terminal, lambda text: 'morph / jobs / output' in text)
            assert 'BUILD_FINISHED' in text and 'exit 7' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            text = terminal.snapshot('output panel returns without stale backgrounds')
            assert 'morph / jobs' not in text, text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            terminal.send('Start a job and notify me later\r')
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'tool_calls': []}
            for index, (description, suffix) in enumerate([
                    ('Notify-only verification', 'printf NOTIFIED'),
                    ('Notify-only failure', 'printf FAILED_NOTIFICATION; exit 9')]):
                release.delta['tool_calls'].append({
                    'index': index, 'id': f'notify_only_job_{index}', 'type': 'function',
                    'function': {'name': 'exec', 'arguments': json.dumps({
                        'command': 'while [ ! -f release-notify ]; do sleep .05; done; ' + suffix,
                        'description': description,
                        'background': True, 'notify_only': True})}})
            release.set()
            _, release = terminal.request()
            releases.append(release)
            release.delta = {'content': 'Started; I will notify you.'}
            release.set()
            text = until(terminal, lambda text: 'Started; I will notify you.' in text)
            terminal.pump(.3)
            text = until(terminal, lambda text: '2 background jobs running · /jobs' in text)
            terminal.send('idle draft')
            check_pulse(terminal, 1, 'idle draft')
            (directory / 'release-notify').touch()
            text = until(terminal, lambda text: 'Notify-only verification · exited 0' in text
                         and 'Notify-only failure · exited 9' in text)
            assert 'idle draft' in text, text
            assert Model.requests.empty(), 'notification caused an unsolicited model turn'
            text = until(terminal, lambda text: 'background jobs running' not in text)
            assert 'idle draft' in text, text
            check_collapsed(terminal)
            assert Model.requests.empty(), 'completion caused an unsolicited model turn'
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            with sqlite3.connect(directory / 'data.db') as db:
                records = db.execute('SELECT process_id,data_json FROM process_jobs').fetchall()
            failure_id, data = next((pid, json.loads(data)) for pid, data in records
                                    if json.loads(data)['description'] == 'Notify-only failure')
            assert data['exit_code'] == 9 and 'FAILED_NOTIFICATION' in data['stdout'], data
            terminal.send('\x15/jobs ' + failure_id[5:13] + '\r')
            text = until(terminal, lambda text: 'morph / jobs / output' in text)
            assert 'FAILED_NOTIFICATION' in text and 'exit 9' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            text = terminal.snapshot('expired failure remains inspectable without reappearing')
            assert not any('└─ while [ ! -f release-notify' in row
                           for row in terminal.screen.display), text
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts

            for mode in ('explicit', 'automatic', 'cancel'):
                terminal.send(f'\x15Start interruptible {mode} verification\r')
                _, release = terminal.request()
                releases.append(release)
                release.delta = {'tool_calls': [{
                    'index': 0, 'id': f'wait_exec_{mode}', 'type': 'function',
                    'function': {'name': 'exec', 'arguments': json.dumps({
                        'command': f'while [ ! -f release-{mode} ]; do sleep .05; done; '
                                   f'printf WAIT_SURVIVED_{mode}',
                        'description': f'Wait verification {mode}', 'background': True})}}]}
                release.set()
                body, release = terminal.request()
                releases.append(release)
                if mode != 'automatic':
                    with sqlite3.connect(directory / 'data.db') as db:
                        records = db.execute('SELECT process_id,data_json FROM process_jobs').fetchall()
                    job_id = next(pid for pid, raw in records
                                  if json.loads(raw)['description'] == f'Wait verification {mode}')
                    release.delta = {'tool_calls': [{
                        'index': 0, 'id': 'explicit_wait', 'type': 'function',
                        'function': {'name': 'background', 'arguments': json.dumps({
                            'action': 'wait', 'session_id': job_id, 'wait_ms': 10000})}}]}
                else:
                    release.delta = {'content': 'Candidate answer before waiting.'}
                release.set()
                until(terminal, lambda text: 'Waiting for background job' in text)
                terminal.pump(1.2)
                for width in (120, 80, 60):
                    terminal.child.setwinsize(30, width)
                    terminal.screen.resize(lines=30, columns=width)
                    text = until(terminal, lambda text: 'Waiting for background job' in text
                                 and '/jobs' in text and 'Esc' in text)
                    assert any(row.startswith('  └ ') for row in terminal.screen.display), text
                    assert 'Working ·' not in text, text
                if mode == 'cancel':
                    terminal.child.send('\x03')
                    text = until(terminal, lambda text: 'Stopped after' in text and
                                 'Waiting for background job' not in text and
                                 'background job running' not in text)
                    assert 'Wait verification cancel · killed' in text, text
                    assert Model.requests.empty(), 'cancellation started a model call'
                    continue
                terminal.send('draft during wait')
                check_pulse(terminal, 1, 'draft during wait')
                terminal.child.send('\x1b')
                text = until(terminal, lambda text: 'Stopped after' in text and
                             'Waiting for background job' not in text)
                assert 'draft during wait' in text, text
                assert '1 background job running · /jobs' in text, text
                assert Model.requests.empty(), 'interrupting the wait started a model call'
                terminal.send('\x15/jobs\r')
                text = until(terminal, lambda text: 'morph / jobs' in text)
                assert 'running' in text, text
                terminal.send('\x1b')
                (directory / f'release-{mode}').touch()
                text = until(terminal, lambda text: f'Wait verification {mode} · exited 0' in text)
                assert 'background job running' not in text, text
                assert Model.requests.empty(), 'completion started a model call'
                assert not terminal.duplicate_prompts, terminal.duplicate_prompts
    finally:
        for release in releases:
            release.set()
        if terminal:
            terminal.child.close(force=True)
        server.shutdown()


if __name__ == '__main__':
    main()
