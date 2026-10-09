#!/usr/bin/env python3
"""Builds a patched Window/Menus/OptionsMenu.wnd with a "graphics enhancements" group (a header and six check boxes) in the advanced
display pane. The original file is read from the game's WindowZH.big; the result is written as a loose file into the game folder
(loose files win over archives), so no original data is modified or redistributed.

  patch_options_wnd.py <GameDir>
"""
import os, re, subprocess, sys, tempfile

NL = chr(10)
here = os.path.dirname(os.path.abspath(__file__))
game = sys.argv[1]
tmp = tempfile.mkdtemp()
subprocess.check_call([sys.executable, os.path.join(here, 'bigtool.py'), 'extract', os.path.join(game, 'WindowZH.big'), tmp, 'optionsmenu.wnd'], stdout=subprocess.DEVNULL)
src = None
for root, _, files in os.walk(tmp):
    for f in files:
        if f.lower() == 'optionsmenu.wnd':
            src = os.path.join(root, f)
text = open(src, encoding='latin-1', newline='').read()
nl = chr(13) + chr(10) if chr(13) + chr(10) in text else NL
text = text.replace(chr(13) + chr(10), NL)

RECT = r'UPPERLEFT: \d+ \d+,\s*BOTTOMRIGHT: \d+ \d+,'


def shift_window(t, xy, dy):
    """shift the window whose SCREENRECT upper-left is (x, y) by dy"""
    x, y = xy
    pat = re.compile(r'(SCREENRECT = UPPERLEFT: %d )(%d)(,\s*BOTTOMRIGHT: \d+ )(\d+)(,)' % (x, y))
    assert pat.search(t), xy
    return pat.sub(lambda m: '%s%d%s%d%s' % (m.group(1), int(m.group(2)) + dy, m.group(3), int(m.group(4)) + dy, m.group(5)), t, count=1)


# make room for the group below the original check boxes
for rect in [(160, 312), (172, 315), (240, 355), (427, 375), (160, 342), (160, 375)]:
    text = shift_window(text, rect, 60)
for rect in [(160, 416), (172, 419), (160, 476), (412, 476), (160, 445), (240, 459)]:
    text = shift_window(text, rect, 48)
for rect in [(295, 521), (466, 521)]:
    text = shift_window(text, rect, 36)
text = re.sub(r'(SCREENRECT = UPPERLEFT: 151 68,\s*BOTTOMRIGHT: 636 )560', r'\g<1>596', text, count=1)
text = re.sub(r'(SCREENRECT = UPPERLEFT: 160 112,\s*BOTTOMRIGHT: 622 )301', r'\g<1>380', text, count=1)


def block_around(t, index):
    start = t.rfind('          WINDOW', 0, index)
    start = t.rfind('          CHILD', 0, start)
    end = t.index('          END', index) + len('          END')
    return start, end + 1


def clone(t, find_text, new_name, x, y, w, h):
    start, end = block_around(t, t.index(find_text))
    b = t[start:end]
    b = re.sub(RECT, 'UPPERLEFT: %d %d,' % (x, y) + NL + '                         BOTTOMRIGHT: %d %d,' % (x + w, y + h), b, count=1)
    b = re.sub(r'NAME = "[^"]*";', 'NAME = "OptionsMenu.wnd:%s";' % new_name, b, count=1)
    b = re.sub(r'TOOLTIPTEXT = "[^"]*"', 'TOOLTIPTEXT = ""', b)
    # any valid string key: the real labels are set by the game code
    b = re.sub(r'(?m)^(\s*)TEXT = "[^"]*";', lambda m: m.group(1) + 'TEXT = "GUI:Shadows3D";', b)
    return end, b


new_blocks = ''
anchor = None
# header: a static text cloned from the one above the original check boxes
hdr_find = 'UPPERLEFT: 167 112'
hs, he = block_around(text, text.index(hdr_find))
_, blk = clone(text, hdr_find, 'EnhancementsHeader', 160, 298, 462, 24)
new_blocks += blk
# check boxes cloned from "smooth water"
cb_find = 'NAME = "OptionsMenu.wnd:CheckSmoothWater"'
for name, x, y in [('CheckSoftShadows', 168, 324), ('CheckAmbientOcclusion', 322, 324), ('CheckBloom', 476, 324),
                   ('CheckFXAA', 168, 348), ('CheckUpdatedWater', 322, 348), ('CheckShockwaves', 476, 348)]:
    anchor, blk = clone(text, cb_find, name, x, y, 150, 24)
    new_blocks += blk
text = text[:anchor] + new_blocks + text[anchor:]

out = os.path.join(game, 'Window', 'Menus')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'OptionsMenu.wnd'), 'w', encoding='latin-1', newline='').write(text.replace(NL, nl))
print('wrote', os.path.join(out, 'OptionsMenu.wnd'))
