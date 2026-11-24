"""Source-level guards for detail-window dispatch (not a host UI test)."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'src' / 'detail_window.cpp').read_text(encoding='utf-8-sig')


def method(signature):
    return source.split(f'CDetailWindow::{signature}', 1)[1].split('\n}', 1)[0]


assert 'legacyMerged' not in method('LoadHistory()')