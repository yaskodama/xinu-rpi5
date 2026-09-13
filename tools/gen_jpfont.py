#!/usr/bin/env python3
"""device/video/jpfont.c を生成する。元は tools/jiskan16.bdf（JIS X 9051, X11 jis-misc）と
tools/8x13.bdf（X11 misc-misc, public domain）。JIS→Unicode は EUC-JP で引く。"""
import sys, os
here = os.path.dirname(os.path.abspath(__file__))
def parse_bdf(path):
    glyphs = {}
    enc = None; bbx = None; rows = None; inbm = False
    for line in open(path, 'r', errors='replace'):
        line = line.rstrip('\n')
        if line.startswith('STARTCHAR'): enc = None; rows = None; inbm = False
        elif line.startswith('ENCODING'): enc = int(line.split()[1])
        elif line.startswith('BBX'): bbx = tuple(int(x) for x in line.split()[1:5])
        elif line.startswith('BITMAP'): inbm = True; rows = []
        elif line.startswith('ENDCHAR'):
            if enc is not None and rows is not None: glyphs[enc] = (bbx, rows)
            inbm = False
        elif inbm: rows.append(line.strip())
    return glyphs
jis = parse_bdf(os.path.join(here, 'jiskan16.bdf')); asc = parse_bdf(os.path.join(here, '8x13.bdf'))
entries = {}
for code, (bbx, rows) in jis.items():
    b1, b2 = code >> 8, code & 0xFF
    try: ch = bytes([b1 | 0x80, b2 | 0x80]).decode('euc_jp')
    except Exception: continue
    cp = ord(ch)
    if cp < 0x80 or cp in entries: continue
    bits = bytes.fromhex(''.join(r.ljust(4, '0')[:4] for r in rows))
    if len(bits) == 32: entries[cp] = bits
out = sorted(entries.items())
asc16 = []
for cp in range(0x20, 0x7F):
    r = [int(x, 16) for x in asc[cp][1]]
    assert len(r) == 13
    asc16.append([0, 0] + r + [0])
dst = os.path.join(here, '..', 'device', 'video', 'jpfont.c')
with open(dst, 'w') as f:
    f.write('/* device/video/jpfont.c —— 16 ドットの日本語フォント（生成物。手で直さない）\n'
            ' * 元: X11 jiskan16.bdf（JIS X 9051-1984, "by permission to use"）と misc 8x13.bdf（public domain）。\n'
            ' * 生成: tools/gen_jpfont.py（Unicode 順に並べ、二分探索で引く）。\n'
            ' * jp_uni[i] の字形が jp_bits[i]（16 行 × 2 バイト、上位ビットが左）。\n'
            ' * asc16_bits は 8x13 を 8x16 の枠に置いたもの（上 2 行・下 1 行が空）。 */\n')
    f.write('const int jp_count = %d;\n' % len(out))
    f.write('const unsigned short jp_uni[%d] = {\n' % len(out))
    for i in range(0, len(out), 16): f.write('  ' + ','.join('0x%04X' % cp for cp, _ in out[i:i+16]) + ',\n')
    f.write('};\nconst unsigned char jp_bits[%d][32] = {\n' % len(out))
    for cp, b in out: f.write('  {' + ','.join('0x%02X' % x for x in b) + '},\n')
    f.write('};\nconst unsigned char asc16_bits[95][16] = {\n')
    for r in asc16: f.write('  {' + ','.join('0x%02X' % x for x in r) + '},\n')
    f.write('};\n')
print('wrote', dst, len(out), 'glyphs')
