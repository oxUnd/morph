"""Read native status constants from the pinned public C contract."""
from pathlib import Path
import re


def native_status(name):
    header = Path(__file__).resolve().parents[1] / 'vendor/nothing/include/nb/err.h'
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*(-?\d+)\b',
                      header.read_text())
    if match is None:
        raise ValueError(f'No numeric native status: {name}')
    return int(match.group(1))
