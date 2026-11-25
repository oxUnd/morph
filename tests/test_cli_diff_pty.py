"""Session and Git diffs through the production sidebar event loop."""
from pathlib import Path
import json
import subprocess
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal
from test_cli_transcript_pty import PacedMarkdownModel, AlternateScreen
from test_cli_files_pty import display, panel_lines, resize, wait_for, check_rules
from test_cli_shortcuts_pty import shortcut
import pyte


def click_side(terminal, label):
    for row, line in enumerate(terminal.screen.display):
        start = line.index('│') + 1 if '│' in line else 0
        column = line.find(label, start)
        if column >= 0:
            terminal.send(f'\x1b[<0;{column + 1};{row + 1}M'
                          f'\x1b[<0;{column + 1};{row + 1}m')
            return
    raise AssertionError(f'missing sidebar target {label}\n{display(terminal)}')


def session_case(binary, port):
    with tempfile.TemporaryDirectory(prefix='morph-session-diff-') as temp:
        directory = Path(temp)
        (directory / 'example.txt').write_text('OLD_SESSION_LINE\n')
        terminal = Terminal(binary, directory, port, production=True, hooks_enabled=False,
                            extra_config='[permissions]\naccess="full"\napproval="never"\n'
                                         'reviewer="user"\n')
        terminal.screen = AlternateScreen(80, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        chunks = None
        try:
            resize(terminal, 120)
            terminal.wait_for_composer()
            terminal.send('/new session-diff\r')
            wait_for(terminal, lambda text: 'created and switched' in text, 'new session')
            terminal.send('Update the example\r')
            _, chunks = terminal.request()
            patch = ('*** Begin Patch\n*** Update File: example.txt\n@@\n'
                     '-OLD_SESSION_LINE\n+NEW_SESSION_LINE\n*** End Patch')
            chunks.put({'tool_calls': [{'index': 0, 'id': 'session-patch', 'type': 'function',
                                       'function': {'name': 'apply_patch',
                                                    'arguments': json.dumps({'input': patch})}}]})
            chunks.put(None)
            _, chunks = terminal.request()
            assert (directory / 'example.txt').read_text() == 'NEW_SESSION_LINE\n'
            chunks.put('Updated the example.')
            chunks.put(None)
            wait_for(terminal, lambda text: 'Worked for' in text, 'completed turn')
            chunks = None
            terminal.send('/side\r')
            click_side(terminal, 'Diff')
            wait_for(terminal, lambda text: 'Session patches (1)' in text, 'default session source')
            click_side(terminal, 'example.txt')
            wait_for(terminal, lambda text: 'OLD_SESSION_LINE' in '\n'.join(panel_lines(terminal)),
                     'recorded non-Git diff')
            check_rules(terminal)
            terminal.send('\x06NEW_SESSION')
            wait_for(terminal, lambda text: 'Find: NEW_SESSION' in text, 'native diff search')
            terminal.send('\x1b')
            for width in (80, 60, 120):
                resize(terminal, width)
                frame = '\n'.join(panel_lines(terminal))
                assert 'OLD_SESSION_LINE' in frame and 'NEW_SESSION_LINE' in frame, frame
                check_rules(terminal)
            terminal.send('\x1bg')
            wait_for(terminal, lambda text: 'Git unavailable:' in text, 'explicit non-repository state')
            terminal.send('\x1bs')
            wait_for(terminal, lambda text: 'OLD_SESSION_LINE' in '\n'.join(panel_lines(terminal)),
                     'retained session preview')
            terminal.send('\x0fUNSENT_DIFF_DRAFT')
            wait_for(terminal, lambda text: 'UNSENT_DIFF_DRAFT' in text, 'draft while inspecting')
            terminal.send('\x0f\x1b\x1b')
            wait_for(terminal, lambda text: 'Session patches' not in text and 'Files (' not in text,
                     'close sidebar')
            terminal.send('\x15/new empty-diff\r/side\r')
            wait_for(terminal, lambda text: 'Files (0)' in text, 'empty session')
            click_side(terminal, 'Diff')
            wait_for(terminal, lambda text: 'No recorded patches' in text, 'session isolation')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'No recorded patches' not in text, 'close empty sidebar')
            terminal.send('/switch session-diff\r')
            wait_for(terminal, lambda text: 'switched to session: session-diff' in text, 'restore history')
            terminal.send('/side\r')
            click_side(terminal, 'Diff')
            wait_for(terminal, lambda text: 'Session patches (1)' in text, 'restored patch inventory')
            click_side(terminal, 'example.txt')
            wait_for(terminal, lambda text: 'OLD_SESSION_LINE' in '\n'.join(panel_lines(terminal)),
                     'restored actual patch')
        finally:
            if chunks is not None:
                chunks.put(None)
            terminal.child.close(force=True)


