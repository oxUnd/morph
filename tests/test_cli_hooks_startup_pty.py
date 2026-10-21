"""Batch hook decisions survive CLI restarts without executing hook commands."""
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
    with tempfile.TemporaryDirectory(prefix='morph-hooks-startup-', dir='/tmp') as temp:
        directory = Path(temp).resolve()
        home = directory / 'home'
        home.mkdir()
        project = directory / '.morph'
        project.mkdir()
        marker = directory / 'SHOULD_NOT_RUN'
        config = directory / 'hooks.json'
        database = directory / 'data.db'
        data = {'hooks': {'PreToolUse': [{'hooks': [
            {'type': 'command', 'command': f'touch {marker}'}]}]}}
        config.write_text(json.dumps(data))
        (project / 'hooks.json').write_text(json.dumps({'hooks': {'PostToolUse': [
            {'hooks': [{'type': 'command', 'command': f'touch {marker}'}]}]}}))

        def launch():
            terminal = Terminal(driver, directory, 1, extra_env={'HOME': str(home)})
            terminal.child.setwinsize(32, 120)
            terminal.screen = AlternateScreen(120, 32)
            terminal.stream = pyte.Stream(terminal.screen)
            terminal.pump(0.5)
            return terminal

        def stop(terminal):
            try:
                terminal.send('/quit\r')
                terminal.child.expect_exact(pexpect.EOF)
            finally:
                terminal.child.close(force=True)
            assert not marker.exists(), 'review or startup executed a hook'

        terminal = launch()
        try:
            text = terminal.snapshot('startup offers one batch decision')
            assert 'Trust lifecycle hooks?' in text and 'Reject all current hooks' in text, text
            terminal.screen.resize(lines=8, columns=30)
            terminal.child.setwinsize(8, 30)
            terminal.pump()
            terminal.send('\x1b[A\r')
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT COUNT(*) FROM hook_trust').fetchone()[0] == 0
            terminal.screen.resize(lines=32, columns=120)
            terminal.child.setwinsize(32, 120)
            terminal.pump()
            terminal.send('\x1b[A\r')
            assert not terminal.screen.cursor.hidden
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT disabled FROM hook_trust').fetchall() == [(1,), (1,)]
        finally:
            stop(terminal)

        terminal = launch()
        try:
            text = terminal.snapshot('restart remembers rejection')
            assert 'Trust lifecycle hooks?' not in text and not terminal.screen.cursor.hidden, text
            terminal.send('/hooks review-all\r')
            text = terminal.snapshot('manual batch review can replace rejection')
            assert 'Trust lifecycle hooks?' in text, text
            terminal.send('\x1b[A\x1b[A\r')
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT disabled FROM hook_trust').fetchall() == [(0,), (0,)]
                assert db.execute('SELECT path FROM hook_workspaces').fetchall() == [(str(directory),)]
        finally:
            stop(terminal)

        terminal = launch()
        try:
            text = terminal.snapshot('restart remembers trust')
            assert 'Trust lifecycle hooks?' not in text and not terminal.screen.cursor.hidden, text
        finally:
            stop(terminal)

        data['hooks']['PreToolUse'][0]['hooks'][0]['command'] += ' changed'
        config.write_text(json.dumps(data))
        terminal = launch()
        try:
            text = terminal.snapshot('changed trusted definition requires review')
            assert 'Trust lifecycle hooks?' in text and 'Changed / needs review' in text, text
            with sqlite3.connect(database) as db:
                before = db.execute('SELECT id,digest,disabled FROM hook_trust ORDER BY id').fetchall()
            data['hooks']['PreToolUse'][0]['hooks'][0]['command'] += ' changed-again'
            config.write_text(json.dumps(data))
            terminal.send('\x1b[A\x1b[A\r')
            text = terminal.snapshot('batch decision rejects an edited snapshot')
            assert 'Action failed:' in text and 'changed-again' in text, text
            with sqlite3.connect(database) as db:
                assert db.execute('SELECT id,digest,disabled FROM hook_trust ORDER BY id').fetchall() == before
            terminal.send('\x1b')
        finally:
            stop(terminal)
        print('PASS: startup batch trust/reject, restart persistence, stale snapshot guard; no hooks executed')


if __name__ == '__main__':
    main()
