"""Composer replacement grows below its top anchor and restores wrapped input."""
from pathlib import Path
import sys
import tempfile

from test_cli_pty import Model, Terminal
from test_cli_slash_pty import wait_for
from cli_inline_test_support import no_screen_switch, stable_rows
import pyte


def main():
    driver = Path(sys.argv[1]).resolve()
    for columns in (120, 80, 60):
        with tempfile.TemporaryDirectory(prefix='morph-panel-anchor-') as temporary:
            terminal = Terminal(driver, Path(temporary), 1, hooks_enabled=False)
            try:
                terminal.wait_for_composer()
                terminal.child.setwinsize(30, columns)
                terminal.screen = pyte.Screen(columns, 30)
                terminal.stream = pyte.Stream(terminal.screen)
                terminal.pump(.2)
                start = len(terminal.raw)
                # Trailing spaces wrap the submitted command without adding arguments.
                terminal.send('/model' + ' ' * columns)
                terminal.send('\r')
                wait_for(terminal, lambda: 'Select Model Mode' in
                         '\n'.join(terminal.screen.display))
                previous_height = 30
                previous_anchor = None
                for height in (30, 12, 24, 8, 30):
                    terminal.child.setwinsize(height, columns)
                    terminal.screen.resize(lines=height, columns=columns)
                    terminal.pump(.2)
                    rows = stable_rows(terminal)
                    diagnostic = '\n'.join(rows)
                    anchors = [i for i, row in enumerate(rows) if 'Select Model Mode' in row]
                    assert len(anchors) == 1, diagnostic
                    anchor = anchors[0]
                    assert terminal.screen.cursor.y == anchor, diagnostic
                    assert terminal.screen.cursor.x == 0, diagnostic
                    if height > previous_height:
                        assert anchor >= previous_anchor, diagnostic
                    assert all(i >= anchor for i, row in enumerate(rows)
                               if row.startswith('> ')), diagnostic
                    previous_anchor, previous_height = anchor, height
                no_screen_switch(terminal.raw[start:])
                terminal.send('\x1b')
                wait_for(terminal, lambda: 'Select Model Mode' not in
                         '\n'.join(terminal.screen.display))
                rows = stable_rows(terminal)
                diagnostic = '\n'.join(rows)
                prompts = [i for i, row in enumerate(rows) if row.startswith('› ')]
                assert len(prompts) == 1, diagnostic
                colored = [i for i in range(terminal.screen.lines)
                           if terminal.screen.buffer[i][0].bg == '3a3a3a']
                assert colored == [prompts[0] - 1, prompts[0], prompts[0] + 1], diagnostic
                assert not any(terminal.screen.buffer[y][x].bg in ('blue', '0000ee')
                               for y in range(terminal.screen.lines)
                               for x in range(columns)), diagnostic
                terminal.send('\x1b[A')
                assert '/model' in '\n'.join(terminal.screen.display)
                assert Model.requests.empty(), 'local panel requested a model turn'
                print(f'checked top anchor, height growth and wrapped recall at {columns}',
                      flush=True)
            finally:
                terminal.child.close(force=True)


if __name__ == '__main__':
    main()
