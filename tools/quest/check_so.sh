#!/bin/bash
# check_so.sh LIB.so [BUNDLED.so ...] -- static checks of one arm64 Android library before it goes into an APK (stage 1,
# 2026-11-07; reused for the game's 60-100 MB libmain.so: only the ELF headers and the dynamic symbol tables are read,
# so it takes well under a second on big files).
# BUNDLED.so: the other libraries shipped in the same APK (check_apk.sh passes them all). The OpenXR loader
# ($OPENXR_LOADER_DIR) always counts as bundled. Checks (exit 0 = all pass, 1 = a check failed, 2 = usage):
#   1. ELF64 AArch64 shared object (ET_DYN), no TEXTREL.
#   2. NEEDED is a subset of the system set {libandroid, liblog, libEGL, libGLESv3, libvulkan, libaaudio, libmediandk, libc,
#      libm, libdl}.so + the sonames of the bundled libraries (libc++_shared.so only when it is bundled); CHECK_SO_ALLOW="a.so
#      b.so" adds names (e.g. libnativewindow.so) on purpose.
#   3. every PT_LOAD alignment is a multiple of 0x4000 (16 KB pages; linked with -Wl,-z,max-page-size=16384).
#   4. every strong undefined dynamic symbol (llvm-nm -D -u, symbol versions dropped) is defined by a NEEDED library:
#      the NDK API-$QUEST_API stubs (sysroot/usr/lib/aarch64-linux-android/$QUEST_API, so symbols newer than minSdk 32
#      fail here) or a bundled library (+ that library's own NEEDED). Unresolved weak symbols are listed, not failed.
. "$(dirname "$0")/common.sh"
so=$1
[ -f "$so" ] || { echo "usage: check_so.sh LIB.so [BUNDLED.so ...]"; exit 2; }
shift
bundled=("$@" "$OPENXR_LOADER_SO")
tmp=$(mktemp -d); trap 'rm -rf "$tmp"' EXIT
fail=0
tag="check_so: $(basename "$so")"
bad() { echo "$tag: FAIL: $*"; fail=1; }
# (a missing input or temp folder must fail, never print empty counts and PASS: seen once on 2026-11-07)
[ -s "$so" ] || { echo "$tag: FAIL: $so is missing or empty"; exit 1; }
[ -n "$tmp" ] && [ -d "$tmp" ] || { echo "$tag: FAIL: no temporary folder ($tmp)"; exit 1; }

# 1. header
hdr=$(llvm-readelf -h "$so" 2>&1 | nocr)
cls=$(echo "$hdr" | awk -F: '/^ *Class:/ {gsub(/ /, "", $2); print $2}')
mach=$(echo "$hdr" | awk -F: '/^ *Machine:/ {gsub(/^ +/, "", $2); print $2}')
typ=$(echo "$hdr" | awk -F: '/^ *Type:/ {gsub(/^ +/, "", $2); print $2}')
[ "$cls" = ELF64 ] && [ "$mach" = AArch64 ] && case $typ in DYN*) true ;; *) false ;; esac \
    || bad "not an arm64 shared object (class '$cls', machine '$mach', type '$typ')"
