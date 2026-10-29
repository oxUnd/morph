"""Snapshots must observe complete synchronized terminal updates."""
import unittest

from test_cli_pty import Terminal, pyte


class TerminalSnapshotTest(unittest.TestCase):
    def test_waits_for_the_end_of_a_split_synchronized_frame(self):
        terminal = Terminal.__new__(Terminal)
        terminal.screen = pyte.Screen(80, 24)
        terminal.stream = pyte.Stream(terminal.screen)
        terminal.snapshots = []
        terminal.stream.feed('Inspecting previous frame')
        fragments = iter(['\x1b[?2026h\r\x1b[2K',
                          'Inspecting complete frame\x1b[?2026l'])
        terminal.pump = lambda duration=.3: terminal.stream.feed(next(fragments))
        snapshot = terminal.snapshot('complete synchronized frame')
        self.assertIn('Inspecting complete frame', snapshot)
        self.assertNotIn('previous frame', snapshot)


if __name__ == '__main__':
    unittest.main()
