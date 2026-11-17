#!/bin/bash
# make_release_quest.sh VERSION SRC OUT -- the Quest installer (ac0oculus, 2026-11-17), on the developer's PC: OUT/stage (what the setup
# installs), OUT/ac0oculus-VERSION-setup.exe (Inno Setup 6, tools/setup/quest/ac0oculus.iss), OUT/SHA256SUMS.
#   SRC   the PUBLIC tree to build (a clone of ac0oculus, so the public build's defaults are the ones compiled in) with the developer's
#         own gen_zero/ and game/SLUS_213.46 in it: they are only used to link a reference libmain.so and to scan the stage for the
#         game's bytes -- nothing made from them goes into the stage.
#   OUT   a folder outside every repository
# Steps: the Quest CMake build of SRC (PS2_STRIP_PATHS=ON: no folder of this PC in the objects; ACZ_VERSION=VERSION); the shader guard;
# make_apk.sh for the APK's prebuilt parts (base.apk with the manifest and the shaders, the stripped OpenXR loader) and a reference APK;
# make_recipe.py (the runtime's objects in link order, recipe.json); the stage (the Python embeddable, the recompilers, the player's
# build and push scripts, headers, the glyph textures, licences, Google's licence text for the accept page); tools/setup/release_scan.py
# with the executable (nothing of the game, no machine path); ISCC.
# Env: QUEST_ENV (the developer's toolchain, tools/quest/env.example.sh), ACZ_PY_EMBED_ZIP (python-3.12.x-embed-amd64.zip),
#   ACZ_ANDROID_LICENSE (Google's android-sdk-license text, from repository2-3.xml; fetched when unset), ACZ_ISCC (ISCC.exe), HEAVY
#   (a command the build runs under), JOBS (8).
set -e
VERSION=${1:?usage: make_release_quest.sh VERSION SRC OUT}; SRC=$(cd "${2:?SRC}" && pwd); OUT=${3:?OUT}
HERE=$(cd "$(dirname "$0")" && pwd)
TREE=$(cd "$HERE/../../.." && pwd)
mkdir -p "$OUT"; OUT=$(cd "$OUT" && pwd)
. "$TREE/tools/quest/common.sh"
export PATH=$PATH:/c/msys64/ucrt64/bin
[ -f "$SRC/gen_zero/ps2_func_table.c" ] || { echo "make_release_quest: $SRC has no gen_zero (recompile your disc there first)"; exit 2; }
[ -f "$SRC/game/SLUS_213.46" ] || { echo "make_release_quest: $SRC has no game/SLUS_213.46 (the scan needs it)"; exit 2; }
: "${ACZ_PY_EMBED_ZIP:?python-3.12.x-embed-amd64.zip}"
GIT=$(git -C "$SRC" rev-parse --short HEAD 2>/dev/null || echo unknown)
B=$OUT/cmake; ST=$OUT/stage
t0=$(date +%s)

# 1. the build (the same configuration as tools/quest/build_game.sh, plus no machine paths and the release's version)
if [ ! -f "$B/build.ninja" ]; then
    cmake -S "$SRC" -B "$B" -G Ninja -DPS2_QUEST=ON -DCMAKE_BUILD_TYPE=Release -DPS2_GENERATED_DIR=gen_zero -DPS2_STRIP_PATHS=ON \
        -DACZ_VERSION="$VERSION" -DACZ_VERSION_GIT="$GIT" > "$OUT/cfg.log" 2>&1 || { tail -20 "$OUT/cfg.log"; exit 1; }
fi
$HEAVY ninja -C "$B" -j "${JOBS:-8}" > "$OUT/build.log" 2>&1 || { grep -E 'error|FAILED' "$OUT/build.log" | head -20; exit 1; }
python -I "$SRC/tools/quest/rq_probe/spv_guard.py" "$B/shaders" "$SRC/tools/quest/rq_probe/spv_guard_allow.txt" > "$OUT/spv_guard.log" 2>&1 \
    || { tail -5 "$OUT/spv_guard.log"; echo "make_release_quest: the shader guard failed"; exit 1; }
t1=$(date +%s)

# 2. the APK's prebuilt parts, through the public tree's own make_apk.sh (its defaults: com.kulakovri.ac0oculus, label ac0oculus)
rm -rf "$OUT/apk_assets" "$OUT/java"; mkdir -p "$OUT/apk_assets/shaders" "$OUT/java/classes" "$OUT/java/dex"
cp "$B/shaders/"*.spv "$OUT/apk_assets/shaders/"
cp "$SRC/tools/quest/rq_pipelines.keys" "$OUT/apk_assets/shaders/"
AJAR=$(cygpath -m "$ANDROID_SDK_ROOT/platforms/android-34/android.jar")
mapfile -t jsrc < <(find "$SRC/runtime/src/android/java" -name '*.java' | sort)
javac --release 11 -nowarn -cp "$AJAR" -d "$(cygpath -m "$OUT/java/classes")" "${jsrc[@]}"
mapfile -t jcls < <(find "$OUT/java/classes" -name '*.class' | sort)
java -cp "$(cygpath -m "$ANDROID_BUILD_TOOLS/lib/d8.jar")" com.android.tools.r8.D8 --release --min-api 32 --lib "$AJAR" \
    --output "$(cygpath -m "$OUT/java/dex")" "${jcls[@]}"
rm -f "$OUT/ref_key.jks"
(cd "$SRC" && QUEST_KEYSTORE="$OUT/ref_key.jks" DEX="$OUT/java/dex/classes.dex" ACTIVITY=com.kulakovri.aczquest.PadActivity \
    ASSETS="$OUT/apk_assets" GFX=vulkan VERSION_NAME="$VERSION" OUT="$OUT/reference/ac0oculus_reference.apk" \
    tools/quest/make_apk.sh aczquest "$B/libmain.so" "$OPENXR_LOADER_SO") > "$OUT/make_apk.log" 2>&1 \
    || { tail -20 "$OUT/make_apk.log"; exit 1; }
