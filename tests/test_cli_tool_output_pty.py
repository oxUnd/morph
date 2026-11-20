"""Click inline tool output links through the real terminal event loop."""
from pathlib import Path
import json
import sys
import tempfile
import threading

from loopback_http import LoopbackHTTPServer
from test_cli_pty import Terminal
from test_cli_transcript_pty import AlternateScreen, PacedMarkdownModel
import pyte


def check_inline_tool_output(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width, color in ((120, True), (80, True), (60, True), (80, False)):
            with tempfile.TemporaryDirectory(prefix='morph-inline-tool-') as temp:
                directory = Path(temp)
                for name in ('FIRST', 'SECOND'):
                    (directory / name).write_text(''.join(
                        f'{name}_OUTPUT_{index}\n' for index in range(7)))
                terminal = Terminal(driver if color else driver.parent / 'morph', directory,
                                    server.server_port, hooks_enabled=False,
                                    production=not color,
                                    extra_arguments=() if color else ('--no-color',),
                                    extra_env={'NO_COLOR': '' if color else '1'})
                terminal.child.setwinsize(36, width)
                terminal.screen = AlternateScreen(width, 36)
                terminal.stream = pyte.Stream(terminal.screen)
                chunks = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('read the two outputs\r')
                    _, chunks = terminal.request()
                    chunks.put({'tool_calls': [
                        {'index': index, 'id': f'inline-{name}', 'type': 'function',
                         'function': {'name': 'file_read', 'arguments': json.dumps({
                             'file_path': str(directory / name)})}}
                        for index, name in enumerate(('FIRST', 'SECOND'))]})
                    chunks.put(None)
                    _, chunks = terminal.request()
                    chunks.put('Both outputs are available.')
                    chunks.put(None)
                    terminal.pump(.4)
                    draft = 'draft中文🙂 ' + ('wrapped input ' * 4 if width == 60 else '') + 'tail'
                    terminal.send(draft + '\x1b[D\x1b[D')
                    collapsed = terminal.snapshot(f'{width}: inline output links')
                    assert collapsed.count('show more') == 2, collapsed
                    assert 'FIRST_OUTPUT_6' not in collapsed and 'SECOND_OUTPUT_6' not in collapsed
                    cursor = (terminal.screen.cursor.x, terminal.screen.cursor.y)
                    frame_start = len(terminal.published_frames)

                    def click(label, occurrence=0):
                        matches = [(row.index(label), index)
                                   for index, row in enumerate(terminal.screen.display)
                                   if label in row]
                        x, y = matches[occurrence]
                        for column in range(x, x + len(label)):
                            cell = terminal.screen.buffer[y][column]
                            assert cell.underscore == color, (label, cell)
                            assert (cell.fg != 'default') == color, (label, cell)
                        terminal.send(f'\x1b[<0;{x + 2};{y + 1}M'
                                      f'\x1b[<0;{x + 2};{y + 1}m')
                        assert terminal.screen.cursor.x == cursor[0], terminal.screen.display

                    click('show more')
                    expanded = terminal.snapshot(f'{width}: first output expands in place')
                    assert 'FIRST_OUTPUT_6' in expanded and 'SECOND_OUTPUT_6' not in expanded
                    assert expanded.count('show more') == expanded.count('show less') == 1
                    assert expanded.index('FIRST_OUTPUT_6') < expanded.index('show less')
                    assert 'draft中文🙂' in expanded and 'tail' in expanded, expanded
                    click('show more')
                    both = terminal.snapshot(f'{width}: outputs expand independently')
                    assert both.count('show less') == 2 and 'SECOND_OUTPUT_6' in both, both
                    click('show less')
                    one = terminal.snapshot(f'{width}: only the clicked output collapses')
                    assert 'FIRST_OUTPUT_6' not in one and 'SECOND_OUTPUT_6' in one, one
                    terminal.send('X')
                    typed = terminal.snapshot(f'{width}: click preserves draft focus and caret')
                    assert 'draft中文🙂' in typed and 'taXil' in typed, typed
                    cursor = (terminal.screen.cursor.x, terminal.screen.cursor.y)
                    terminal.send('\x14')
                    details = terminal.snapshot(f'{width}: Ctrl+T still opens transcript')
                    assert 'Turn details' in details and 'SECOND_OUTPUT_6' in details, details
                    terminal.send('\x1b')
                    restored = terminal.snapshot(f'{width}: inline state survives transcript')
                    assert 'FIRST_OUTPUT_6' not in restored and 'SECOND_OUTPUT_6' in restored
                    click('show less')
                    final = terminal.snapshot(f'{width}: both previews restored')
                    assert final.count('show more') == 2 and 'show less' not in final, final
                    assert 'draft中文🙂' in final and 'taXil' in final, final
                    for frame, _ in terminal.published_frames[frame_start:]:
                        if 'Turn details' not in frame:
                            assert frame.count('draft中文🙂') == 1, frame
                            assert 'tail' in frame or 'taXil' in frame, frame
                    assert not terminal.duplicate_prompts, terminal.duplicate_prompts
                finally:
                    if chunks is not None:
                        chunks.put(None)
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


def check_inline_patch_output(driver):
    server = LoopbackHTTPServer(('127.0.0.1', 0), PacedMarkdownModel)
    server.daemon_threads = True
    threading.Thread(target=server.serve_forever, daemon=True).start()
    try:
        for width, color in ((120, True), (80, True), (60, True), (80, False)):
            with tempfile.TemporaryDirectory(prefix='morph-inline-patch-') as temp:
                directory = Path(temp)
                target = directory / 'example.txt'
                padding = ''.join(f'padding {index}\n' for index in range(40))
                target.write_text(padding + 'context before\nOLD_PATCH_LINE\ncontext after\n')
                terminal = Terminal(driver if color else driver.parent / 'morph', directory,
                                    server.server_port, hooks_enabled=False,
                                    production=not color,
                                    extra_arguments=() if color else ('--no-color',),
                                    extra_env={'NO_COLOR': '' if color else '1'},
                                    extra_config='[permissions]\naccess = "full"\n'
                                                 'approval = "never"\nreviewer = "user"\n')
                terminal.child.setwinsize(36, width)
                terminal.screen = AlternateScreen(width, 36)
                terminal.stream = pyte.Stream(terminal.screen)
                chunks = None
                try:
                    terminal.wait_for_composer()
                    terminal.send('apply the example changes\r')
                    _, chunks = terminal.request()
                    patch = ('*** Begin Patch\n*** Update File: example.txt\n@@\n'
                             ' context before\n-OLD_PATCH_LINE\n+NEW_PATCH_ONE\n'
                             '+NEW_PATCH_TWO\n+NEW_PATCH_THREE\n context after\n'
                             '*** Add File: extra.txt\n+EXTRA_PATCH_FILE\n*** End Patch')
                    chunks.put({'tool_calls': [{'index': 0, 'id': 'inline-patch',
                                               'type': 'function', 'function': {
                                                   'name': 'apply_patch',
                                                   'arguments': json.dumps({'input': patch})}}]})
                    chunks.put(None)
                    body, chunks = terminal.request()
                    assert 'NEW_PATCH_ONE' in target.read_text(), body
                    assert (directory / 'extra.txt').read_text() == 'EXTRA_PATCH_FILE\n'
                    receipt = next(message for message in body['messages']
                                   if message.get('role') == 'tool' and
                                   message.get('tool_call_id') == 'inline-patch')
                    saved_diff = json.loads(receipt['content'])['ui']['patch_diff']
                    assert '@@ -41,3 +41,5 @@' in saved_diff, saved_diff
                    target.write_text('EXTERNAL_LATER_CHANGE\n')
                    terminal.send('patch draft中文🙂 tail\x1b[D')
                    compact = terminal.snapshot(f'{width}: patch shows actual code changes')
                    assert '-OLD_PATCH_LINE' in compact and '+NEW_PATCH_ONE' in compact, compact
                    assert f'{42:>5} {"":>5} -OLD_PATCH_LINE' in compact, compact
                    assert f'{"":>5} {42:>5} +NEW_PATCH_ONE' in compact, compact
                    assert 'show more' in compact and 'EXTRA_PATCH_FILE' not in compact, compact
                    for marker, colors in (('-OLD_PATCH_LINE', ('red', 'cd0000')),
                                           ('+NEW_PATCH_ONE', ('green', '00cd00'))):
                        row = next(y for y, text in enumerate(terminal.screen.display)
                                   if marker in text)
                        column = terminal.screen.display[row].index(marker)
                        cell = terminal.screen.buffer[row][column]
                        assert (cell.fg in colors) == color, (marker, cell)

                    def click(label):
                        y = next(y for y, row in enumerate(terminal.screen.display) if label in row)
                        x = terminal.screen.display[y].index(label)
                        terminal.send(f'\x1b[<0;{x + 2};{y + 1}M'
                                      f'\x1b[<0;{x + 2};{y + 1}m')

                    frame_start = len(terminal.published_frames)
                    click('show more')
                    expanded = terminal.snapshot(f'{width}: patch expands all files and context')
                    for text in ('context before', '-OLD_PATCH_LINE', '+NEW_PATCH_THREE',
                                 'extra.txt', '+EXTRA_PATCH_FILE', 'show less'):
                        assert text in expanded, expanded
                    assert 'EXTERNAL_LATER_CHANGE' not in expanded, expanded
                    for old, new, text in ((41, 41, ' context before'),
                                           (42, '', '-OLD_PATCH_LINE'),
                                           ('', 44, '+NEW_PATCH_THREE'),
                                           (43, 45, ' context after'),
                                           ('', 1, '+EXTRA_PATCH_FILE')):
                        assert f'{old:>5} {new:>5} {text}' in expanded, expanded
                    click('show less')
                    terminal.send('X')
                    collapsed = terminal.snapshot(f'{width}: patch collapses and preserves draft')
                    assert 'show more' in collapsed and 'show less' not in collapsed, collapsed
                    assert '+NEW_PATCH_THREE' not in collapsed, collapsed
                    assert 'patch draft中文🙂 taiXl' in collapsed, collapsed
                    for frame, _ in terminal.published_frames[frame_start:]:
                        assert frame.count('patch draft中文🙂') == 1, frame
                    chunks.put('Applied the patch.')
                    chunks.put(None)
                finally:
                    if chunks is not None:
                        chunks.put(None)
                    terminal.child.close(force=True)
    finally:
        server.shutdown()


if __name__ == '__main__':
    driver = Path(sys.argv[1]).resolve()
    check_inline_tool_output(driver)
    check_inline_patch_output(driver)
