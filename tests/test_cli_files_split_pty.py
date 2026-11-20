"""File-sidebar native split dragging, pointer shapes and opening animation."""
import os
from pathlib import Path
import sys
import tempfile

from test_cli_files_pty import display, resize, wait_for
from test_cli_native_media_pty import TextStream
from test_cli_pty import Terminal


def divider(frame):
    return next((line.index('│') for line in frame.splitlines() if '│' in line), None)


def motion(terminal, button, column, row=6, release=False):
    terminal.send(f'\x1b[<{button};{column + 1};{row + 1}{"m" if release else "M"}')


def opening_positions(terminal, start):
    positions = [divider(frame) for frame, _ in terminal.published_frames[start:]]
    return [column for column in positions if column is not None]


def check_opening(terminal, start, target, animated):
    positions = opening_positions(terminal, start)
    assert positions and positions[-1] == target, (positions, display(terminal))
    if animated:
        assert len(set(positions)) >= 3, positions
        assert all(first >= second for first, second in zip(positions, positions[1:])), positions
    else:
        assert set(positions) == {target}, positions


def run_case(binary, modern, plain):
    with tempfile.TemporaryDirectory(prefix='morph-files-split-') as temp:
        directory = Path(temp)
        (directory / 'pane.c').write_text('const int SPLIT_CONTENT = 7;\n' + '\n'.join(
            f'int split_row_{line} = {line};' for line in range(100)), encoding='utf-8')
        terminal = Terminal(binary, directory, 12345, production=True, hooks_enabled=False,
                            extra_env={'TERM_PROGRAM': 'ghostty' if modern else '',
                                       'NO_COLOR': '1' if plain else ''},
                            extra_arguments=('--no-color',) if plain else ())
        terminal.stream = TextStream(terminal.screen)
        try:
            resize(terminal, 120)
            terminal.wait_for_composer()
            start = len(terminal.published_frames)
            terminal.send('/files pane.c\r')
            wait_for(terminal, lambda text: 'SPLIT_CONTENT' in text, 'open file sidebar')
            terminal.pump(.2)
            original = divider(display(terminal))
            check_opening(terminal, start, original, not plain)

            row = 6
            normal = terminal.screen.buffer[row][original]
            output = len(terminal.raw)
            motion(terminal, 35, original, row)
            hovered = terminal.screen.buffer[row][original]
            if not plain:
                assert hovered.fg != normal.fg, 'native divider should highlight'

            pointer = '\x1b]22;ew-resize\x1b\\'
            if modern:
                assert pointer in terminal.raw[output:], terminal.raw[output:]
            else:
                assert pointer not in terminal.raw, 'unsupported terminals use hover/drag only'

            output = len(terminal.raw)
            motion(terminal, 35, original - 3, row)
            if modern:
                assert '\x1b]22;default\x1b\\' in terminal.raw[output:]

            motion(terminal, 0, original, row)
            motion(terminal, 32, 47, row)
            resized = divider(display(terminal))
            assert resized != original and abs(resized - 47) <= 1, display(terminal)
            assert 'SPLIT_CONTENT' in display(terminal), display(terminal)
            motion(terminal, 0, 47, row, release=True)
            assert divider(display(terminal)) == resized, display(terminal)

            terminal.send('\x06SPLIT')
            wait_for(terminal, lambda text: 'Find: SPLIT' in text, 'drag retains viewer focus')
            terminal.send('\x1b')
            terminal.send('\x0fUNSENT_SPLIT_DRAFT ' + 'keep ' * 16)
            wait_for(terminal, lambda text: 'UNSENT_SPLIT_DRAFT' in text, 'chat draft')
            main_pane = '\n'.join(line.split('│', 1)[0] for line in terminal.screen.display)
            assert 'UNSENT_SPLIT_DRAFT' in main_pane, display(terminal)
            motion(terminal, 0, resized, row)
            motion(terminal, 32, 68, row)
            motion(terminal, 0, 68, row, release=True)
            chosen = divider(display(terminal))
            assert abs(chosen - 68) <= 1, display(terminal)
            assert display(terminal).count('UNSENT_SPLIT_DRAFT') == 1, display(terminal)
            terminal.send('Z')
            assert display(terminal).count('UNSENT_SPLIT_DRAFT') == 1, display(terminal)

            terminal.send('\x0f\x1b')
            wait_for(terminal, lambda text: 'Files (1)' in text, 'return to list')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: divider(text) is None, 'close split')
            assert display(terminal).count('UNSENT_SPLIT_DRAFT') == 1, display(terminal)
            terminal.send('\x15')
            start = len(terminal.published_frames)
            terminal.send('/files pane.c\r')
            wait_for(terminal, lambda text: 'SPLIT_CONTENT' in text, 'reopen at chosen width')
            terminal.pump(.2)
            check_opening(terminal, start, chosen, not plain)

            for columns in (80, 60):
                resize(terminal, columns)
                assert divider(display(terminal)) is None, display(terminal)
                assert 'SPLIT_CONTENT' in display(terminal), display(terminal)

            resize(terminal, 120)
            assert divider(display(terminal)) == chosen, display(terminal)
            motion(terminal, 35, chosen, row)
            output = len(terminal.raw)
            terminal.send('\x1b')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: divider(text) is None, 'close hovered sidebar')
            if modern:
                assert '\x1b]22;default\x1b\\' in terminal.raw[output:]

            terminal.child.send('/files pane.c\r')
            terminal.pump(.04)
            terminal.child.send('\x1b')
            terminal.pump(.06)
            terminal.send('\x1b')
            wait_for(terminal, lambda text: divider(text) is None, 'close during opening')
            terminal.pump(.3)
            assert divider(display(terminal)) is None, display(terminal)
            terminal.send('/files pane.c\r')
            wait_for(terminal, lambda text: 'SPLIT_CONTENT' in text, 'reopen after cancellation')
            terminal.pump(.2)
            assert divider(display(terminal)) == chosen, display(terminal)
            assert '↔' not in display(terminal), display(terminal)
            print(f'PASS native file split: modern={modern}, plain={plain}', flush=True)
        finally:
            terminal.child.close(force=True)


def main():
    for key in ('TMUX', 'STY'):
        os.environ.pop(key, None)
    binary = Path(sys.argv[1]).resolve()
    for modern, plain in ((True, False), (False, False), (True, True)):
        run_case(binary, modern, plain)


if __name__ == '__main__':
    main()
