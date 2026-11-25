"""Source-level guards for detail-window dispatch (not a host UI test)."""
from pathlib import Path

source = (Path(__file__).resolve().parents[1] / 'src' / 'detail_window.cpp').read_text(encoding='utf-8-sig')


def method(signature):
    return source.split(f'CDetailWindow::{signature}', 1)[1].split('\n}', 1)[0]


assert 'legacyMerged' not in source  # retired flag: nothing may read or set it
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

chart = method('DrawHourlyChart(')
assert 'const bool fits_summary = nav_left >= speed_right + nav_gap;' in chart
assert 'nav_left = area.right - nav_width;' in chart
assert 'const auto date_button = std::wstring(date_text)' in chart
assert 'nav_buttons[] = {&m_rcChartPrev, &m_rcChartDate, &m_rcChartNext, &m_rcChartToday};' in chart
assert 'area.top - (SUMMARY_H + nav_height) / 2' in chart
assert 'const int nav_bottom = nav_top + nav_height;' in chart
assert 'const int chart_toolbar_gap = max(12, (int)(10 * m_dpi_scale));' in chart
assert 'nav_bottom + chart_toolbar_gap' in chart
assert 'DrawHourlyChart(memDC, w, y, h, speed_right);' in source
click = method('OnLButtonDown(')
for button in ('m_rcChartPrev', 'm_rcChartDate', 'm_rcChartNext', 'm_rcChartToday'):
    assert f'PtInRect(&{button}' in click, button

toolbar = method('DrawTimeRangeButtons(')
assert 'return m_active_tab == 1 ? TIME_RANGE_H : 0;' in source
assert 'const int row_top = y + 3;' in toolbar
assert 'const int row_bottom = y + TIME_RANGE_H - 3;' in toolbar
assert 'm_rcClearBtn.left - action_gap - jump_bw' in toolbar
assert 'm_rcClearBtn.left - action_gap, row_bottom' in toolbar
assert 'm_rcCustomDates = {x, row_top' in toolbar
assert 'm_rcOpenChart.left - action_gap' in toolbar
for button in ('m_rcCustomDates', 'm_rcOpenChart', 'm_rcClearBtn'):
    assert f'PtInRect(&{button}' in source, button