def git_case(binary, plain):
    with tempfile.TemporaryDirectory(prefix='morph-git-sidebar-') as temp:
        directory = Path(temp)
        def git(*arguments):
            subprocess.run(['git', '-c', 'commit.gpgsign=false', '-c',
                            'core.hooksPath=/dev/null', *arguments], cwd=directory,
                           check=True, capture_output=True)
        git('init', '-q')
        git('config', 'user.name', 'Sidebar Test')
        git('config', 'user.email', 'sidebar@example.invalid')
        (directory / '.gitignore').write_text('*\n!tracked.txt\n!new.txt\n')
        target = directory / 'tracked.txt'
        target.write_text('BASE_GIT_LINE\n')
        git('add', 'tracked.txt')
        git('commit', '-qm', 'base')
        target.write_text('INDEX_GIT_LINE\n')
        git('add', 'tracked.txt')
        target.write_text('WORKTREE_GIT_LINE\n')
        (directory / 'new.txt').write_text('UNTRACKED_GIT_LINE\n')
        terminal = Terminal(binary, directory, 1, production=True, hooks_enabled=False,
                            extra_arguments=('--no-color',) if plain else (),
                            extra_env={'NO_COLOR': '1' if plain else ''})
        try:
            resize(terminal, 120)
            terminal.wait_for_composer()
            terminal.send('/side\r')
            click_side(terminal, 'Diff')
            wait_for(terminal, lambda text: 'No recorded patches' in text, 'Session default in Git repo')
            terminal.send('\x1bg')
            wait_for(terminal, lambda text: 'Unstaged' in text and 'Staged' in text and 'Untracked' in text,
                     'Git groups')
            click_side(terminal, 'tracked.txt')
            wait_for(terminal, lambda text: 'WORKTREE_GIT_LINE' in '\n'.join(panel_lines(terminal)),
                     'unstaged diff')
            if not plain:
                added = next((row, line.index('WORKTREE_GIT_LINE'))
                             for row, line in enumerate(terminal.screen.display) if 'WORKTREE_GIT_LINE' in line)
                removed = next((row, line.index('INDEX_GIT_LINE'))
                               for row, line in enumerate(terminal.screen.display) if 'INDEX_GIT_LINE' in line)
                added_style = terminal.screen.buffer[added[0]][added[1]]
                removed_style = terminal.screen.buffer[removed[0]][removed[1]]
                assert added_style.fg != 'default' and added_style.fg != removed_style.fg
            for width in (120, 80, 60):
                resize(terminal, width)
                lines = panel_lines(terminal)
                assert any('INDEX_GIT_LINE' in line for line in lines), display(terminal)
                assert any('WORKTREE_GIT_LINE' in line for line in lines), display(terminal)
                check_rules(terminal)
            target.write_text('LATEST_GIT_LINE\n')
            terminal.send('r')
            wait_for(terminal, lambda text: 'LATEST_GIT_LINE' in '\n'.join(panel_lines(terminal)),
                     'refresh diff')
            terminal.send('\x1bs\x1bg')
            wait_for(terminal, lambda text: 'LATEST_GIT_LINE' in '\n'.join(panel_lines(terminal)),
                     'retained Git document')
            shortcut(terminal, 'Diff sidebar tab', 'Alt+2', 'Diff sidebar tab')
            shortcut(terminal, 'Session diffs', 'Alt+S', 'Session diffs')
            shortcut(terminal, 'Git diffs', 'Alt+G', 'Git diffs')
            shortcut(terminal, 'Next diff hunk', 'N', 'Next diff hunk')
            terminal.send('\x1b')
            wait_for(terminal, lambda text: 'Unstaged' in text, 'back to Git list')
            click_side(terminal, 'new.txt')
            wait_for(terminal, lambda text: 'UNTRACKED_GIT_LINE' in '\n'.join(panel_lines(terminal)),
                     'new file diff')
            if plain:
                row = next(i for i, line in enumerate(terminal.screen.display) if 'UNTRACKED_GIT_LINE' in line)
                assert all(cell.fg == 'default' and cell.bg == 'default'
                           for cell in terminal.screen.buffer[row].values()), display(terminal)
        finally:
            terminal.child.close(force=True)


def main():
    binary = Path(sys.argv[1]).resolve()
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        session_case(binary, server.server_port)
        git_case(binary, False)
        git_case(binary, True)
    finally:
        server.shutdown()
        server.server_close()
    print('PASS: default Session diffs, history/isolation, Git groups, refresh, resize and no-color')


if __name__ == '__main__':
    main()
