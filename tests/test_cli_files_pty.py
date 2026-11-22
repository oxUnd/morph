"""Conversation-file sidebar, local previews, focus and active-turn PTY regression."""
from pathlib import Path
import sys
import tempfile
import threading
import time

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal
from test_cli_native_media_pty import png


def display(terminal):
    return '\n'.join(terminal.screen.display)


def wait_for(terminal, predicate, label, timeout=8):
    deadline = time.monotonic() + timeout
    while not predicate(display(terminal)):
        assert time.monotonic() < deadline, label + '\n' + display(terminal)
        terminal.pump(.05)
    return display(terminal)


def click_text(terminal, text):
    for row, line in enumerate(terminal.screen.display):
        if text in line:
            column = line.index(text) + 1
            terminal.send(f'\x1b[<0;{column};{row + 1}M\x1b[<0;{column};{row + 1}m')
            return
    raise AssertionError('missing click target: ' + text + '\n' + display(terminal))


def panel_lines(terminal):
    lines = terminal.screen.display
    if terminal.screen.columns >= 100:
        return [line.split('│', 1)[-1] for line in lines]
    return lines


def check_rules(terminal):
    lines = panel_lines(terminal)
    count = sum(bool(line.strip()) and set(line.strip()) == {'─'} for line in lines)
    assert count == 2, display(terminal)


def click_close(terminal, label):
    for row, line in enumerate(terminal.screen.display):
        if label in line:
            column = line.index(label) + label.index('×') + 1
            terminal.send(f'\x1b[<0;{column};{row + 1}M\x1b[<0;{column};{row + 1}m')
            return
    raise AssertionError('missing tab: ' + label + '\n' + display(terminal))


def resize(terminal, columns):
    terminal.child.setwinsize(30, columns)
    terminal.screen.resize(30, columns)
    terminal.pump(.3)


def plain_case(driver, directory, port):
    directory.mkdir()
    (directory / '文件.txt').write_text('PLAIN_FILE_CONTENT\n'
                                      '\tPLAIN_TAB\n\t\tPLAIN_NESTED\n'
                                      '  \tPLAIN_MIXED\n', encoding='utf-8')
    png(directory / 'preview.png', 8, 8, (60, 90, 120, 255))
    terminal = Terminal(driver, directory, port, hooks_enabled=False, production=True,
                        extra_env={'NO_COLOR': '1'}, extra_arguments=('--no-color',))
    try:
        terminal.wait_for_composer()
        terminal.send('/side 文件.txt\r')
        wait_for(terminal, lambda text: 'PLAIN_FILE_CONTENT' in text, 'plain viewer')
        for columns in (120, 80, 60):
            resize(terminal, columns)
            lines = panel_lines(terminal)
            origin = next(line.index('PLAIN_FILE_CONTENT') for line in lines
                          if 'PLAIN_FILE_CONTENT' in line)
            for text, indent in (('PLAIN_TAB', 8), ('PLAIN_NESTED', 16), ('PLAIN_MIXED', 8)):
                line = next(line for line in lines if text in line)
                assert line.index(text) == origin + indent, display(terminal)

        check_rules(terminal)
        terminal.send('\x1b')
        listed = wait_for(terminal, lambda text: '- 文件.txt (./)' in text, 'plain Unicode list')
        row = next(i for i, text in enumerate(terminal.screen.display) if '- 文件.txt' in text)
        assert all(cell.fg == 'default' and cell.bg == 'default'
                   for cell in terminal.screen.buffer[row].values()), listed
        terminal.send('\x1b')
        terminal.send('/side preview.png\r')
        image = wait_for(terminal, lambda text: '· Image' in text, 'image viewer')
        assert '2 open' in image and 'M source' not in image, image
        check_rules(terminal)
        click_text(terminal, '文件.txt ×')
        wait_for(terminal, lambda text: 'PLAIN_FILE_CONTENT' in text, 'return from image tab')
    finally:
        terminal.child.close(force=True)


