"""Inspect and manage hooks using the production CLI, PTY, and terminal emulator."""
import json
from pathlib import Path
import sqlite3
import sys
import tempfile

from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen
import pexpect
import pyte


def main():
    driver = Path(sys.argv[1]).resolve()
    artifacts = Path(sys.argv[2]) if len(sys.argv) > 2 else None
    if artifacts:
        artifacts.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='morph-hooks-pty-', dir='/tmp') as temp:
        directory = Path(temp).resolve()
        home = directory / 'home'
        home.mkdir()
        project = directory / '.morph'
        project.mkdir()
        marker = directory / 'SHOULD_NOT_RUN'
        command = f'touch {marker}'
        events = ['PreToolUse', 'PostToolUse', 'UserPromptSubmit',
                  'SessionStart', 'Stop', 'SessionEnd']
        data = {'hooks': {event: [{'matcher': '^Bash$' if 'ToolUse' in event else '',
                                 'hooks': [{'type': 'command', 'command': command,
                                            'timeout': 3}]}] for event in events}}
        config = directory / 'hooks.json'
        config.write_text(json.dumps(data))
        (project / 'hooks.json').write_text(json.dumps({'hooks': {'PreToolUse': [
            {'matcher': 'project-only', 'hooks': [{'type': 'command', 'command': command}]}]}}))
        terminal = Terminal(driver, directory, 1, extra_env={'HOME': str(home)})
        terminal.child.setwinsize(40, 120)
        terminal.screen = AlternateScreen(120, 40)
        terminal.stream = pyte.Stream(terminal.screen)
        database = directory / 'data.db'

        def snapshot(label):
            text = terminal.snapshot(label)
            assert not marker.exists(), 'inspection or trust executed a hook'
            assert text.count('morph') >= 1, text
            if artifacts:
                (artifacts / f'{len(terminal.snapshots):02d}.txt').write_text(text)
            return text

        def trust_rows():
            with sqlite3.connect(database) as db:
                return db.execute("SELECT id,digest,disabled,definition FROM hook_trust WHERE digest<>'' OR disabled=1").fetchall()

        def review():
            terminal.send('\r')
            text = snapshot('open review')
            assert '/ hooks / review' in text and 'Review decision' in text, text
            assert 'Trust current definition' in text, text
            selected = [r for r in range(terminal.screen.lines)
                        if terminal.screen.buffer[r][2].bg == '1d3038']
            assert len(selected) == 1, text
            assert 'Keep unchanged' in terminal.screen.display[selected[0]], text
            return text

        def choose_first():
            # The final/back item is initially selected; Home scrolls the document.
            text = '\n'.join(terminal.screen.display)
            count = 3 if 'Disable hook' in text or 'Enable hook' in text else 2
            terminal.send('\x1b[A' * (count - 1) + '\r')
            return snapshot('confirm explicitly selected action')

        def resize(rows, columns):
            terminal.screen.resize(lines=rows, columns=columns)
            terminal.child.setwinsize(rows, columns)
            terminal.pump(0.5)
            return snapshot(f'resize without keyboard input: {rows}x{columns}')

        try:
            terminal.pump(1)
            text = snapshot('startup batch review')
            assert 'Trust lifecycle hooks?' in text and 'Reject all current hooks' in text, text
            terminal.send('\x1b')
            terminal.send('/hooks\r')
            text = snapshot('hook list')
            assert text.count('Lifecycle hooks') == 1, text
            assert 'COMMAND' in text and 'SOURCE' in text and 'STATE' in text, text
            assert 'Needs review' in text and 'Workspace untrusted' in text, text
            assert not trust_rows()
            text = review()
            assert 'Current definition' in text and command in text, text
            text = choose_first()
            assert 'Completed: trust' in text and 'Revoke trust' in text, text
            original = trust_rows()[0]
            assert original[2] == 0 and json.loads(original[3])['command'] == command
            # Changed definitions show removed and added values.
            terminal.send('\x1b')
            data['hooks']['PreToolUse'][0]['hooks'][0]['command'] = command + ' --strict'
            config.write_text(json.dumps(data))
            text = review()
            assert 'Definition changes' in text and '--strict' in text, text
            assert any(terminal.screen.buffer[r][2].bg == '31181a' for r in range(40)), text
            assert any(terminal.screen.buffer[r][2].bg == '162e21' for r in range(40)), text
            # Editing after review must fail the digest check, then show refreshed content.
            newest = command + ' --newest'
            data['hooks']['PreToolUse'][0]['hooks'][0]['command'] = newest
            config.write_text(json.dumps(data))
            text = choose_first()
            assert 'Action failed:' in text and '--newest' in text, text
            assert trust_rows()[0][1] == original[1]
            text = choose_first()
            assert 'Completed: trust' in text, text
            assert json.loads(trust_rows()[0][3])['command'] == newest
            # Trusted hooks offer revoke and disable, with enable on disabled hooks.
            terminal.send('\x1b[A\r')
            text = snapshot('disable trusted hook')
            assert 'Completed: disable' in text and 'Enable hook' in text, text
            assert trust_rows()[0][2] == 1
            text = choose_first()
            assert 'Completed: enable' in text and trust_rows()[0][2] == 0, text
            text = choose_first()
            assert 'Completed: revoke' in text and not trust_rows(), text
            # A terminal too small to display the review cannot accept trust.
            resize(8, 30)
            terminal.send('\x1b[A\r')
            assert not trust_rows()
            resize(40, 120)
            terminal.send('\x1b')
            terminal.send('\x1b[F\r')
            text = snapshot('project workspace review')
            assert 'Workspace untrusted' in text and 'Trust this workspace' in text, text
            text = choose_first()
            assert 'Completed: trust-project' in text and 'Trust current definition' in text, text
            assert not trust_rows()
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT path FROM hook_workspaces').fetchall() == [(str(directory),)]
            text = choose_first()
            assert 'Completed: trust' in text and len(trust_rows()) == 1, text
            # Long definitions remain readable through document scrolling and resizing.
            terminal.send('\x1b')
            long_command = command + ' ' + '中文-long-argument ' * 80
            data['hooks']['SessionEnd'][0]['hooks'][0]['command'] = long_command
            config.write_text(json.dumps(data))
            terminal.send('\x1b[H' + '\x1b[B' * 5 + '\r')
            text = snapshot('long command review')
            assert 'Lines ' in text, text
            terminal.send('\x1b[F')
            text = snapshot('scroll to end of full definition')
            assert 'Last execution' in text and 'Not run' in text, text
            terminal.send('\x1b[H')
            text = snapshot('scroll back to metadata')
            assert 'ID' in text and 'Command' in text, text
            resize(24, 60)
            terminal.send('\x1b[6~')
            snapshot('page through narrow review')
            resize(40, 120)
            terminal.send('\x1b')
            text = snapshot('back to list')
            assert 'Lifecycle hooks' in text and '/ hooks / review' not in text, text
            resize(24, 60)
            text = snapshot('compact hook list')
            assert 'STATE' in text and 'SOURCE' not in text, text
            terminal.send('\x1b')
            resize(40, 120)
            terminal.send('/new after-hooks\r')
            text = terminal.snapshot('composer restored after hooks')
            assert 'created and switched to session: after-hooks' in text, text
            project_id = trust_rows()[0][0]
            terminal.send('/hooks review ' + project_id + '\r')
            text = snapshot('direct review by ID')
            assert '/ hooks / review' in text and 'PreToolUse' in text, text
            terminal.send('\x1b')
            terminal.send('\x1b')
            terminal.send('/hooks check\r')
            text = terminal.snapshot('plain check remains available')
            assert 'Hooks (7)' in text and not terminal.screen.cursor.hidden, text
            config.write_text('{"hooks": {}}')
            (project / 'hooks.json').write_text('{"hooks": {}}')
            terminal.send('/hooks\r')
            resize(48, 160)
            text = snapshot('empty hooks have a dedicated compact empty state')
            assert 'No hooks configured.' in text, text
            assert 'Showing' not in text and 'SOURCE' not in text and 'STATE' not in text, text
            assert 'need review' not in text and 'Enter Review' not in text, text
            assert 'hooks.json' in text and 'r Refresh' in text, text
            terminal.send('\x1b[C\x1b[B\r')
            assert 'No hooks configured.' in snapshot('empty navigation stays empty')
            one = {'hooks': {'PreToolUse': data['hooks']['PreToolUse']}}
            config.write_text(json.dumps(one))
            terminal.send('r')
            text = snapshot('refresh empty view into single hook')
            assert 'Showing 1–1 of 1' in text and 'No hooks configured.' not in text, text
            lines = text.splitlines()
            header = next(i for i, line in enumerate(lines) if 'EVENT' in line and 'STATE' in line)
            summary = next(i for i, line in enumerate(lines) if 'Showing' in line)
            assert summary == header + 2, text
            assert '←→ Page' not in text, text
            many = {'hooks': {'PreToolUse': [dict(one['hooks']['PreToolUse'][0]) for _ in range(25)]}}
            config.write_text(json.dumps(many))
            terminal.send('r')
            terminal.send('\x1b[F')
            text = snapshot('partial last page collapses unused rows')
            lines = text.splitlines()
            summary = next(i for i, line in enumerate(lines) if 'Showing' in line)
            header = next(i for i, line in enumerate(lines) if 'EVENT' in line and 'STATE' in line)
            visible = sum('PreToolUse' in line for line in lines[header + 1:summary])
            assert summary == header + 1 + visible, text
            assert 'of 25' in lines[summary], text
            terminal.send('\x1b')
            terminal.send('/hooks\r')
            terminal.send('\x03')
            assert not terminal.screen.cursor.hidden
            terminal.send('/quit\r')
            terminal.child.expect_exact(pexpect.EOF)
            assert not marker.exists()
            print('PASS: hooks list/review, diff, digest guard, trust/revoke/disable/enable, '
                  'workspace trust, scrolling, resizing, terminal restoration; no hooks executed')
        finally:
            if artifacts:
                (artifacts / 'terminal.raw').write_text(terminal.raw)
                (artifacts / 'last-screen.txt').write_text('\n'.join(terminal.screen.display))
            terminal.child.close(force=True)


if __name__ == '__main__':
    main()
