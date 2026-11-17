"""quest_player_build.py -- the player's PC builds the Quest 3 app from the player's own disc (ac0oculus installer, 2026-11-17).

  python -I -B quest_player_build.py --app DIR --disc ISO_OR_FOLDER [--ndk DIR] [--accept-android-sdk-license] [--key FILE]
                                     [--jobs N] [--check-disc] [--find-ndk]

DIR is the install folder (the setup's {app}); it holds quest/ (the runtime's prebuilt objects, headers, recipe.json, the APK's
prebuilt parts), recomp/ (the recompilers and their configuration), python/ and this script. Steps, each named in the output:
  disc      the US executable SLUS_213.46 read out of the image (or the folder), its SHA-256 compared: the US disc only
  recompile ps2recomp, the vector-unit programs from the executable, vurecomp -> build/gen_zero/ (the game's code: stays here)
  ndk       Android NDK r29: --ndk, the saved one, ANDROID_NDK_ROOT, Android Studio's folder, or the download from Google
            (dl.google.com, 834 MB, SHA-1 checked) -- only with --accept-android-sdk-license: the player has read and accepted
            the Android SDK License Agreement (the setup shows it). Only what the build needs is unpacked (toolchain/android-ndk-r29).
  compile   the generated code and ps2_vu.c, with the NDK's clang and the flags CMake uses (recipe.json)
  link      libmain.so from the prebuilt runtime objects and the new ones, in CMake's order; stripped as Gradle does
  apk       quest/apk/base.apk (manifest, shaders) + libmain.so + the OpenXR loader + classes.dex, STORED and aligned, signed (APK
            Signature Scheme v2) with the player's own key (--key, default %APPDATA%\\ac0oculus\\apk_signing_key.json: kept when
            the program is uninstalled, so a later build installs over this one), checked
  headset   headset/: what goes to /sdcard/ac0oculus/ on the Quest (gen_zero/ps2_image.bin here; textures/ and the cockpit sets
            the setup copied in); the disc image itself is pushed from where it is (ac0oculus.json)
Exit status: 0 built; 2 usage; 10 no disc; 11 not the US disc; 12 no NDK (not found, licence not accepted, download failed);
13 compile; 14 link; 15 APK; 16 recompile. Every failure prints one line that starts with "ac0oculus: ERROR" and says what to do.
--check-disc: only the disc step (the setup's disc page). --find-ndk: only prints the NDK that would be used, or nothing."""
import argparse
import concurrent.futures
import hashlib
import json
import os
import shlex
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import zipfile

SLUS_SHA256 = '34ee58ca92a6e8913aa84b6dc5af3534b878a44ca4e33173e962c6090ea243c1'
SLUS_NAME = 'SLUS_213.46'
NDK_VERSION = '29.0.14206865'
NDK_URL = 'https://dl.google.com/android/repository/android-ndk-r29-windows.zip'
NDK_SHA1 = 'ab3bb30fbb9e6903666d60c55d11e78b04e07472'          # Google's repository2-3.xml, ndk;29.0.14206865, windows
NDK_SIZE = 833850862
NDK_TOP = 'android-ndk-r29/'
# what the build reads from the NDK: the driver, the linker, strip, their DLLs, clang's own headers and the arm64 runtime, the sysroot's
# headers and arm64 libraries (the rest of the 2.4 GB stays in the zip)
NDK_KEEP = ('source.properties',
            'toolchains/llvm/prebuilt/windows-x86_64/bin/clang.exe', 'toolchains/llvm/prebuilt/windows-x86_64/bin/ld.lld.exe',
            'toolchains/llvm/prebuilt/windows-x86_64/bin/lld.exe', 'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe',
            'toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-objcopy.exe',
            'toolchains/llvm/prebuilt/windows-x86_64/bin/*.dll', 'toolchains/llvm/prebuilt/windows-x86_64/lib/clang/*/include/',
            'toolchains/llvm/prebuilt/windows-x86_64/lib/clang/*/lib/linux/aarch64/',
            'toolchains/llvm/prebuilt/windows-x86_64/lib/clang/*/lib/linux/libclang_rt.builtins-aarch64-android.a',
            'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/include/',
            'toolchains/llvm/prebuilt/windows-x86_64/sysroot/usr/lib/aarch64-linux-android/',
            'toolchains/llvm/prebuilt/windows-x86_64/lib/*.dll')