def image_case(binary, directory):
    directory.mkdir()
    png(directory / 'preview.png', 64, 32, (200, 70, 40, 255))
    terminal = Terminal(binary, directory, 1, production=True, hooks_enabled=False,
                        extra_env={'NO_COLOR': ''})
    try:
        terminal.wait_for_composer()
        terminal.send('/side preview.png\r')
        wait_for(terminal, lambda text: '· Image' in text, 'fallback image metadata')
        for columns in (120, 80, 60):
            resize(terminal, columns)
            frame = wait_for(terminal, lambda text: '▀' in text,
                             f'half-block image preview at {columns} columns')
            pixels = [cell for line in terminal.screen.buffer.values()
                      for cell in line.values() if cell.data == '▀']
            assert pixels and all(cell.fg == 'c84628' for cell in pixels), pixels
            assert '(image:' not in frame, frame
            assert not any(packet in terminal.raw for packet in
                           ('\x1b_G', '\x1b]1337;File=', '\x1bPq')), terminal.raw
            check_rules(terminal)
        print('PASS: image previews without a native terminal graphics protocol')
    finally:
        terminal.child.close(force=True)


def main():
    driver = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), Model)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    with tempfile.TemporaryDirectory(prefix='morph-files-pty-') as temp:
        directory = Path(temp)
        notes = directory / 'notes.md'
        notes.write_text('# SIDEBAR_HEADING\n\nNATIVE_SEARCH_NEEDLE\n\n'
                         '[Nested source](src/demo.c:80)\n', encoding='utf-8')
        (directory / 'src').mkdir()
        (directory / 'src/demo.c').write_text('\n'.join(
            '' if line <= 2 else
            '/* 中文 comment' if line == 3 else
            ' * continued */' if line == 4 else
            '\treturn 80; /* SOURCE_PREVIEW_MARKER */' if line == 80 else
            '\t\tint nested = 81;' if line == 81 else
            '  \tint mixed = 82;' if line == 82 else
            'int\tfield;' if line == 83 else
            f'const int source_row_{line} = {line};'
            for line in range(1, 121)) + '\n', encoding='utf-8')
        (directory / 'unrelated.txt').write_text('not part of this conversation')
        (directory / 'missing.txt').write_text('removed after the answer')
        (directory / 'extra.txt').write_text('extra content')
        terminal = Terminal(driver, directory, server.server_port, hooks_enabled=False)
        release = None
        try:
            resize(terminal, 120)
            terminal.wait_for_composer()
            terminal.send('/new files-test\r')
            wait_for(terminal, lambda text: 'created and switched' in text, 'new session')
            terminal.send('Show the relevant files\r')
            _, release = terminal.request()
            release.delta = {'content': 'Inspect [Notes](notes.md), [Source](src/demo.c:80), '
                                       'and [Missing](missing.txt).'}
            release.set()
            wait_for(terminal, lambda text: 'Worked for' in text and 'Inspect Notes' in text,
                     'initial answer')
            (directory / 'missing.txt').unlink()
            click_text(terminal, 'Notes')
            opened = wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text,
                              'file link opens sidebar')
            assert '< Files' in opened and 'unrelated.txt' not in opened, opened
            assert 'notes.md ×' not in opened, 'one file must not show a tab bar'
            check_rules(terminal)
            assert 'Inspect Notes' in opened, 'main conversation must remain visible'
            terminal.send('m')
            wait_for(terminal, lambda text: '# SIDEBAR_HEADING' in text, 'Markdown source')
            terminal.send('m')
            terminal.send('\x06NEEDLE')
            searched = wait_for(terminal, lambda text: 'Find:' in text, 'native document search')
            assert 'SIDEBAR_HEADING' in searched, searched
            terminal.send('\x1b')
            terminal.send('\x1b')
            listed = wait_for(terminal, lambda text: 'Files (3)' in text, 'back to list')
            assert '- notes.md (./)' in listed and '- demo.c (src/)' in listed, listed
            assert 'Open: 1' in listed and 'SIDEBAR_HEADING' not in listed, listed
            row = next(i for i, line in enumerate(terminal.screen.display)
                       if '- demo.c (src/)' in line)
            column = terminal.screen.display[row].index('demo.c')
            assert terminal.screen.buffer[row][column].fg != terminal.screen.buffer[row][column + 7].fg
            check_rules(terminal)
            click_text(terminal, 'Open: 1')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text, 'return to retained viewer')
            click_text(terminal, 'Nested source')
            wait_for(terminal, lambda text: 'return 80;' in text, 'local link inside viewer')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (3)' in text, 'back after nested link')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (3)' not in text and '< Files' not in text,
                     'close sidebar')

            for columns in (80, 60, 120):
                resize(terminal, columns)
                terminal.send('/side "src/demo.c:80"\r')
                frame = wait_for(terminal, lambda text: 'return 80;' in text,
                                 f'preview at {columns} columns')
                assert '2 open' in frame and 'demo.c ×' in frame, frame
                assert 'M source' not in frame, frame
                check_rules(terminal)
                row = next(i for i, text in enumerate(terminal.screen.display)
                           if 'return 80;' in text)
                column = terminal.screen.display[row].index('return 80;')
                pane = panel_lines(terminal)
                origin = next(line.index('const int source_row_') for line in pane
                              if 'const int source_row_' in line)
                for text, indent in (('return 80;', 8), ('int nested', 16),
                                     ('int mixed', 8), ('field;', 8)):
                    line = next(line for line in pane if text in line)
                    assert line.index(text) == origin + indent, frame

                keyword = terminal.screen.buffer[row][column]
                number = terminal.screen.buffer[row][column + len('return ')]
                assert keyword.bold and keyword.fg != 'default', frame
                assert number.fg != keyword.fg, frame
                terminal.send('\x06mixed')
                searched = wait_for(terminal, lambda text: 'Find: mixed' in text,
                                    'search after tab indentation')
                match_row = next(i for i, text in enumerate(terminal.screen.display)
                                 if 'int mixed = 82;' in text)
                match_column = terminal.screen.display[match_row].index('mixed')
                assert terminal.screen.buffer[match_row][match_column].reverse, searched
                terminal.send('\x1b')
                if columns >= 100:
                    assert 'Inspect Notes' in frame, frame
                terminal.send('\x1b')
                wait_for(terminal, lambda text: 'Files (3)' in text, 'return to file list')
                terminal.send('\x1b')
                wait_for(terminal, lambda text: 'Files (3)' not in text and '< Files' not in text,
                         f'close at {columns} columns')

            terminal.send('/side\r')
            wait_for(terminal, lambda text: '- notes.md (./)' in text, 'list first')
            click_text(terminal, '- notes.md')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text, 'reopen existing tab')
            assert '2 open' in display(terminal), 'opening an existing file must reuse its tab'
            click_text(terminal, 'demo.c ×')
            wait_for(terminal, lambda text: 'return 80;' in text, 'switch native tab')
            terminal.send('\x1b[6~')
            terminal.pump(.2)
            scrolled = [line for line in panel_lines(terminal) if 'source_row_' in line]
            assert scrolled, display(terminal)
            click_text(terminal, 'notes.md ×')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text, 'switch back to Markdown')
            terminal.send('\x06SIDEBAR')
            wait_for(terminal, lambda text: 'Find: SIDEBAR' in text, 'search in active tab')
            click_text(terminal, 'demo.c ×')
            terminal.pump(.2)
            assert [line for line in panel_lines(terminal) if 'source_row_' in line] == scrolled
            click_text(terminal, 'notes.md ×')
            terminal.pump(.2)
            assert any('SIDEBAR' in line and ('Find' in line or 'find' in line)
                       for line in panel_lines(terminal)), display(terminal)
            terminal.send('\x1b')
            click_text(terminal, 'demo.c ×')
            terminal.send('\x17')
            single = wait_for(terminal, lambda text: '2 open' not in text and 'SIDEBAR_HEADING' in text,
                              'close tab with Ctrl+W')
            assert 'notes.md ×' not in single, single
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (3)' in text, 'list after tab close')
            click_text(terminal, '- missing.txt')
            wait_for(terminal, lambda text: 'The file no longer exists.' in text, 'missing preview')
            (directory / 'missing.txt').write_text('RELOADED_FILE')
            terminal.send('r')
            wait_for(terminal, lambda text: 'RELOADED_FILE' in text, 'retry missing file')
            click_close(terminal, 'missing.txt ×')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text and '2 open' not in text,
                     'close native tab with mouse')
            terminal.send('\x17')
            wait_for(terminal, lambda text: 'Files (3)' in text and 'Open:' not in text,
                     'last tab returns to list')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (3)' not in text, 'close file list')

            terminal.send('Continue working while I inspect files\r')
            _, release = terminal.request()
            terminal.send('/side\r')
            wait_for(terminal, lambda text: 'Files (3)' in text, 'list during turn')
            click_text(terminal, '- notes.md')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text, 'preview during turn')
            terminal.send('\x0fUNSENT_SIDEBAR_DRAFT')
            before = wait_for(terminal, lambda text: 'UNSENT_SIDEBAR_DRAFT' in text,
                              'draft while sidebar is open')
            release.delta = {'content': 'Finished. Also inspect [Extra](extra.txt).'}
            release.set()
            after = wait_for(terminal, lambda text: 'Finished.' in text,
                             'live inventory update')
            assert 'UNSENT_SIDEBAR_DRAFT' in after, after
            assert 'SIDEBAR_HEADING' in after, after
            assert 'wait for the active turn' not in after, after
            terminal.send('\x15')
            terminal.send('\x0f\x1b')
            listed = wait_for(terminal, lambda text: 'Files (4)' in text, 'live inventory update')
            assert '- extra.txt (./)' in listed, listed
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (4)' not in text, 'return to main')
            terminal.send('/new empty-files\r/side\r')
            wait_for(terminal, lambda text: 'Files (0)' in text, 'session inventory isolation')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (0)' not in text, 'close empty sidebar')
            terminal.send('/switch files-test\r')
            wait_for(terminal, lambda text: 'switched to session: files-test' in text,
                     'switch back to saved session')
            terminal.send('/side\r')
            wait_for(terminal, lambda text: 'Files (4)' in text, 'restored inventory')
            assert 'Open:' not in display(terminal), 'session changes must close old tabs'
            terminal.send('\x1b')

            for index in range(4):
                name = f'tab_{index}_long_file_name.txt'
                (directory / name).write_text(f'TAB_CONTENT_{index}')
                terminal.send('/side ' + name + '\r')
                frame = wait_for(terminal, lambda text: f'TAB_CONTENT_{index}' in text,
                                 'overflowing native tabs')
                if index:
                    assert name + ' ×' in frame, 'active tab must remain visible\n' + frame
                else:
                    assert name + ' ×' not in frame, 'a single file hides the tab bar'
                check_rules(terminal)
                if index < 3:
                    terminal.send('\x1b\x1b')
                    wait_for(terminal, lambda text: '< Files' not in text and 'Files (' not in text,
                             'close sidebar while retaining tabs')

            terminal.send('\x1b[1;3D')
            wait_for(terminal, lambda text: 'TAB_CONTENT_2' in text, 'previous tab shortcut')
            terminal.send('\x1b[1;3C')
            wait_for(terminal, lambda text: 'TAB_CONTENT_3' in text, 'next tab shortcut')
            click_close(terminal, 'tab_3_long_file_name.txt ×')
            wait_for(terminal, lambda text: 'TAB_CONTENT_2' in text, 'close scrolled tab')
            for columns in (80, 60, 120):
                resize(terminal, columns)
                frame = wait_for(terminal, lambda text: 'tab_2_long_file_name.txt ×' in text,
                                 'active tab visible after resize')
                assert 'TAB_CONTENT_2' in frame, frame
                check_rules(terminal)

            assert terminal.child.isalive(), terminal.raw[-4000:]
            print('PASS: sidebar file links, Markdown/source, syntax colors and source locations, '
                  'native tabs, retained scroll/search, closing, retry, two rules, responsive layout, '
                  'active-turn updates, drafts and session restore')
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)
        binary = Path(sys.argv[2]).resolve() if len(sys.argv) > 2 else driver.with_name('morph')
        plain_case(binary, directory / 'plain', server.server_port)
        image_case(binary, directory / 'images')
        print('PASS: production no-color mode, Unicode filenames and image tabs')
    server.shutdown()


if __name__ == '__main__':
    main()
