"""Skill trees must keep CJK and long descriptions inside the terminal width."""
from pathlib import Path
import sys
import tempfile

from test_cli_pty import Terminal, pexpect
import pyte


def main():
    driver = Path(sys.argv[1]).resolve()
    for width in (40, 80, 120):
        with tempfile.TemporaryDirectory(prefix='morph-tree-pty-') as temp:
            directory = Path(temp)
            for index in range(8):
                name = f'skill-{index}-with-an-extremely-long-component-name'
                skill = directory / 'home' / '.agents' / 'skills' / name
                skill.mkdir(parents=True)
                (skill / 'SKILL.md').write_text(
                    f'---\nname: {name}\ndescription: 中文描述 mixed text ' +
                    'with many additional words ' * 12 + '\n---\nInstructions.\n')
            terminal = Terminal(driver, directory, 1, hooks_enabled=False)
            try:
                terminal.pump(.5)
                terminal.child.setwinsize(32, width)
                terminal.screen = pyte.Screen(width, 32)
                terminal.stream = pyte.Stream(terminal.screen)
                terminal.pump(.4)
                terminal.send('/skill list\r')
                terminal.pump(.3)
                rows = terminal.screen.display
                display = '\n'.join(rows)
                assert 'Skills ' in display, display
                branches = [row for row in rows if row.startswith(('  ├ ', '  └ '))]
                skill_rows = [row for row in branches if 'skill-' in row]
                assert len(skill_rows) == 8, display
                assert sum(row.startswith('  └ ') for row in branches) == 1, display
                for index, row in enumerate(skill_rows):
                    assert f'skill-{index}' in row and '中文描述' in row, display
                    assert '…' in row, row
                assert sum(row.startswith('› ') for row in rows) == 1, display
                print(f'PASS: {width}-column skill tree stays on eight bounded rows')
            finally:
                terminal.child.close(force=True)


if __name__ == '__main__':
    main()
