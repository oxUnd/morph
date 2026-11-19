"""Read native status constants from the pinned public C contract."""
import re


def native_status(name, header):
    match = re.search(r'\b' + re.escape(name) + r'\s*=\s*(-?\d+)\b',
                      header.read_text())
    if match is None:
        raise ValueError(f'No numeric native status: {name}')
    return int(match.group(1))
