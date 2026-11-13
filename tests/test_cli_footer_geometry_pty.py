"""Small terminals must keep editable input above the optional task footer."""
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import time

from test_cli_pty import Terminal, pexpect, pyte, terminal_environment


def all_rows(terminal):
    history = [''.join(cell.data for _, cell in sorted(row.items()))
               for row in terminal.screen.history.top]
    return history + terminal.screen.display


def check(terminal, height, tasks, draft, label):
    terminal.pump(.15)
    deadline = time.monotonic() + 5
    while 2026 << 5 in terminal.screen.mode:
        assert time.monotonic() < deadline, terminal.raw[-4000:]
        terminal.pump(.03)
    rows = all_rows(terminal)
    prompts = [index for index, row in enumerate(rows)
               if row.startswith(('› ', '> '))]
    hints = [index for index, row in enumerate(rows) if 'Ctrl+T transcript' in row]
    footers = [index for index, row in enumerate(rows) if 'perm: ' in row]
    task_rows = [index for index, row in enumerate(rows) if 'Tasks ' in row]
    diagnostic = (f'{label}: cursor={terminal.screen.cursor.y},'
                  f'{terminal.screen.cursor.x}\n' +
                  '\n'.join(f'{index:03}: {row}' for index, row in enumerate(rows)))
    assert len(prompts) == 1, diagnostic
    assert not terminal.screen.cursor.hidden, diagnostic
    prompt = prompts[0] - len(terminal.screen.history.top)
    last_row = (2 + len(draft)) // terminal.screen.columns
    assert prompt >= 0, diagnostic
    assert terminal.screen.cursor.y == prompt + last_row, diagnostic
    assert len(hints) == 0, diagnostic
    assert len(footers) == int(last_row + 2 < height), diagnostic
    assert len(task_rows) == int(tasks and last_row + 3 < height), diagnostic
    print(f'checked: {label}', flush=True)


def exercise(driver, height, tasks):
    with tempfile.TemporaryDirectory(prefix='morph-footer-geometry-') as temporary:
        directory = Path(temporary)
        config = directory / 'config.toml'
        database = directory / 'data.db'
        config.write_text('''[features]
hooks = false
[memory]
enabled = false
[model.text]
provider = "openai"
model = "pty-test"
api_base = "http://127.0.0.1:9/v1"
api_key_env = "MORPH_PTY_API_KEY"
retry_count = 0
''')
        environment = terminal_environment(directory)
        arguments = [str(config), str(directory), str(database)]
        bootstrap = subprocess.run([str(driver), *arguments], input='/quit\n', text=True,
                                   capture_output=True, env=environment, timeout=10)
        assert bootstrap.returncode == 0, bootstrap.stdout + bootstrap.stderr
        if tasks:
            with sqlite3.connect(database) as connection:
                connection.execute('''INSERT INTO scheduled_tasks
                    (title,kind,status,trigger_type,next_run_at,action_type,created_at,updated_at)
                    VALUES ('footer geometry','reminder','pending','once',4102444800,
                            'reminder',1,1)''')
        terminal = Terminal.__new__(Terminal)
        terminal.child = pexpect.spawn(str(driver), arguments, env=environment,
                                       encoding='utf-8', dimensions=(height, 40), timeout=8)
        terminal.screen = pyte.HistoryScreen(40, height, history=2000)
        terminal.stream = pyte.Stream(terminal.screen)
        terminal.raw = ''
        terminal.duplicate_prompts = []
        terminal.snapshots = []
        terminal.queued_states = []
        label = f'{height} rows, tasks={tasks}'
        try:
            terminal.wait_for_composer()
            terminal.pump(1.1)
            check(terminal, height, tasks, '', f'{label}, idle')
            terminal.send('\x1b[O\x1b[I')
            check(terminal, height, tasks, '', f'{label}, focus')
            draft = 'x' * 43
            terminal.send(draft)
            check(terminal, height, tasks, draft, f'{label}, wrapped draft')
            terminal.send('\x15')
            check(terminal, height, tasks, '', f'{label}, cleared draft')
            terminal.send('x' * 160 + 'TAIL')
            screen = terminal.snapshot(f'{label}, draft exceeds viewport')
            cursor = terminal.screen.cursor
            assert not cursor.hidden and 0 <= cursor.y < height, screen
            assert terminal.screen.display[cursor.y][:cursor.x].endswith('TAIL'), screen
            terminal.send('\x01')
            screen = terminal.snapshot(f'{label}, edit beginning of long draft')
            assert not terminal.screen.cursor.hidden, screen
            assert terminal.screen.display[terminal.screen.cursor.y].startswith(('› ', '> ')), screen
            terminal.send('\x05\x15')
            check(terminal, height, tasks, '', f'{label}, cleared long draft')
            terminal.screen.resize(lines=24, columns=40)
            terminal.child.setwinsize(24, 40)
            terminal.pump(.3)
            check(terminal, 24, tasks, '', f'{label}, enlarge restores spacing')
            terminal.screen.resize(lines=height, columns=40)
            terminal.child.setwinsize(height, 40)
            terminal.pump(.3)
            check(terminal, height, tasks, '', f'{label}, shrink restores editable input')
        finally:
            terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    for height in (3, 4, 6):
        for tasks in (False, True):
            exercise(driver, height, tasks)


if __name__ == '__main__':
    main()
