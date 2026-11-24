"""Check exact Chinese keys and format tokens in English.ini."""
import re
from pathlib import Path

src = Path(__file__).resolve().parents[1] / 'src'
data = (src / 'lang' / 'English.ini').read_bytes()
assert data.startswith(b'\xef\xbb\xbf')
assert b'\n' not in data.replace(b'\r\n', b'')
entries = {}
for key, value in re.findall(r'^([^;\s][^=\r\n]*?)\s*=\s*"([^"]*)"\s*$',
                             data.decode('utf-8-sig'), re.M):
    assert key not in entries, key
    entries[key] = value

source = (src / 'detail_window.cpp').read_text(encoding='utf-8-sig')
chart = source.split('void CDetailWindow::DrawHourlyChart(', 1)[1].split('\n}', 1)[0]
keys = set(re.findall(r'TR\(L"([^"]+)"\)', chart))
keys.update(re.findall(r'm_history_notice = L"([^"]+)"', source))
keys.update(re.findall(r'TR\(L"([^"]*%lu[^"]*)"\)', source))
keys.add('流量图表')
assert len(keys) >= 19
for key in keys:
    assert entries.get(key), key
    assert re.findall(r'%[a-z]+', key) == re.findall(r'%[a-z]+', entries[key]), key