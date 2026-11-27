"""Assertions shared by composer replacement panel PTY regressions."""
import re
import time


def conversation_rows(rows, composer):
    """Exclude all four dock regions when reconstructing scrolling history."""
    header = re.compile(r'^[◉◯?] (?:Working|Waiting|Retrying|Reconnecting|Running|Tasks pending|'
                        r'Agents running|Background jobs|Other sessions|Connecting MCP|'
                        r'Compressing|Generating recap|Naming session|Cancelling)')
    end = composer - 2
    cursor = composer - 1
    while cursor >= 0 and not rows[cursor].strip():
        cursor -= 1
    if cursor >= 0 and (rows[cursor].startswith('  └─ ') or header.match(rows[cursor])
                        or '↓ Back to bottom' in rows[cursor]):
        while cursor >= 0 and (not rows[cursor].strip() or rows[cursor].startswith('  └─ ')
                               or header.match(rows[cursor])
                               or '↓ Back to bottom' in rows[cursor]):
            cursor -= 1
        end = cursor + 1
    result = list(rows[:max(0, end)])
    while result and not result[-1].strip():
        result.pop()
    return result


def stable_rows(terminal):
    terminal.pump(.08)
    deadline = time.monotonic() + 5
    while 2026 << 5 in terminal.screen.mode:
        assert time.monotonic() < deadline, 'terminal frame did not finish'
        terminal.pump(.03)
    return terminal.screen.display


def no_screen_switch(raw):
    # A retained frame can repaint after resize; it must keep the same buffer.
    forbidden = re.compile(
        r'\x1b\[\?(?:47|1047|1049)[hl]|\x1bc')
    found = forbidden.search(raw)
    assert not found, f'inline panel used full-screen control {found.group()!r}'


def inline_panel(terminal, title, history_marker, maximum=12, command=None):
    rows = stable_rows(terminal)
    diagnostic = '\n'.join(rows)
    headers = [index for index, row in enumerate(rows) if title in row]
    assert len(headers) == 1, diagnostic
    start = headers[0]
    marker = next((index for index, row in enumerate(rows) if history_marker in row), None)
    assert marker is not None and marker < start, diagnostic
    if command:
        commands = [index for index, row in enumerate(rows) if row.rstrip() == command]
        assert commands and commands[-1] < start, diagnostic
        between = rows[commands[-1] + 1:start]
        assert between and not between[0].strip(), diagnostic
        activity = [row for row in between if row.strip()]
        if activity:
            assert activity[0].startswith(('◉ ', '◯ ', '? ')), diagnostic
            assert all(row.startswith('  └─ ') for row in activity[1:]), diagnostic
        else:
            assert start == commands[-1] + 2, diagnostic
    # The shared component parks at its upper edge, never below its last option.
    # Growth reserves rows beneath this anchor and may scroll history for space.
    assert terminal.screen.cursor.y == start, diagnostic
    assert terminal.screen.cursor.x == 0, diagnostic
    end = max((index for index, row in enumerate(rows) if row.strip() and 'perm: ' not in row),
              default=start)
    assert 'perm: ' in rows[-1], diagnostic
    assert end - start + 1 <= maximum, diagnostic
    # Composer panels paint through the usable terminal width, including empty cells.
    for index in range(start, end + 1):
        first = terminal.screen.buffer[index][0]
        if first.bg != 'default':
            assert terminal.screen.buffer[index][terminal.screen.columns - 2].bg == first.bg, diagnostic
    return rows[start:end + 1]


def restored_composer(terminal, history_marker, expected='', command=None):
    rows = stable_rows(terminal)
    diagnostic = '\n'.join(rows)
    footers = [index for index, row in enumerate(rows) if 'perm: ' in row]
    assert len(footers) == 1, diagnostic
    row = footers[0] - 2
    assert row >= 1 and rows[row].startswith(('› ', '» ')), diagnostic
    boundary = max((index for index in range(row - 1)
                    if rows[index].startswith('/') or 'Worked for ' in rows[index]
                    or 'Working ·' in rows[index] or 'Stopped after' in rows[index]
                    or history_marker in rows[index]), default=-1)
    if command:
        commands = [index for index in range(row) if rows[index].rstrip() == command]
        assert commands, diagnostic
    editable = [index for index in range(boundary + 1, len(rows))
                if rows[index].startswith(('› ', '» '))]
    assert len(editable) == 1, diagnostic
    assert editable[0] == row, diagnostic
    assert expected in rows[row], diagnostic
    assert history_marker in diagnostic, diagnostic
    backgrounds = [index for index in range(boundary + 1, len(rows))
                   if terminal.screen.buffer[index][0].bg == '3a3a3a']
    assert backgrounds == [row - 1, row, row + 1], (backgrounds, diagnostic)
    assert not any(terminal.screen.buffer[y][x].bg in ('blue', '0000ee')
                   for y in range(boundary + 1, len(rows))
                   for x in range(terminal.screen.columns)), diagnostic
    return row
