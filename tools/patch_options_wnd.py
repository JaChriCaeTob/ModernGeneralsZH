#!/usr/bin/env python3
"""Builds a patched Window\Menus\OptionsMenu.wnd that has three extra check boxes (ambient occlusion, bloom, FXAA) in the
advanced display pane. The original file is read from the game's WindowZH.big; the result is written as a loose file into the game
folder (loose files win over archives), so no original data is modified or redistributed.

  patch_options_wnd.py <GameDir>
"""
import os, re, subprocess, sys, tempfile

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
nl = '\r\n' if '\r\n' in text else '\n'
text = text.replace('\r\n', '\n')

def shift_window(t, name_or_rect, dy):
    """shift the window whose SCREENRECT upper-left is (x, y) by dy"""
    x, y = name_or_rect
    pat = re.compile(r'(SCREENRECT = UPPERLEFT: %d )(%d)(,\s*BOTTOMRIGHT: \d+ )(\d+)(,)' % (x, y))
    assert pat.search(t), name_or_rect
    return pat.sub(lambda m: '%s%d%s%d%s' % (m.group(1), int(m.group(2)) + dy, m.group(3), int(m.group(4)) + dy, m.group(5)), t, count=1)

# make room: low-res texture block moves down 24, particle cap block 12
for rect in [(160, 312), (172, 315), (240, 355), (427, 375), (160, 342), (160, 375)]:
    text = shift_window(text, rect, 24)
for rect in [(160, 416), (172, 419), (160, 476), (412, 476), (160, 445), (240, 459)]:
    text = shift_window(text, rect, 12)

# the container of the check boxes has to cover the new rows
text = re.sub(r'(SCREENRECT = UPPERLEFT: 160 112,\s*BOTTOMRIGHT: 622 )301', r'\g<1>340', text, count=1)

# clone the "smooth water" check box for the new ones
i = text.index('NAME = "OptionsMenu.wnd:CheckSmoothWater"')
start = text.rfind('          WINDOW', 0, i)
start = text.rfind('          CHILD', 0, start)
end = text.index('          END', i) + len('          END')
block = text[start:end + 1]
new_blocks = ''
for name, x, y, w in [('CheckAmbientOcclusion', 168, 264, 200), ('CheckBloom', 168, 288, 200), ('CheckFXAA', 384, 288, 230)]:
    b = block
    b = re.sub(r'UPPERLEFT: \d+ \d+,\s*BOTTOMRIGHT: \d+ \d+,', 'UPPERLEFT: %d %d,\n                         BOTTOMRIGHT: %d %d,' % (x, y, x + w, y + 24), b, count=1)
    b = b.replace('OptionsMenu.wnd:CheckSmoothWater', 'OptionsMenu.wnd:' + name)
    b = re.sub(r'TOOLTIPTEXT = "[^"]*"', 'TOOLTIPTEXT = ""', b)
    b = re.sub(r'(?m)^(\s*)TEXT = "[^"]*";', r'\1TEXT = "GUI:Shadows3D";', b)		# any valid key: the real label is set by the game code
    new_blocks += b
text = text[:end + 1] + new_blocks + text[end + 1:]

out = os.path.join(game, 'Window', 'Menus')
os.makedirs(out, exist_ok=True)
open(os.path.join(out, 'OptionsMenu.wnd'), 'w', encoding='latin-1', newline='').write(text.replace('\n', nl))
print('wrote', os.path.join(out, 'OptionsMenu.wnd'))
