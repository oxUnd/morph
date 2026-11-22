"""Native restore confirmation preserves data on cancel and commits only on explicit consent."""
import json
import os
from pathlib import Path
import signal
import sqlite3
import sys
import tempfile
import time

from test_cli_pty import Terminal
from test_cli_slash_pty import wait_for


def main():
    terminal = None
    with tempfile.TemporaryDirectory(prefix='morph-native-restore-') as temporary:
        directory = Path(temporary)
        remote = directory / 'remote'
        config = (f'\n[sync]\nenabled=true\ndir={json.dumps(str(remote))}\n'
                  'interval_seconds=3600\ninclude=["home/.morph/data.db"]\n')
        try:
            terminal = Terminal(Path(sys.argv[1]).resolve(), directory, 1,
                                production=True, hooks_enabled=False, extra_config=config)
            terminal.wait_for_composer()
            database = directory / 'home/.morph/data.db'
            with sqlite3.connect(database) as connection:
                connection.execute('CREATE TABLE native_confirm_fixture(value TEXT)')
                connection.execute("INSERT INTO native_confirm_fixture VALUES('original')")
            terminal.send('/sync now\r')
            wait_for(terminal, lambda: 'sync completed' in '\n'.join(terminal.screen.display))
            snapshots = list((remote / '.morph-sync/db/v1/snapshots').glob('*.json'))
            assert snapshots, 'sync produced no immutable database backup'
            snapshot = json.loads(max(snapshots, key=lambda item: item.stat().st_mtime_ns).read_text())
            snapshot_id = snapshot['snapshot_id']
            with sqlite3.connect(database) as connection:
                connection.execute("INSERT INTO native_confirm_fixture VALUES('newer')")

            def values():
                with sqlite3.connect(database) as connection:
                    return [row[0] for row in connection.execute(
                        'SELECT value FROM native_confirm_fixture ORDER BY rowid')]

            def open_dialog():
                terminal.send(f'/sync restore-db {snapshot_id}\r')
                wait_for(terminal, lambda: 'Confirm this action?' in
                         '\n'.join(terminal.screen.display) and 'Enter confirm' in
                         '\n'.join(terminal.screen.display))

            for key in ('\r', '\x1b'):
                open_dialog()
                terminal.send(key)
                wait_for(terminal, lambda: 'Enter confirm' not in '\n'.join(terminal.screen.display))
                assert values() == ['original', 'newer'], values()
                assert not list(directory.rglob('*.restore_*.tmp')), 'cancel retained a staged database'
                assert not list((directory / '.morph-sync/restore').glob('*.json'))

            open_dialog()
            terminal.send('\x1b[57353;1:1u\x1b[57353;1:3u')
            focused = [row.strip() for row in terminal.screen.display
                       if row.lstrip().startswith('› ●')]
            assert focused == ['› ● 2. Confirm'], focused
            terminal.send('\r')
            wait_for(terminal, lambda: 'database restored; Morph restarted' in terminal.raw)
            wait_for(terminal, lambda: 'perm:' in '\n'.join(terminal.screen.display))
            assert values() == ['original'], values()

            with sqlite3.connect(database) as connection:
                connection.execute("INSERT INTO native_confirm_fixture VALUES('after restart')")
            open_dialog()
            os.kill(terminal.child.pid, signal.SIGTERM)
            deadline = time.monotonic() + 6
            while terminal.child.isalive() and time.monotonic() < deadline:
                terminal.pump(.05)
            assert not terminal.child.isalive(), 'shutdown blocked on restore confirmation'
            assert values() == ['original', 'after restart'], values()
            assert not list(directory.rglob('*.restore_*.tmp')), 'shutdown retained a staged database'
            assert not list((directory / '.morph-sync/restore').glob('*.json'))
            print('PASS: restore cancel, deny, modern keys, explicit commit, restart and pending shutdown')
        finally:
            if terminal is not None:
                terminal.child.close(force=True)


if __name__ == '__main__':
    main()
