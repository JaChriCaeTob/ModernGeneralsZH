#!/usr/bin/env python3
"""List or extract files from EA .big archives (BIGF format, as used by Generals / Zero Hour).

  bigtool.py list <archive.big> [substring]
  bigtool.py extract <archive.big> <out_dir> [substring ...]     (case-insensitive substring match on the stored path)
"""
import os, struct, sys

def read_index(f):
    magic = f.read(4)
    if magic not in (b'BIGF', b'BIG4'):
        raise SystemExit('not a BIG archive')
    f.read(4)                                   # archive size (little endian)
    count = struct.unpack('>I', f.read(4))[0]
    f.read(4)                                   # header size
    entries = []
    for _ in range(count):
        off, size = struct.unpack('>II', f.read(8))
        name = b''
        while True:
            c = f.read(1)
            if c in (b'\0', b''):
                break
            name += c
        entries.append((name.decode('latin-1'), off, size))
    return entries

def main(argv):
    if len(argv) < 3 or argv[1] not in ('list', 'extract'):
        print(__doc__); return 1
    with open(argv[2], 'rb') as f:
        entries = read_index(f)
        if argv[1] == 'list':
            sub = argv[3].lower() if len(argv) > 3 else ''
            for n, o, s in entries:
                if sub in n.lower():
                    print('%10d  %s' % (s, n))
            return 0
        out = argv[3]
        subs = [s.lower() for s in argv[4:]]
        n_done = 0
        for n, o, s in entries:
            if subs and not any(x in n.lower() for x in subs):
                continue
            dest = os.path.join(out, *n.replace(chr(92), '/').split('/'))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            f.seek(o)
            with open(dest, 'wb') as g:
                g.write(f.read(s))
            n_done += 1
        print('extracted %d files to %s' % (n_done, out))
    return 0

if __name__ == '__main__':
    sys.exit(main(sys.argv))
