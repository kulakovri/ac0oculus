"""make_recipe.py BUILD_DIR STAGE_QUEST_DIR -- the developer's side of the player's Quest build (make_release_quest.sh): from a finished
Quest CMake build (tools/quest: PS2_QUEST=ON, PS2_STRIP_PATHS=ON), the runtime's objects that hold nothing of the game, in the link's own
order, and recipe.json: how the player's PC compiles the generated code and ps2_vu.c (the one runtime file that includes generated code)
and links libmain.so exactly as CMake does.

Written into STAGE_QUEST_DIR: obj/NNN_<name>.o (debug sections stripped; NNN keeps the link order), liblua54.a, recipe.json. Left out
on purpose: every object of the generated code and ps2_vu.c.o (both made from the player's disc). Absolute paths of this PC never go
into the recipe: the source tree becomes {src}, the NDK {ndk}, the OpenXR loader {openxr}; make_release_quest.sh's scan checks."""
import json
import os
import re
import shutil
import subprocess
import sys


def ninja_vars(text, header):
    """the indented `NAME = value` lines below a `build` line that starts with header"""
    i = text.find('\n' + header)
    if i < 0:
        raise SystemExit('make_recipe: no "%s" in build.ninja' % header.strip())
    line_end = text.find('\n', i + 1)
    out = {}
    for ln in text[line_end + 1:].split('\n'):
        if not ln.startswith('  '):
            break
        k, _, v = ln.strip().partition(' = ')
        out[k] = v
    return text[i + 1:line_end], out


def unescape(p):
    return p.replace('$:', ':').replace('$ ', ' ').replace('$$', '$')


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    bdir, stage = sys.argv[1], sys.argv[2]
    text = open(os.path.join(bdir, 'build.ninja'), encoding='utf-8').read()
    cache = open(os.path.join(bdir, 'CMakeCache.txt'), encoding='utf-8').read()
    src = re.search(r'^CMAKE_HOME_DIRECTORY:INTERNAL=(.*)$', cache, re.M).group(1).strip()
    ndk = re.search(r'^ANDROID_NDK:\w+=(.*)$', cache, re.M)
    ndk = (ndk.group(1) if ndk else os.environ['ANDROID_NDK_ROOT']).strip().replace('\\', '/')
    xr = re.search(r'^OPENXR_LOADER_DIR:\w+=(.*)$', cache, re.M).group(1).strip().replace('\\', '/')
    gen_dir = re.search(r'^PS2_GENERATED_DIR:\w+=(.*)$', cache, re.M).group(1).strip()

    def tmpl(s):
        s = s.replace('\\', '/')
        for a, b in ((src, '{src}'), (bdir.replace('\\', '/'), '{build}'), (ndk, '{ndk}'), (xr, '{openxr}')):
            s = s.replace(a, b)
        return s

    gen_line, gen_vars = ninja_vars(text, 'build CMakeFiles/ac5.dir/%s/ps2_code_0000.c.o:' % gen_dir)
    vu_line, vu_vars = ninja_vars(text, 'build CMakeFiles/ac5.dir/runtime/src/ps2_vu.c.o:')
    link_line, link_vars = ninja_vars(text, 'build libmain.so:')
    rules = open(os.path.join(bdir, 'CMakeFiles', 'rules.ninja'), encoding='utf-8').read()
    cc = re.search(r'rule C_COMPILER__ac5_unscanned_Release\n(?:  .*\n)*?  command = (.*)\n', rules).group(1)
    target = re.search(r'--target=(\S+)', cc).group(1)
    ins = link_line.split(':', 1)[1].split(' || ')[0].split(' | ')[0].split()[1:]   # (after the rule's name)
    os.makedirs(os.path.join(stage, 'obj'), exist_ok=True)
    order = []
    n = 0
    strip = os.path.join(ndk, 'toolchains', 'llvm', 'prebuilt', 'windows-x86_64', 'bin', 'llvm-objcopy.exe')
    for p in ins:
        p = unescape(p)
        rel = p.replace('\\', '/')
        if rel.startswith('CMakeFiles/ac5.dir/%s/' % gen_dir):
            order.append('@gen:' + os.path.basename(rel)[:-2])                     # ps2_code_0000.c etc.
            continue
        if rel == 'CMakeFiles/ac5.dir/runtime/src/ps2_vu.c.o':
            order.append('@vu')
            continue
        if not rel.endswith('.o'):
            continue
        n += 1
        name = '%03d_%s' % (n, os.path.basename(rel))
        dst = os.path.join(stage, 'obj', name)
        subprocess.run([strip, '--strip-debug', os.path.join(bdir, p), dst], check=True)
        order.append('obj/' + name)
    shutil.copyfile(os.path.join(bdir, 'liblua54.a'), os.path.join(stage, 'liblua54.a'))
    subprocess.run([strip, '--strip-debug', os.path.join(stage, 'liblua54.a')], check=True)
    libs = tmpl(link_vars['LINK_LIBRARIES']).split()
    recipe = {
        'format': 'ac0oculus-quest-recipe-1',
        'target': target,
        'gen_dir': gen_dir,
        'defines': tmpl(gen_vars['DEFINES']),
        'flags_gen': tmpl(gen_vars['FLAGS']),
        'flags_vu': tmpl(vu_vars['FLAGS']),
        'defines_vu': tmpl(vu_vars['DEFINES']),
        'language_flags': tmpl(link_vars['LANGUAGE_COMPILE_FLAGS']),
        'link_flags': re.sub(r'\s*-Xlinker --dependency-file=\S+', '', tmpl(link_vars['LINK_FLAGS'])),
        'link_libs': ['{openxr_so}' if l.endswith('libopenxr_loader.so') else l for l in libs],
        'objects': order,
        'soname': link_vars.get('SONAME', 'libmain.so'),
    }
    bad = [k for k, v in recipe.items() if isinstance(v, str) and re.search(r'[A-Za-z]:/', v)]
    if bad:
        raise SystemExit('make_recipe: a machine path is left in %s' % ', '.join(bad))
    with open(os.path.join(stage, 'recipe.json'), 'w', encoding='utf-8', newline='\n') as fp:
        json.dump(recipe, fp, indent=1)
    print('make_recipe: %d runtime objects, %d generated places, ps2_vu.c, %s' % (n, sum(o.startswith('@gen:') for o in order), target))
    return 0


if __name__ == '__main__':
    sys.exit(main())
