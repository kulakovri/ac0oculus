"""push_to_quest.py -- the installer's one click: the built app and its data onto a Meta Quest 3 over the USB cable (ac0oculus, 2026-11-17).

  python -I -B push_to_quest.py --app DIR [--adb PROGRAM] [--accept-android-sdk-license] [--no-disc] [--yes]

What it does, in order (each step printed, with its time):
  1. adb: --adb, the one this setup unpacked (toolchain/platform-tools), Android Studio's, or one on PATH; else Google's platform-tools
     r37.0.1 (8 MB, SHA-1 checked) when the Android SDK License Agreement was accepted in the setup (or --accept-android-sdk-license).
  2. exactly one headset, in developer mode, with USB debugging allowed ("unauthorized": put it on and allow this PC).
  3. adb install -r -g ac0oculus.apk (the app; -g: its permissions without dialogs).
  4. the app's access to its data folder: appops MANAGE_EXTERNAL_STORAGE allow (Horizon OS has no page for it).
  5. /sdcard/ac0oculus/: the disc image as disc.iso (from where it lies; 4.24 GB, about 20 s over USB 3), then everything under
     headset/ (gen_zero/ps2_image.bin, textures/, the cockpit sets). A file whose size on the headset already matches is skipped, so a
     second run only installs the new APK.
Exit status 0 = done; 1 = a step failed (the line says why); 2 = usage."""
import argparse
import hashlib
import json
import os
import shlex
import shutil
import subprocess
import sys
import time
import urllib.request
import zipfile

PT_URL = 'https://dl.google.com/android/repository/platform-tools_r37.0.1-win.zip'
PT_SHA1 = 'e03e78b1d80b396f1c3358e31251cb31740e1110'           # Google's repository2-3.xml, platform-tools 37.0.1 stable, windows
DATA = '/sdcard/ac0oculus'


def say(s):
    print(s, flush=True)


def find_adb(app, given, accept):
    if given:
        return shlex.split(given, posix=False)
    for c in (os.path.join(app, 'toolchain', 'platform-tools', 'adb.exe'),
              os.path.join(os.environ.get('LOCALAPPDATA', ''), 'Android', 'Sdk', 'platform-tools', 'adb.exe'),
              shutil.which('adb') or ''):
        if c and os.path.isfile(c):
            return [c]
    if not accept:
        raise SystemExit('ac0oculus: ERROR: no adb on this PC. Run the setup again and accept the Android SDK License Agreement (it then '
                         'downloads Google\'s platform-tools), or install the platform-tools yourself.')
    tc = os.path.join(app, 'toolchain')
    os.makedirs(tc, exist_ok=True)
    z = os.path.join(tc, 'platform-tools.zip')
    say('adb: downloading Google\'s platform-tools r37.0.1')
    h = hashlib.sha1()
    with urllib.request.urlopen(PT_URL, timeout=60) as r, open(z + '.part', 'wb') as out:
        while True:
            b = r.read(1 << 20)
            if not b:
                break
            h.update(b)
            out.write(b)
    if h.hexdigest() != PT_SHA1:
        os.remove(z + '.part')
        raise SystemExit('ac0oculus: ERROR: the platform-tools download did not match Google\'s SHA-1: deleted, try again later')
    os.replace(z + '.part', z)
    with zipfile.ZipFile(z) as zf:
        zf.extractall(tc)
    os.remove(z)
    return [os.path.join(tc, 'platform-tools', 'adb.exe')]


class Adb:
    def __init__(self, cmd):
        self.cmd = cmd

    def run(self, *args, timeout=None):
        r = subprocess.run(self.cmd + list(args), capture_output=True, text=True, encoding='utf-8', errors='replace', timeout=timeout)
        return r.returncode, (r.stdout + r.stderr).replace('\r', '').strip()

    def shell(self, line):
        return self.run('shell', line)[1]


def one_device(adb):
    code, out = adb.run('devices')
    devs = [l.split('\t') for l in out.splitlines()[1:] if '\t' in l]
    ok = [d for d in devs if d[1] == 'device']
    if len(ok) == 1:
        return ok[0][0]
    if any(d[1] == 'unauthorized' for d in devs):
        raise SystemExit('ac0oculus: ERROR: the headset has not allowed this PC yet: put it on and accept "Allow USB debugging" '
                         '(tick "Always allow from this computer"), then run this again.')
    if len(ok) > 1:
        raise SystemExit('ac0oculus: ERROR: more than one Android device is connected: leave only the Quest connected.')
    raise SystemExit('ac0oculus: ERROR: no headset found. Connect the Quest 3 with a USB-C data cable; its developer mode must be on '
                     '(Meta Horizon app on your phone > the headset > Developer mode).')


