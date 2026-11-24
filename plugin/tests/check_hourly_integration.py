"""Source-level guards for detail-window dispatch (not a host UI test)."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'src' / 'detail_window.cpp').read_text(encoding='utf-8-sig')


def method(signature):
    return source.split(f'CDetailWindow::{signature}', 1)[1].split('\n}', 1)[0]


assert 'legacyMerged' not in method('LoadHistory()')
assert 'MigrateLegacyHistoryToHourly' not in source
assert 'history.dat"' not in source  # no old-file open/write/delete path

for name in ('RebuildRows()', 'ResortRows()', 'SortByColumn(', 'HitTestRow(',
             'OnLButtonDblClk(', 'OnRButtonDown(', 'DrawTableHeader('):
    assert 'm_active_tab == 2' in method(name).split('return', 1)[0], name

menu = method('ShowContextMenu(')
assert 'm_active_tab == 2' in menu.split('TrackPopupMenu', 1)[0]
dispatch = menu.split('TrackPopupMenu', 1)[1]
assert 'm_active_tab == 2' in dispatch.split('switch (cmd)', 1)[0]
assert dispatch.index('MB_DEFBUTTON2') < dispatch.index('== IDYES') < dispatch.index('TerminateProcess')