PREBUILT = 'toolchains/llvm/prebuilt/windows-x86_64'


class Fail(Exception):
    def __init__(self, code, msg):
        Exception.__init__(self, msg)
        self.code = code


def say(s):
    print(s, flush=True)


def stage(name):
    say('@stage %s' % name)


def app_config_path(app):
    return os.path.join(app, 'ac0oculus.json')


def load_config(app):
    try:
        with open(app_config_path(app), encoding='utf-8') as fp:
            return json.load(fp)
    except (OSError, ValueError):
        return {}


def save_config(app, cfg):
    tmp = app_config_path(app) + '.tmp'
    with open(tmp, 'w', encoding='utf-8', newline='\n') as fp:
        json.dump(cfg, fp, indent=1)
    os.replace(tmp, app_config_path(app))


# ---------------------------------------------------------------- the disc

def iso_find(f, name):
    """(sector, size) of a file in the root directory of an ISO 9660 image, or None"""
    f.seek(16 * 2048)
    pvd = f.read(2048)
    if pvd[1:6] != b'CD001':
        return None
    root = pvd[156:190]
    lba, size = struct.unpack('<I', root[2:6])[0], struct.unpack('<I', root[10:14])[0]
    f.seek(lba * 2048)
    data = f.read(size)
    pos = 0
    while pos < len(data):
        n = data[pos]
        if n == 0:
            pos = (pos // 2048 + 1) * 2048
            continue
        rec = data[pos:pos + n]
        fid = rec[33:33 + rec[32]].decode('ascii', 'replace').split(';')[0]
        if fid.upper() == name.upper():
            return struct.unpack('<I', rec[2:6])[0], struct.unpack('<I', rec[10:14])[0]
        pos += n
    return None


def read_elf(disc):
    """the executable's bytes from an image or a folder; Fail 10 / 11 with the reason"""
    if not disc:
        raise Fail(10, 'no disc given: put the ISO image of your own US disc beside the setup, or choose it on the disc page')
    if os.path.isdir(disc):
        p = os.path.join(disc, SLUS_NAME)
        if not os.path.isfile(p):
            raise Fail(11, '%s has no %s: not the US disc of Ace Combat Zero (SLUS-21346)' % (disc, SLUS_NAME))
        with open(p, 'rb') as fp:
            return fp.read()
    if not os.path.isfile(disc):
        raise Fail(10, 'the disc image %s is not there (moved or deleted?)' % disc)
    with open(disc, 'rb') as f:
        hit = iso_find(f, SLUS_NAME)
        if hit is None:
            raise Fail(11, '%s is not an ISO image of the US disc of Ace Combat Zero (no %s in it)' % (disc, SLUS_NAME))
        f.seek(hit[0] * 2048)
        return f.read(hit[1])


def check_disc(disc):
    elf = read_elf(disc)
    h = hashlib.sha256(elf).hexdigest()
    if h != SLUS_SHA256:
        raise Fail(11, 'the disc has %s, but not the US release this build is made for (its SHA-256 is %s...): only SLUS-21346 works'
                   % (SLUS_NAME, h[:16]))
    return elf


# ---------------------------------------------------------------- the NDK

def ndk_ok(d):
    try:
        with open(os.path.join(d, 'source.properties'), encoding='utf-8') as fp:
            props = fp.read()
    except OSError:
        return False
    return ('Pkg.Revision = %s' % NDK_VERSION) in props and os.path.isfile(os.path.join(d, PREBUILT, 'bin', 'clang.exe'))


def ndk_candidates(app, given):
    c = []
    if given:
        c.append(given)
    cfg = load_config(app)
    if cfg.get('ndk'):
        c.append(cfg['ndk'])
    if os.environ.get('ANDROID_NDK_ROOT'):
        c.append(os.environ['ANDROID_NDK_ROOT'])
    la = os.environ.get('LOCALAPPDATA')
    if la:
        c.append(os.path.join(la, 'Android', 'Sdk', 'ndk', NDK_VERSION))
    c.append(os.path.join(app, 'toolchain', 'android-ndk-r29'))
    return c


def keep_member(rel):
    import fnmatch
    for k in NDK_KEEP:
        if k.endswith('/'):
            if fnmatch.fnmatchcase(rel, k + '*') or fnmatch.fnmatchcase(rel + '/', k):
                return True
        elif fnmatch.fnmatchcase(rel, k):
            return True
    return False


def download(url, dst, sha1, size):
    part = dst + '.part'
    h = hashlib.sha1()
    got = 0
    t0 = time.time()
    with urllib.request.urlopen(url, timeout=60) as r, open(part, 'wb') as out:
        last = 0
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            out.write(b)
            h.update(b)
            got += len(b)
            if time.time() - last > 2:
                last = time.time()
                say('@progress %.3f' % (got / float(size or 1)))
                say('  %d of %d MB, %.1f MB/s' % (got >> 20, size >> 20, got / 1048576.0 / max(0.1, time.time() - t0)))
    if h.hexdigest() != sha1:
        os.remove(part)
        raise Fail(12, 'the download of %s did not match Google\'s published SHA-1 (got %s): deleted; try again later' % (url, h.hexdigest()))
    os.replace(part, dst)


def unpack_ndk(zpath, dest):
    tmp = dest + '.new'
    shutil.rmtree(tmp, ignore_errors=True)
    n = 0
    with zipfile.ZipFile(zpath) as z:
        for zi in z.infolist():
            if not zi.filename.startswith(NDK_TOP) or zi.is_dir():
                continue
            rel = zi.filename[len(NDK_TOP):]
            if not keep_member(rel):
                continue
            out = os.path.join(tmp, *rel.split('/'))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            with z.open(zi) as src, open(out, 'wb') as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            n += 1
    if not ndk_ok(tmp):
        raise Fail(12, 'the NDK archive %s did not hold what the build needs' % zpath)
    shutil.rmtree(dest, ignore_errors=True)
    os.replace(tmp, dest)
    say('ndk: %d files unpacked into %s' % (n, dest))


def find_ndk(app, given, accept, url=NDK_URL, sha1=NDK_SHA1, size=NDK_SIZE):
    if given and not ndk_ok(given):
        raise Fail(12, 'the NDK folder %s is not Android NDK r29 (%s)' % (given, NDK_VERSION))
    for c in ndk_candidates(app, given):
        if c and ndk_ok(c):
            return os.path.abspath(c)
    if not accept:
        raise Fail(12, 'Android NDK r29 is needed and was not found on this PC; the setup downloads it from Google only after you '
                   'accept the Android SDK License Agreement on its page (or install NDK r29 with Android Studio and build again)')
    tc = os.path.join(app, 'toolchain')
    os.makedirs(tc, exist_ok=True)
    zpath = os.path.join(tc, 'android-ndk-r29-windows.zip')
    say('ndk: downloading %s (%d MB) from Google' % (url, size >> 20))
    try:
        download(url, zpath, sha1, size)
    except Fail:
        raise
    except Exception as e:                                      # (network errors, a proxy, a full disk)
        raise Fail(12, 'the NDK could not be downloaded (%s): check the connection and build again' % e)
    dest = os.path.join(tc, 'android-ndk-r29')
    unpack_ndk(zpath, dest)
    os.remove(zpath)
    return dest


# ---------------------------------------------------------------- compile, link, package

def run(cmd, cwd=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, encoding='utf-8', errors='replace')
    return r.returncode, (r.stdout + r.stderr).strip()


def build(app, disc, ndk, key, jobs):
    q = os.path.join(app, 'quest')
    with open(os.path.join(q, 'recipe.json'), encoding='utf-8') as fp:
        rc = json.load(fp)
    b = os.path.join(app, 'build')
    gen = os.path.join(b, 'gen_zero')
    obj = os.path.join(b, 'obj')
    os.makedirs(obj, exist_ok=True)

    stage('disc')
    elf = check_disc(disc)
    say('disc: %s, the US executable (SHA-256 checked)' % disc)

    stage('recompile')
    t0 = time.time()
    elfp = os.path.join(b, SLUS_NAME)
    with open(elfp, 'wb') as fp:
        fp.write(elf)
    rec = os.path.join(app, 'recomp')
    tmp = gen + '.new'
    shutil.rmtree(tmp, ignore_errors=True)
    sys.path.insert(0, rec)
    import ps2recomp.__main__ as r1
    cfg = os.path.join(rec, 'config_zero')
    if r1.main([elfp, '-o', tmp, '--ida-db', os.path.join(cfg, 'ida_like_zero.json'), '--ida-seeds', os.path.join(cfg, 'seeds_zero.json'),
                '--symbols', os.path.join(cfg, 'sdk', 'sdk_symbols.json'), '--symbols', os.path.join(cfg, 'sdk', 'manual_symbols.json'),
                '--overrides', os.path.join(cfg, 'sdk', 'overrides_draft.json')]):
        raise Fail(16, 'the recompiler failed (the lines above say where)')
    import vu_images_from_elf as vui
    imgs = os.path.join(b, 'vu_programs')
    shutil.rmtree(imgs, ignore_errors=True)
    if vui.build(elfp, os.path.join(cfg, 'vu_recipe.json'), imgs):
        raise Fail(16, 'the vector-unit programs could not be read from the executable')
    import vurecomp.__main__ as r2
    if r2.main(['--programs', imgs, '-o', os.path.join(tmp, 'ps2_vu1_progs.inc')]):
        raise Fail(16, 'the vector-unit recompiler failed')
    shutil.rmtree(gen, ignore_errors=True)
    os.replace(tmp, gen)
    shutil.rmtree(imgs, ignore_errors=True)
    say('recompile: %d files in %.1f s' % (len(os.listdir(gen)), time.time() - t0))

    tc = os.path.join(ndk, *PREBUILT.split('/'))
    clang = os.path.join(tc, 'bin', 'clang.exe')
    sysroot = os.path.join(tc, 'sysroot').replace('\\', '/')
    subst = {'{src}': q.replace('\\', '/'), '{build}': b.replace('\\', '/'), '{ndk}': ndk.replace('\\', '/'),
             '{openxr}': q.replace('\\', '/'), '{openxr_so}': os.path.join(q, 'apk', 'lib', 'libopenxr_loader.so').replace('\\', '/')}

    def sub(s):
        for k, v in subst.items():
            s = s.replace(k, v)
        return s

    base = [clang, '--target=' + rc['target'], '--sysroot=' + sysroot]
    inc = ['-I' + os.path.join(q, 'runtime', 'include'), '-I' + gen, '-I' + os.path.join(q, 'third_party', 'sse2neon')]

    stage('compile')
    t0 = time.time()
    jobs_list = []
    for o in rc['objects']:
        if o.startswith('@gen:'):
            src = os.path.join(gen, o[5:])
            jobs_list.append((src, os.path.join(obj, o[5:] + '.o'), shlex.split(sub(rc['defines'])) + inc + shlex.split(sub(rc['flags_gen']))))
        elif o == '@vu':
            src = os.path.join(q, 'runtime', 'src', 'ps2_vu.c')
            jobs_list.append((src, os.path.join(obj, 'ps2_vu.c.o'), shlex.split(sub(rc['defines_vu'])) + inc + shlex.split(sub(rc['flags_vu']))))
    # the biggest first: the vector-unit file and the largest generated files set the wall time
    jobs_list.sort(key=lambda j: -os.path.getsize(j[0]) * (4 if j[0].endswith('ps2_vu.c') else 1))
    done = [0]

    def one(j):
        src, out, flags = j
        code, text = run(base + flags + ['-o', out, '-c', src])
        done[0] += 1
        say('@progress %.3f' % (done[0] / float(len(jobs_list))))
        return src, code, text

    with concurrent.futures.ThreadPoolExecutor(max_workers=jobs) as ex:
        for src, code, text in ex.map(one, jobs_list):
            if code:
                say(text[-4000:])
                raise Fail(13, 'compiling %s failed' % os.path.basename(src))
    say('compile: %d files in %.1f s on %d threads' % (len(jobs_list), time.time() - t0, jobs))

    stage('link')
    t0 = time.time()
    rsp = os.path.join(b, 'link.rsp')
    items = []
    for o in rc['objects']:
        if o.startswith('@gen:'):
            items.append(os.path.join(obj, o[5:] + '.o'))
        elif o == '@vu':
            items.append(os.path.join(obj, 'ps2_vu.c.o'))
        else:
            items.append(os.path.join(q, *o.split('/')))
    libs = [sub(l) for l in rc['link_libs']]
    libs = [os.path.join(q, 'liblua54.a') if l == 'liblua54.a' else l for l in libs]
    with open(rsp, 'w', encoding='utf-8', newline='\n') as fp:
        fp.write(' '.join('"%s"' % p.replace('\\', '/') for p in items + libs))
    so = os.path.join(b, 'libmain.so')
    cmd = base + ['-fPIC'] + shlex.split(sub(rc['language_flags'])) + shlex.split(sub(rc['link_flags'])) + \
        ['-Wl,-soname,' + rc['soname'], '-o', so, '@' + rsp]
    code, text = run(cmd, cwd=b)
    if code:
        say(text[-4000:])
        raise Fail(14, 'linking libmain.so failed')
    stripped = os.path.join(b, 'libmain.stripped.so')
    code, text = run([os.path.join(tc, 'bin', 'llvm-strip.exe'), '--strip-unneeded', '-o', stripped, so])
    if code:
        say(text)
        raise Fail(14, 'stripping libmain.so failed')
    say('link: libmain.so %.1f MB (%.1f MB stripped) in %.1f s' % (os.path.getsize(so) / 1048576.0, os.path.getsize(stripped) / 1048576.0,
                                                                  time.time() - t0))

    stage('apk')
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import apk_tool
    apkdir = os.path.join(q, 'apk')
    unsigned = os.path.join(b, 'unsigned.apk')
    apk = os.path.join(app, 'ac0oculus.apk')
    try:
        apk_tool.cmd_pack(os.path.join(apkdir, 'base.apk'), unsigned,
                          ['lib/arm64-v8a/libmain.so=' + stripped, 'lib/arm64-v8a/libopenxr_loader.so=' + os.path.join(apkdir, 'lib', 'libopenxr_loader.so'),
                           'classes.dex=' + os.path.join(apkdir, 'classes.dex')])
        if apk_tool.cmd_libs(unsigned):
            raise Fail(15, 'the libraries are not aligned in the APK')
        apk_tool.cmd_keygen(key)
        apk_tool.cmd_sign(unsigned, apk + '.new', key)
        if apk_tool.cmd_verify(apk + '.new', key):
            raise Fail(15, 'the new APK\'s signature does not verify')
        os.replace(apk + '.new', apk)
    except SystemExit as e:
        raise Fail(15, str(e))
    os.remove(unsigned)
    with open(apk, 'rb') as fp:
        h = hashlib.sha256(fp.read()).hexdigest()
    say('apk: %s, %.1f MB, sha256 %s' % (apk, os.path.getsize(apk) / 1048576.0, h))

    stage('headset')
    hs = os.path.join(app, 'headset')
    os.makedirs(os.path.join(hs, 'gen_zero'), exist_ok=True)
    shutil.copyfile(os.path.join(gen, 'ps2_image.bin'), os.path.join(hs, 'gen_zero', 'ps2_image.bin'))
    os.remove(elfp)
    return apk


def summary(app):
    hs = os.path.join(app, 'headset')
    have = lambda p: os.path.isdir(os.path.join(hs, p)) and any(os.scandir(os.path.join(hs, p)))
    lines = ['headset/ holds what goes to /sdcard/ac0oculus/ on the Quest:']
    for p, what in (('gen_zero', 'the recompiler\'s memory image (needed)'), ('textures', 'controller glyphs and texture packs'),
                    ('cockpits_q4', 'the 3D cockpits'), ('cockpits_ql_t9', 'the cockpits\' detail levels')):
        lines.append('  %-15s %s' % (p + '/', what if have(p) else 'NOT THERE: ' + what +
                                    (' -- without them the game flies from its own views' if p == 'cockpits_q4' else '')))
    return '\n'.join(lines)


class Tee:
    def __init__(self, *outs):
        self.outs = outs

    def write(self, s):
        for o in self.outs:
            o.write(s)
        return len(s)

    def flush(self):
        for o in self.outs:
            o.flush()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--app', required=True)
    ap.add_argument('--disc')
    ap.add_argument('--ndk')
    ap.add_argument('--accept-android-sdk-license', action='store_true')
    ap.add_argument('--key')
    ap.add_argument('--jobs', type=int, default=max(1, min(8, os.cpu_count() or 1)))
    ap.add_argument('--check-disc', action='store_true')
    ap.add_argument('--find-ndk', action='store_true')
    ap.add_argument('--log', help='also write everything printed to this file')
    ap.add_argument('--ndk-url', default=NDK_URL, help=argparse.SUPPRESS)        # (tests: a file:// archive)
    ap.add_argument('--ndk-sha1', default=NDK_SHA1, help=argparse.SUPPRESS)
    ap.add_argument('--ndk-size', type=int, default=NDK_SIZE, help=argparse.SUPPRESS)
    a = ap.parse_args(argv)
    app = os.path.abspath(a.app)
    if a.log:
        sys.stdout = Tee(sys.stdout, open(a.log, 'w', encoding='utf-8', newline='\n'))
    cfg = load_config(app)
    disc = a.disc or cfg.get('disc')
    try:
        if a.check_disc:
            check_disc(disc)
            say('ok: %s is the US disc (SLUS-21346)' % disc)
            return 0
        if a.find_ndk:
            for c in ndk_candidates(app, a.ndk):
                if c and ndk_ok(c):
                    say(os.path.abspath(c))
                    return 0
            return 1
        t0 = time.time()
        stage('ndk')
        ndk = find_ndk(app, a.ndk, a.accept_android_sdk_license or cfg.get('android_sdk_license_accepted'), a.ndk_url, a.ndk_sha1,
                       a.ndk_size)
        say('ndk: %s' % ndk)
        key = a.key or cfg.get('key') or os.path.join(os.environ.get('APPDATA', app), 'ac0oculus', 'apk_signing_key.json')
        cfg.update({'disc': os.path.abspath(disc) if disc else '', 'ndk': ndk, 'key': key,
                    'package': open(os.path.join(app, 'quest', 'package.txt'), encoding='utf-8').read().strip()})
        if a.accept_android_sdk_license:
            cfg['android_sdk_license_accepted'] = time.strftime('%Y-%m-%d')
        save_config(app, cfg)
        apk = build(app, disc, ndk, key, a.jobs)
        say('@done %.0f' % (time.time() - t0))
        say('ac0oculus: built %s in %.0f s' % (apk, time.time() - t0))
        say(summary(app))
        say('Next: connect the Quest 3 (developer mode, USB debugging allowed) and start "ac0oculus - copy to the Quest" from the Start menu.')
        return 0
    except Fail as e:
        say('ac0oculus: ERROR %d: %s' % (e.code, e))
        return e.code


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