def remote_size(adb, path):
    out = adb.shell('stat -c %%s "%s" 2>/dev/null' % path)
    return int(out) if out.strip().isdigit() else -1


def push(adb, local, remote):
    sz = os.path.getsize(local)
    if remote_size(adb, remote) == sz:
        say('  = %s (%d bytes, already there)' % (remote, sz))
        return 0
    adb.shell('mkdir -p "%s"' % remote.rsplit('/', 1)[0])
    t = time.time()
    code, out = adb.run('push', local, remote)
    dt = max(0.001, time.time() - t)
    if code or 'error' in out.lower():
        raise SystemExit('ac0oculus: ERROR: copying %s failed: %s' % (local, out[-300:]))
    say('  + %s (%.1f MB in %.1f s, %.0f MB/s)' % (remote, sz / 1048576.0, dt, sz / 1048576.0 / dt))
    return sz


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('--app', required=True)
    ap.add_argument('--adb', help='the adb program (or a command line, for tests)')
    ap.add_argument('--accept-android-sdk-license', action='store_true')
    ap.add_argument('--no-disc', action='store_true', help='do not push the disc image (it is there already)')
    ap.add_argument('--yes', action='store_true', help='do not wait for Enter at the end')
    a = ap.parse_args(argv)
    app = os.path.abspath(a.app)
    try:
        with open(os.path.join(app, 'ac0oculus.json'), encoding='utf-8') as fp:
            cfg = json.load(fp)
    except (OSError, ValueError):
        cfg = {}
    pkg = cfg.get('package') or open(os.path.join(app, 'quest', 'package.txt'), encoding='utf-8').read().strip()
    apk = os.path.join(app, 'ac0oculus.apk')
    rc = 0
    t0 = time.time()
    try:
        if not os.path.isfile(apk):
            raise SystemExit('ac0oculus: ERROR: %s is not built yet: start "ac0oculus - build the APK again" first.' % apk)
        adb = Adb(find_adb(app, a.adb, a.accept_android_sdk_license or cfg.get('android_sdk_license_accepted')))
        dev = one_device(adb)
        say('headset: %s (%s, Horizon OS build %s)' % (dev, adb.shell('getprop ro.product.model'), adb.shell('getprop ro.build.version.incremental')))
        say('app: installing %s' % apk)
        code, out = adb.run('install', '-r', '-g', apk)
        if 'INSTALL_FAILED_UPDATE_INCOMPATIBLE' in out:
            raise SystemExit('ac0oculus: ERROR: an ac0oculus signed with another key is on the headset (built on another PC, or with '
                             'another key file). Uninstall it in the headset first (Library > ac0oculus > Uninstall; its saves go '
                             'with it), then run this again.')
        if code or 'Success' not in out:
            raise SystemExit('ac0oculus: ERROR: the install failed: %s' % out[-300:])
        adb.shell('appops set --uid %s MANAGE_EXTERNAL_STORAGE allow' % pkg)
        say('app: installed; storage access: %s' % adb.shell('appops get --uid %s MANAGE_EXTERNAL_STORAGE' % pkg))
        adb.shell('mkdir -p %s' % DATA)
        total = 0
        disc = cfg.get('disc', '')
        if not a.no_disc:
            if os.path.isfile(disc):
                total += push(adb, disc, DATA + '/disc.iso')
            elif os.path.isdir(disc):
                for d, _, files in os.walk(disc):
                    for f in files:
                        rel = os.path.relpath(os.path.join(d, f), disc).replace('\\', '/')
                        total += push(adb, os.path.join(d, f), DATA + '/disc/' + rel)
            else:
                raise SystemExit('ac0oculus: ERROR: the disc image %s is not there any more: put it back, or build again with the '
                                 'new place.' % disc)
        hs = os.path.join(app, 'headset')
        for d, _, files in os.walk(hs):
            for f in sorted(files):
                rel = os.path.relpath(os.path.join(d, f), hs).replace('\\', '/')
                total += push(adb, os.path.join(d, f), DATA + '/' + rel)
        say('done: %.1f MB copied in %.0f s. In the headset: Library > Unknown sources > ac0oculus.' % (total / 1048576.0, time.time() - t0))
    except SystemExit as e:
        say(str(e))
        rc = 1
    except subprocess.TimeoutExpired as e:
        say('ac0oculus: ERROR: adb did not answer (%s)' % e)
        rc = 1
    if not a.yes:
        try:
            input('Press Enter to close this window.')
        except EOFError:
            pass
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