W=$OUT/reference/ac0oculus_reference.work
pkg=$(grep -o 'package="[^"]*"' "$W/AndroidManifest.xml" | head -1 | cut -d'"' -f2)

# 3. the stage
rm -rf "$ST"; mkdir -p "$ST/quest/apk/lib" "$ST/quest/runtime/src" "$ST/quest/third_party/sse2neon" "$ST/recomp" "$ST/tools" \
    "$ST/headset_textures" "$ST/licenses" "$ST/python"
python -I "$HERE/make_recipe.py" "$B" "$ST/quest"
cp "$W/base.apk" "$OUT/java/dex/classes.dex" "$ST/quest/apk/"
cp "$W/lib/libopenxr_loader.so" "$ST/quest/apk/lib/"
echo "$pkg" > "$ST/quest/package.txt"
cp -r "$SRC/runtime/include" "$ST/quest/runtime/"
cp "$SRC/runtime/src/ps2_vu.c" "$ST/quest/runtime/src/"
cp "$SRC/third_party/sse2neon/sse2neon.h" "$SRC/third_party/sse2neon/LICENSE" "$ST/quest/third_party/sse2neon/"
cp -r "$SRC/tools/ps2recomp" "$SRC/tools/vurecomp" "$SRC/config_zero" "$ST/recomp/"
cp "$SRC/tools/setup/vu_images_from_elf.py" "$ST/recomp/"
find "$ST/recomp" -name '__pycache__' -prune -exec rm -rf {} +
cp "$HERE/quest_player_build.py" "$HERE/apk_tool.py" "$HERE/push_to_quest.py" "$ST/tools/"
for d in xbox xbox_modern touch touch_modern; do
    [ -d "$SRC/textures/$d" ] && cp -r "$SRC/textures/$d" "$ST/headset_textures/"
done
unzip -q "$ACZ_PY_EMBED_ZIP" -d "$ST/python"
cp "$SRC/LICENSE" "$SRC/NOTICE" "$SRC/THIRD_PARTY_NOTICES.md" "$ST/licenses/"
cp "$OPENXR_LOADER_DIR/META-INF/LICENSE" "$ST/licenses/openxr_loader_LICENSE.txt"
cp "$SRC/third_party/sse2neon/LICENSE" "$ST/licenses/sse2neon_LICENSE.txt"
cp "$SRC/third_party/miniz/LICENSE" "$ST/licenses/miniz_LICENSE.txt"
cp "$SRC/third_party/lua/LICENSE.html" "$ST/licenses/lua_LICENSE.html" 2>/dev/null || true
[ -f "$ST/python/LICENSE.txt" ] && cp "$ST/python/LICENSE.txt" "$ST/licenses/python_LICENSE.txt"
if [ -n "${ACZ_ANDROID_LICENSE:-}" ]; then cp "$ACZ_ANDROID_LICENSE" "$ST/android-sdk-license.txt"
else
    curl -sf -o "$OUT/repository2-3.xml" https://dl.google.com/android/repository/repository2-3.xml
    python -I -c "import re,sys; s=open(sys.argv[1],encoding='utf-8').read(); m=re.search(r'<license id=\"android-sdk-license\" type=\"text\">(.*?)</license>',s,re.S); open(sys.argv[2],'w',encoding='utf-8',newline='\n').write(m.group(1).strip()+'\n')" "$OUT/repository2-3.xml" "$ST/android-sdk-license.txt"
fi
printf '%s (%s)\n' "$VERSION" "$GIT" > "$ST/version.txt"
cat > "$ST/README.txt" <<EOF
ac0oculus $VERSION -- Ace Combat Zero for Meta Quest 3, built on your PC from your own disc.
The Start menu has: "ac0oculus - copy to the Quest" (the cable, developer mode), "ac0oculus - build the APK again" (another disc,
a new NDK), and this folder. build.log holds the last build's lines. The APK here holds code made from your disc: keep it for yourself.
Source, licences and help: https://github.com/kulakovri/ac0oculus
EOF
t2=$(date +%s)

# 4. nothing of the game, no folder of this PC
python -I "$TREE/tools/setup/release_scan.py" "$ST" --elf "$SRC/game/SLUS_213.46"

# 5. the setup exe
iscc=${ACZ_ISCC:-}
for c in "$LOCALAPPDATA/Programs/Inno Setup 6/ISCC.exe" "$PROGRAMFILES/Inno Setup 6/ISCC.exe" "${PROGRAMFILES:-x}(x86)/Inno Setup 6/ISCC.exe"; do
    [ -z "$iscc" ] && [ -f "$c" ] && iscc=$c
done
[ -n "$iscc" ] || { echo "make_release_quest: no Inno Setup 6 (ACZ_ISCC): the stage is ready in $ST"; exit 0; }
MSYS2_ARG_CONV_EXCL='*' "$iscc" /Q "/DVERSION=$VERSION" "/DSTAGE=$(cygpath -wa "$ST")" "/O$(cygpath -wa "$OUT")" "$(cygpath -wa "$HERE/ac0oculus.iss")"
(cd "$OUT" && sha256sum "ac0oculus-$VERSION-setup.exe" > SHA256SUMS && cat SHA256SUMS)
du -sh "$ST" "$OUT/ac0oculus-$VERSION-setup.exe"
echo "make_release_quest: build $((t1 - t0)) s, stage $((t2 - t1)) s, total $(( $(date +%s) - t0 )) s"
