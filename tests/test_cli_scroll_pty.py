"""Scroll long retained content without repainting unchanged rows or moving drafts."""
from pathlib import Path
import sys
import tempfile

from test_cli_files_pty import display, resize, wait_for
from test_cli_pty import Terminal


def main():
    binary = Path(sys.argv[1]).resolve()
    with tempfile.TemporaryDirectory(prefix='morph-scroll-pty-') as temp:
        directory = Path(temp)
        document = directory / 'scroll.md'
        document.write_text('```text\n' + '\n'.join(
            f'SCROLL_ROW_{row:04d} repeated retained conversation content for scrolling.'
            for row in range(400)) + '\n```\n')
        terminal = Terminal(binary, directory, 1, production=True, hooks_enabled=False,
                            extra_env={'NO_COLOR': ''})
        try:
            resize(terminal, 120)
            terminal.wait_for_composer()
            terminal.send('/render ' + str(document) + '\r')
            wait_for(terminal, lambda text: 'SCROLL_ROW_0399' in text, 'long document')
            terminal.send('SCROLL_PROTECTED_DRAFT')
            row = next(index for index, text in enumerate(terminal.screen.display)
                       if 'SCROLL_PROTECTED_DRAFT' in text)
            terminal.pump(.2)
            start = len(terminal.raw.encode('utf-8'))
            buttons = [64] * 12 + [65] * 12
            for index, button in enumerate(buttons):
                terminal.child.send(f'\x1b[<{button};4;4M')
                terminal.pump(.08)
                assert 'SCROLL_PROTECTED_DRAFT' in terminal.screen.display[row], display(terminal)
                if index == 11:
                    assert 'SCROLL_ROW_0399' not in display(terminal), display(terminal)
            terminal.pump(.2)
            output = len(terminal.raw.encode('utf-8')) - start
            viewport = terminal.screen.columns * terminal.screen.lines
            # Only the row numbers change in this fixture; whole-row redraws
            # unnecessarily resend the repeated text and its empty background.
            assert output < viewport * len(buttons) // 4, (output, display(terminal))
            assert 'SCROLL_ROW_0399' in display(terminal), display(terminal)
            assert not terminal.duplicate_prompts, terminal.duplicate_prompts
            print(f'PASS: {len(buttons)} wheel events, {output} output bytes, draft pinned')
        finally:
            terminal.child.close(force=True)


if __name__ == '__main__':
    main()
