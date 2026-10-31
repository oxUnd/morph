"""Validate the component, page, and interaction extension demo."""
import sys

from test_tui_samples_pty import customization

if __name__ == '__main__':
    customization(sys.argv[1])
    print('custom widget, page composition, bound actions, shortcuts, model sync, and cleanup passed')
