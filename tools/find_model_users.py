#!/usr/bin/env python3
"""Finds which game objects (units, buildings, projectiles ...) use a W3D model, by searching the INI files extracted from the game archives.

  find_model_users.py <folder with the extracted Data\INI tree> <model name> [<model name> ...]

Example:  find_model_users.py E:\GeneralsModels\ini ABNukeMissle
Extract the INI files first:  bigtool.py extract <GameDir>\INIZH.big E:\GeneralsModels\ini\zh   (and the base game's INI.big, PatchINI.big)
The model name is the W3D file name without extension; damaged/destroyed variants are listed as well (ABNukeMissle_D ...).
"""
import os
import re
import sys


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    root = sys.argv[1]
    wanted = [m.lower() for m in sys.argv[2:]]
    obj_re = re.compile(r'^\s*(?:Object|ObjectReskin|ChildObject)\s+(\S+)', re.I)
    model_re = re.compile(r'^\s*(?:Model|ModelName)\s*=\s*(\S+)', re.I)
    found = {}
    for r, _, files in os.walk(root):
        for f in files:
            if not f.lower().endswith('.ini'):
                continue
            path = os.path.join(r, f)
            obj = None
            for n, line in enumerate(open(path, encoding='latin-1', errors='replace'), 1):
                line = line.split(';')[0]
                m = obj_re.match(line)
                if m:
                    obj = m.group(1)
                    continue
                m = model_re.match(line)
                if m and obj:
                    model = m.group(1).lower()
                    for w in wanted:
                        if model == w or model.startswith(w + '_') or model.startswith(w):
                            found.setdefault((obj, os.path.relpath(path, root)), set()).add(m.group(1))
    if not found:
        print('no object uses', ', '.join(sys.argv[2:]))
    for (obj, path), models in sorted(found.items()):
        print('%-40s %-60s %s' % (obj, path, ', '.join(sorted(models))))
    return 0


sys.exit(main())
