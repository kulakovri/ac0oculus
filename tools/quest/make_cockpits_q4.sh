#!/bin/bash
# make_cockpits_q4.sh SRC_DIR OUT_DIR [NAME ...] -- the Quest's compressed cockpit set, cockpits_q4 (rq3_tex 2026-11-11,
# PROMPT_2026-11-11_night_cockpits N1.2, BUGS_RQ R-65, NOTES_RQ "rq3_tex"). Each model of the Quest set (ps2_android.c
# defaults(): the 35 aircraft files, the default f5e_t38 and hands.cpit, the gloves every model without its own takes)
# from SRC_DIR (format 3, e.g. aczrecomp/cockpits) through build/tools/cpit_pack.exe (tools/cockpit/build_cpit_pack.sh)
# into OUT_DIR as format 4:
#   - every texture stored from the finest mip level the eye box can sample (--need, the Quest 3 eye at scale 1.0: 758 px
#     per tangent; the head within LEAN m of the design eye; MARGIN x the image centre's density, i.e. exact to 45 deg off the
#     eye's axis: cpit_view's audit and head-box views never read a level the set left out) and at most CAP x CAP texels
#     (by area: the F-2A's 100 x 10346 PFD tape stays whole);
#   - BC1 where its top level holds BC1_DB dB, at most BC1_P8 % of texels off by more than 8 of 255 and BC1_P4 % by more
#     than 4, else BC7 (text,
#     dials, glyphs, glows, alpha cut-outs); RGBA8 for the tiny ones, for a texture up to 1024^2 whose BC7 stays under
#     44 dB, and for one up to 512^2 under 46 dB or with over 0.5 % of its texels off by more than 8 (text, dials, drums); the stored mips go to the GPU as they are (ACZ_VR_COCKPIT_BC, no blits on the device);
#   - the meshes indexed, bit-identical to the source's (the packer reads every file back and compares), so the side
#     files of ACZ_VR_COCKPIT_LOD_DIR (cockpits_ql*) fit the new files as they fit the old ones.
# A model whose pack fails (the read-back check, a missing source) is left out of OUT_DIR: on the device the runtime then
# reads that one from cockpits/ (RGBA8). OUT_DIR/reports/NAME.{txt,tsv}: the packer's summary and per-texture table;
# OUT_DIR/MANIFEST.tsv: name, bytes, sha256, source bytes, GPU texture MB before / after, seconds.
# Env: NEED (758), LEAN (0.25), MARGIN (2.0), CAP (4096), BC1_DB (46), BC1_P8 (0.05), BC1_P4 (0.5), UBER (4), JOBS (2 models at a time).
# Each pack uses 8 threads; HEAVY names a command to run each one under (e.g. nice). Private assets: OUT_DIR is never committed.
# Push (README, "Install"): MSYS_NO_PATHCONV=1 adb push OUT_DIR/. /sdcard/ac0oculus/cockpits_q4/
#   (reports/ and MANIFEST.tsv are small; the APK's defaults() takes cockpits_q4 when the folder exists).
set -e
SRC=${1:?usage: make_cockpits_q4.sh SRC_DIR OUT_DIR [NAME ...]}; OUT=${2:?usage: make_cockpits_q4.sh SRC_DIR OUT_DIR [NAME ...]}; shift 2
T=$(cd "$(dirname "$0")/../.." && pwd)
PK=$T/build/tools/cpit_pack.exe
HEAVY=${HEAVY-}   # (HEAVY: a command each pack runs under; empty: none)
[ -x "$PK" ] || { echo "no $PK: tools/cockpit/build_cpit_pack.sh"; exit 2; }
NEED=${NEED:-758}; LEAN=${LEAN:-0.25}; MARGIN=${MARGIN:-2.0}; CAP=${CAP:-4096}; BC1_DB=${BC1_DB:-46}; BC1_P8=${BC1_P8:-0.05}; BC1_P4=${BC1_P4:-0.5}; UBER=${UBER:-4}
ALL="f5e_t38 f15c f16c a10a mig21bis j35j su27 f14a m2000d mig29a f20a tornado typhoon fa18c f4e ea6b gripen f22a yf23 su47
     f15e f15smtd su37 f1 f5e fa18e f117 f35a adfx01 f16xl adf01 x02s mig31 rafale_m f2a su34 hands"
names=${*:-$ALL}
mkdir -p "$OUT/reports"
one() {
    local m=$1 t0 rc
    t0=$(date +%s)
    [ -f "$SRC/$m.cpit" ] || { echo "$m: no $SRC/$m.cpit" > "$OUT/reports/$m.txt"; return; }
    rm -f "$OUT/$m.cpit"
    set +e
    $HEAVY "$PK" "$SRC/$m.cpit" "$OUT/$m.cpit" --format 4 --need "$NEED" --need-lean "$LEAN" --need-margin "$MARGIN" --cap "$CAP" \
        --bc1-auto "$BC1_DB,$BC1_P8,$BC1_P4" --raw-below 44 --raw-max-bytes 4194304 --raw-text 46,0.5,1048576 --uber "$UBER" --threads 8 \
        --report "$OUT/reports/$m.tsv" > "$OUT/reports/$m.txt" 2>&1
    rc=$?
    set -e
    if [ $rc != 0 ]; then echo "FAILED (exit $rc): left out" >> "$OUT/reports/$m.txt"; rm -f "$OUT/$m.cpit" "$OUT/$m.cpit.tmp"; fi
    echo "seconds $(( $(date +%s) - t0 ))" >> "$OUT/reports/$m.txt"
}
running=0
for m in $names; do
    one "$m" &
    running=$((running + 1))
    if [ $running -ge "${JOBS:-2}" ]; then wait -n; running=$((running - 1)); fi
done
wait
{   printf "name\tbytes\tsha256\tsrc_bytes\ttex_mb_before\ttex_mb_after\tseconds\tstatus\n"
    for m in $names; do
        r=$OUT/reports/$m.txt
        vb=$(sed -n 's/.*texture VRAM \([0-9.]*\) -> \([0-9.]*\) MB.*/\1/p' "$r" 2>/dev/null | head -1)
        va=$(sed -n 's/.*texture VRAM \([0-9.]*\) -> \([0-9.]*\) MB.*/\2/p' "$r" 2>/dev/null | head -1)
        s=$(sed -n 's/^seconds //p' "$r" 2>/dev/null | tail -1)
        if [ -f "$OUT/$m.cpit" ]; then
            printf "%s\t%s\t%s\t%s\t%s\t%s\t%s\tok\n" "$m" "$(stat -c %s "$OUT/$m.cpit")" "$(sha256sum "$OUT/$m.cpit" | cut -c1-64)" \
                "$(stat -c %s "$SRC/$m.cpit" 2>/dev/null)" "$vb" "$va" "$s"
        else
            printf "%s\t-\t-\t%s\t-\t-\t%s\tleft out\n" "$m" "$(stat -c %s "$SRC/$m.cpit" 2>/dev/null)" "$s"
        fi
    done
} > "$OUT/MANIFEST.tsv"
echo "set in $OUT: $(grep -c $'\tok$' "$OUT/MANIFEST.tsv") ok, $(grep -c 'left out$' "$OUT/MANIFEST.tsv") left out; $(du -sm "$OUT" | cut -f1) MB"
