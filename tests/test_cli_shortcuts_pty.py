"""Morph's raw key handlers appear in the native Ctrl+K shortcut popup."""
from pathlib import Path
import sys
import tempfile

from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen
from test_cli_setup_pty import Terminal as SetupTerminal
import pyte


def display(terminal):
    return '\n'.join(terminal.screen.display)


def shortcut(terminal, query, key, description):
    terminal.send('\x0b' + query)
    text = display(terminal)
    assert 'Keyboard shortcuts' in text, text
    assert key in text and description in text, text
    assert 'other focus' not in text, text
    terminal.send('\x1b')
    assert 'Keyboard shortcuts' not in display(terminal), display(terminal)


def check_chat(driver):
    for width in (120, 80, 60):
        with tempfile.TemporaryDirectory(prefix='morph-shortcuts-') as temp:
            terminal = Terminal(driver, Path(temp), 1, hooks_enabled=False,
                                extra_env={'NO_COLOR': '1'} if width == 60 else None)
            terminal.child.setwinsize(30, width)
            terminal.screen = AlternateScreen(width, 30)
            terminal.stream = pyte.Stream(terminal.screen)
            try:
                terminal.wait_for_composer()
                terminal.send('shortcut draft中文🙂 tail\x1b[D')
                cursor = (terminal.screen.cursor.x, terminal.screen.cursor.y)
                shortcut(terminal, 'Ctrl+T', 'Ctrl+T', 'Toggle turn details')
                shortcut(terminal, 'Paste image', 'Ctrl+Cmd+V', 'Paste image')
                shortcut(terminal, 'Previous session', 'Ctrl+PageUp', 'Previous session')
                assert (terminal.screen.cursor.x, terminal.screen.cursor.y) == cursor
                terminal.send('X')
                assert 'taiXl' in display(terminal), display(terminal)
                terminal.send('\x14')
                assert 'Turn details' in display(terminal), display(terminal)
                shortcut(terminal, 'Toggle raw', 'R', 'Toggle raw JSON')
                terminal.send('r')
                assert 'raw JSON' in terminal.screen.display[3], display(terminal)
                terminal.send('\x14')
                assert 'taiXl' in display(terminal), display(terminal)
            finally:
                terminal.child.close(force=True)


def check_pages(driver):
    with tempfile.TemporaryDirectory(prefix='morph-shortcut-pages-') as temp:
        directory = Path(temp)
        (directory / 'note.md').write_text('# Shortcut preview\n', encoding='utf-8')
        terminal = Terminal(driver, directory, 1, memory_enabled=True, hooks_enabled=True)
        terminal.child.setwinsize(30, 120)
        terminal.screen = AlternateScreen(120, 30)
        terminal.stream = pyte.Stream(terminal.screen)
        try:
            terminal.wait_for_composer()
            for command, query, key, description in (
                    ('/resume', 'Next page of sessions', 'N', 'Next page of sessions'),
                    ('/history', 'Show last model', 'S', 'Show last model request'),
                    ('/memory', 'Refresh memories', 'R', 'Refresh memories'),
                    ('/jobs', 'Refresh background', 'R', 'Refresh background jobs'),
                    ('/hooks', 'Browse hook', 'E', 'Browse hook events'),
                    ('/agents', 'Toggle agent browser', '?', 'Toggle agent browser help')):
                terminal.send(command + '\r')
                shortcut(terminal, query, key, description)
                terminal.send('\x1b')
                terminal.wait_for_composer()
            terminal.send('/side note.md\r')
            shortcut(terminal, 'Toggle Markdown', 'M', 'Toggle Markdown source')
            shortcut(terminal, 'Close file tab', 'Ctrl+W', 'Close file tab')
            shortcut(terminal, 'Switch focus', 'Ctrl+O', 'Switch focus between chat and files')
            terminal.send('\x0f')
            shortcut(terminal, 'Toggle turn', 'Ctrl+T', 'Toggle turn details')
            terminal.send('file draft\x1b[D')
            shortcut(terminal, 'Previous session', 'Ctrl+PageUp', 'Previous session')
            terminal.send('X')
            assert 'file drafXt' in display(terminal), display(terminal)
        finally:
            terminal.child.close(force=True)


def check_setup(driver):
    with tempfile.TemporaryDirectory(prefix='morph-shortcut-setup-') as temp:
        terminal = SetupTerminal(driver, Path(temp) / 'config.toml', ready=True,
                                 size=(30, 120))
        try:
            terminal.send('\x0bSave configuration')
            text = terminal.display()
            assert 'Keyboard shortcuts' in text and 'Setup' in text, text
            assert 'F10' in text and 'Ctrl+Enter' in text, text
            terminal.send('\x1b')
            assert 'Set up morph' in terminal.display(), terminal.display()
        finally:
            terminal.child.close(force=True)


if __name__ == '__main__':
    driver = Path(sys.argv[1]).resolve()
    check_chat(driver)
    check_pages(driver)
    check_setup(Path(sys.argv[2]).resolve())
