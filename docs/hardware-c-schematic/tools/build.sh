#!/bin/zsh
# Regenerate the KiCad project, check it against the original netlist, export the PDF bundle.
# usage: tools/build.sh   (run from anywhere; needs KiCad 10 and python3; the PDF bundle step needs macOS swift)
set -e
T=${0:A:h}; D=${T:h}; OUT=$D/kicad
KICLI=${KICLI:-/Applications/KiCad/KiCad.app/Contents/MacOS/kicad-cli}
rm -rf $OUT; mkdir -p $OUT
python3 -I $T/build.py $OUT
cd $OUT
$KICLI sch export netlist --format kicadsexpr -o /tmp/hwc_c062.net hwc_c062.kicad_sch >/dev/null 2>&1
python3 -I $T/netcmp.py /tmp/hwc_c062.net | grep -v UNNAMED
$KICLI sch erc --format json --severity-all -o /tmp/hwc_c062_erc.json hwc_c062.kicad_sch 2>&1 | grep -i violation
$KICLI sch export pdf -o /tmp/hwc_c062.pdf hwc_c062.kicad_sch >/dev/null 2>&1
rm -f $OUT/*.kicad_prl
swift $T/assemble.swift $D/Hardware-C-schematic-redrawn.pdf /tmp/hwc_c062.pdf \
  $T/data/page12-rev2.svg "$D/../../apm按键采集图.pdf"
