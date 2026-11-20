"""Conversation-file sidebar, local previews, focus and active-turn PTY regression."""
from pathlib import Path
import sys
import tempfile
import threading
import time

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Model, Terminal


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


def resize(terminal, columns):
    terminal.child.setwinsize(30, columns)
    terminal.screen.resize(30, columns)
    terminal.pump(.3)


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
            'SOURCE_PREVIEW_MARKER' if line == 80 else f'source row {line}'
            for line in range(1, 121)) + '\n')
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
            assert 'Files (3)' in opened and 'unrelated.txt' not in opened, opened
            assert 'Inspect Notes' in opened, 'main conversation must remain visible'
            terminal.send('m')
            wait_for(terminal, lambda text: '# SIDEBAR_HEADING' in text, 'Markdown source')
            terminal.send('m')
            terminal.send('\x06NEEDLE')
            searched = wait_for(terminal, lambda text: 'NEEDLE' in text, 'native document search')
            assert 'SIDEBAR_HEADING' in searched, searched
            terminal.send('\x1b')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (3)' not in text, 'close sidebar')

            for columns in (80, 60, 120):
                resize(terminal, columns)
                terminal.send('/sidebar "src/demo.c:80"\r')
                frame = wait_for(terminal, lambda text: 'SOURCE_PREVIEW_MARKER' in text,
                                 f'preview at {columns} columns')
                assert 'Files (3)' in frame, frame
                if columns >= 100:
                    assert 'Inspect Notes' in frame, frame
                terminal.send('\x1b')
                wait_for(terminal, lambda text: 'Files (3)' not in text,
                         f'close at {columns} columns')

            terminal.send('Continue working while I inspect files\r')
            _, release = terminal.request()
            terminal.send('/files\r\r')
            wait_for(terminal, lambda text: 'SIDEBAR_HEADING' in text, 'preview during turn')
            terminal.send('\x0fUNSENT_SIDEBAR_DRAFT')
            before = wait_for(terminal, lambda text: 'UNSENT_SIDEBAR_DRAFT' in text,
                              'draft while sidebar is open')
            release.delta = {'content': 'Finished. Also inspect [Extra](extra.txt).'}
            release.set()
            after = wait_for(terminal, lambda text: 'Files (4)' in text,
                             'live inventory update')
            assert 'UNSENT_SIDEBAR_DRAFT' in after, after
            assert 'SIDEBAR_HEADING' in after, after
            assert 'wait for the active turn' not in after, after
            terminal.send('\x15')
            terminal.send('\x0f\x1b')
            wait_for(terminal, lambda text: 'Files (4)' not in text, 'return to main')
            terminal.send('/new empty-files\r/files\r')
            wait_for(terminal, lambda text: 'Files (0)' in text, 'session inventory isolation')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Files (0)' not in text, 'close empty sidebar')
            terminal.send('/switch files-test\r')
            wait_for(terminal, lambda text: 'switched to session: files-test' in text,
                     'switch back to saved session')
            terminal.send('/files\r')
            wait_for(terminal, lambda text: 'Files (4)' in text, 'restored inventory')
            assert terminal.child.isalive(), terminal.raw[-4000:]
            print('PASS: sidebar file links, Markdown/source, search, responsive layout, '
                  'active-turn updates, drafts and session restore')
        finally:
            if release:
                release.set()
            terminal.child.close(force=True)
    server.shutdown()


if __name__ == '__main__':
    main()