dyn=$(llvm-readelf -d "$so" 2>&1 | nocr)
soname=$(echo "$dyn" | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
echo "$tag: $(stat -c %s "$so") bytes, $cls $mach ${typ%% *}, soname ${soname:-none}"
echo "$dyn" | grep -q 'TEXTREL' && bad "TEXTREL present (non-PIC code: build with -fPIC)"

# 2. NEEDED
sysset="libandroid.so liblog.so libEGL.so libGLESv3.so libvulkan.so libaaudio.so libmediandk.so libc.so libm.so libdl.so"
declare -A provider=()       # soname -> bundled file
for b in "${bundled[@]}"; do
    [ -f "$b" ] || { bad "bundled library $b does not exist"; continue; }
    sn=$(llvm-readelf -d "$b" 2>/dev/null | nocr | sed -n 's/.*(SONAME).*\[\(.*\)\].*/\1/p')
    provider[${sn:-$(basename "$b")}]=$b
done
needed=$(echo "$dyn" | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p')
nok=
for n in $needed; do
    if [ -n "${provider[$n]}" ]; then nok="$nok $n(bundled)"
    elif [[ " $sysset $CHECK_SO_ALLOW " == *" $n "* ]]; then nok="$nok $n"
    else bad "NEEDED $n is neither a system library of the allowed set nor bundled"; fi
done
echo "$tag: NEEDED$nok"

# 3. LOAD alignment
loads=$(llvm-readelf -lW "$so" 2>/dev/null | nocr | awk '$1 == "LOAD" {print $NF}')
nl=0; nbad=0
for a in $loads; do
    nl=$((nl + 1))
    [ $(( a % 0x4000 )) -eq 0 ] || nbad=$((nbad + 1))
done
[ $nl -gt 0 ] || bad "no PT_LOAD segment"
[ $nbad = 0 ] || bad "$nbad of $nl PT_LOAD alignments are not a multiple of 0x4000 (link with -Wl,-z,max-page-size=16384)"
echo "$tag: $nl LOAD segments, alignment $(echo $loads | tr ' ' '\n' | sort -u | tr '\n' ' ')"

# 4. undefined symbols against the NEEDED libraries (stubs or bundled ones, plus a bundled library's own NEEDED)
llvm-nm -D -u "$so" 2>/dev/null | nocr | awk '{s = $2; sub(/@.*/, "", s); print $1, s}' > "$tmp/undef"
awk '$1 == "U" {print $2}' "$tmp/undef" | sort -u > "$tmp/strong"
awk '$1 != "U" {print $2}' "$tmp/undef" | sort -u > "$tmp/weak"
: > "$tmp/defs"
resolved_from=
declare -A seen=()
add_defs() {     # $1 soname
    local f
    [ -n "${seen[$1]}" ] && return
    seen[$1]=1
    if [ -n "${provider[$1]}" ]; then f=${provider[$1]}
    elif [ -f "$NDK_STUBS/$1" ]; then f=$NDK_STUBS/$1
    else bad "no API-$QUEST_API stub or bundled file for NEEDED $1"; return; fi
    llvm-nm -D --defined-only "$f" 2>/dev/null | nocr | awk '{s = $NF; sub(/@.*/, "", s); print s}' >> "$tmp/defs"
    resolved_from="$resolved_from $1"
}
for n in $needed; do
    add_defs "$n"
    if [ -n "${provider[$n]}" ]; then    # a bundled library's own NEEDED are in the same local group
        for m in $(llvm-readelf -d "${provider[$n]}" 2>/dev/null | nocr | sed -n 's/.*(NEEDED).*\[\(.*\)\].*/\1/p'); do
            add_defs "$m"
        done
    fi
done
sort -u "$tmp/defs" -o "$tmp/defs"
comm -23 "$tmp/strong" "$tmp/defs" > "$tmp/unres"
comm -23 "$tmp/weak" "$tmp/defs" > "$tmp/wunres"
nu=$(wc -l < "$tmp/unres"); nw=$(wc -l < "$tmp/wunres")
[ -s "$tmp/strong" ] && [ -s "$tmp/defs" ] && [ -n "$nu" ] && [ -n "$nw" ] || bad "the symbol lists could not be made ($tmp)"
nu=${nu:-0}; nw=${nw:-0}
echo "$tag: $(wc -l < "$tmp/strong") undefined (+ $(wc -l < "$tmp/weak") weak), checked against$resolved_from (API $QUEST_API stubs): $nu unresolved, $nw weak unresolved"
if [ "$nu" -gt 0 ]; then
    sed 's/^/    unresolved: /' "$tmp/unres" | head -50
    [ "$nu" -gt 50 ] && echo "    ... $((nu - 50)) more"
    bad "$nu unresolved undefined symbols (a missing library, or a symbol newer than API $QUEST_API)"
fi
[ "$nw" -gt 0 ] && sed 's/^/    weak unresolved (null at run time): /' "$tmp/wunres" | head -20
if [ $fail = 0 ]; then echo "$tag: PASS"; else echo "$tag: FAILED"; fi
exit $fail
