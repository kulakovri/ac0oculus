"""apkzip.py -- the zip and template steps of tools/quest/make_apk.sh and check_apk.sh (there is no zip tool in Git Bash).

  python -I apkzip.py fill TEMPLATE OUT KEY=VALUE ...   replace @KEY@ in TEMPLATE (values XML-escaped unless KEY ends in _RAW,
                                                        then @KEY_RAW@ -> @KEY@ in the template is filled verbatim)
  python -I apkzip.py addlibs APK ARCNAME=FILE ...      append FILEs to APK as ARCNAME, STORED (uncompressed), fixed date
  python -I apkzip.py libs APK                          one line per lib/*.so entry: name, method, size, data offset,
                                                        offset % 16384; exit 1 if one is compressed
  python -I apkzip.py extract APK DIR                   write every lib/arm64-v8a/*.so of APK into DIR (for check_so.sh)

Pure standard library; streams big files (the game's libmain.so is 60-100 MB)."""
import os
import shutil
import struct
import sys
import zipfile

FIXED_DATE = (2008, 1, 1, 0, 0, 0)     # what the Android build tools use: reproducible bytes for unchanged inputs


def xml_escape(s):
    return s.replace('&', '&amp;').replace('<', '&lt;').replace('>', '&gt;').replace('"', '&quot;')


def cmd_fill(args):
    tmpl, out = args[0], args[1]
    s = open(tmpl, encoding='utf-8').read()
    for kv in args[2:]:
        k, v = kv.split('=', 1)
        if k.endswith('_RAW'):
            s = s.replace('@' + k[:-4] + '@', v)
        else:
            s = s.replace('@' + k + '@', xml_escape(v))
    left = [w for w in s.split('@') if w.isupper() and w.replace('_', '').isalnum()]
    if left:
        print('apkzip fill: unfilled placeholders: %s' % ', '.join(sorted(set(left))), file=sys.stderr)
        return 1
    open(out, 'w', encoding='utf-8', newline='\n').write(s)
    return 0


def cmd_addlibs(args):
    apk = args[0]
    with zipfile.ZipFile(apk, 'a', allowZip64=True) as z:
        have = set(z.namelist())
        for kv in args[1:]:
            arc, path = kv.split('=', 1)
            if arc in have:
                print('apkzip addlibs: %s already in %s' % (arc, apk), file=sys.stderr)
                return 1
            zi = zipfile.ZipInfo(arc, FIXED_DATE)
            zi.compress_type = zipfile.ZIP_STORED
            zi.external_attr = 0o644 << 16
            with open(path, 'rb') as src, z.open(zi, 'w', force_zip64=os.path.getsize(path) > 0x7fffffff) as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
    return 0


def lib_entries(apk):
    """(name, method, size, data_offset) of every lib/ entry; the data offset read from the local header."""
    out = []
    with zipfile.ZipFile(apk) as z, open(apk, 'rb') as f:
        for zi in z.infolist():
            if not zi.filename.startswith('lib/'):
                continue
            f.seek(zi.header_offset)
            h = f.read(30)
            if h[:4] != b'PK\x03\x04':
                raise SystemExit('bad local header for %s' % zi.filename)
            n, e = struct.unpack('<HH', h[26:30])
            out.append((zi.filename, zi.compress_type, zi.file_size, zi.header_offset + 30 + n + e))
    return out


def cmd_libs(args):
    bad = 0
    for name, method, size, off in lib_entries(args[0]):
        stored = method == zipfile.ZIP_STORED
        if name.endswith('.so') and not stored:
            bad += 1
        print('%s %s %d %d %d' % (name, 'stored' if stored else 'deflated', size, off, off % 16384))
    return 1 if bad else 0


def cmd_extract(args):
    apk, d = args
    os.makedirs(d, exist_ok=True)
    with zipfile.ZipFile(apk) as z:
        for zi in z.infolist():
            if zi.filename.startswith('lib/arm64-v8a/') and zi.filename.endswith('.so'):
                with z.open(zi) as src, open(os.path.join(d, os.path.basename(zi.filename)), 'wb') as dst:
                    shutil.copyfileobj(src, dst, 1 << 20)
                print(os.path.basename(zi.filename))
    return 0


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = {'fill': cmd_fill, 'addlibs': cmd_addlibs, 'libs': cmd_libs, 'extract': cmd_extract}.get(sys.argv[1])
    if not cmd:
        print(__doc__)
        return 2
    return cmd(sys.argv[2:])


sys.exit(main())
