"""Check exact Chinese keys and format tokens in English.ini."""
import re
from pathlib import Path

src = Path(__file__).resolve().parents[1] / 'src'
data = (src / 'lang' / 'English.ini').read_bytes()
assert data.startswith(b'\xef\xbb\xbf')
assert b'\n' not in data.replace(b'\r\n', b'')
entries = {}
for key, value in re.findall(r'^([^;\s][^=\r\n]*?)\s*=\s*"((?:\\[^\r\n]|[^"\\\r\n])*)"\s*$',
                             data.decode('utf-8-sig'), re.M):
    assert key not in entries, key
    entries[key] = value

source = (src / 'detail_window.cpp').read_text(encoding='utf-8-sig')
keys = set(re.findall(r'TR\(L"([^"\n]+)"\)', source))
keys = {re.sub(r'\\u([0-9a-fA-F]{4})', lambda match: chr(int(match[1], 16)), key) for key in keys}
keys.update(re.findall(r'm_history_notice = L"([^"]+)"', source))
for key in keys:
    assert entries.get(key), key
    assert re.findall(r'%[a-z]+', key) == re.findall(r'%[a-z]+', entries[key]), key