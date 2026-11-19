#!/bin/sh
# Check a build against recorded behaviour of a known-good build.
#
# Usage: tests/rool_baseline.sh [--record] [core.so]
#
# Each case in tests/baseline.txt names a ROM, a frame count, an input
# script and core options, and records the summary line the harness
# printed for it. The six fields are separated by | (not tab: a shell
# treating tab as whitespace collapses empty fields). Running the script again replays every case and
# compares. --record rewrites the file from the build being passed,
# which is how a case is added, or how a deliberate change in emulated
# output is accepted.
#
# The ROMs are not in this repository. They are looked for at the paths
# below, which can be overridden with ROOL_SMOKE_ROM and ROOL_ROM_DIR.
# A case whose ROM is missing is reported and skipped.
#
#   ROOL_ROM_DIR=/path/to/roms tests/rool_baseline.sh ./fceumm_libretro.so

set -u

here=$(dirname "$0")
play="$here/../play_rom"
baseline="$here/baseline.txt"
sep='|'
record=0

if [ ! -x "$play" ]; then
   echo "build the harness first (see the header of tests/play_rom.c)" >&2
   exit 2
fi

core=./fceumm_libretro.so
for arg in "$@"; do
   case "$arg" in
      --record) record=1 ;;
      *)        core=$arg ;;
   esac
done

if [ ! -f "$core" ]; then
   echo "no core at $core" >&2
   exit 2
fi

smoke=${ROOL_SMOKE_ROM:-/rool-drive/generated/fceumm-test/Rool Smoke Test.nes}
romdir=${ROOL_ROM_DIR:-/rool-drive/Retro Journey/ROMs}

rom_for() {
   case "$1" in
      smoke)    printf '%s' "$smoke" ;;
      goonies2) printf '%s' "$romdir/Goonies II, The (USA).nes" ;;
      goonies2-ines1) printf '%s' "$romdir/Goonies II, The (USA) [iNES 1.0 header].nes" ;;
      *)        printf '%s' "$1" ;;
   esac
}

# Play one case and print the harness summary line.
play_case() {
   rom=$1 frames=$2 input=$3 opts=$4
   if [ -n "$input" ]; then
      # shellcheck disable=SC2086
      "$play" "$core" "$rom" --frames "$frames" --input "$input" $opts 2>/dev/null | tail -1
   else
      # shellcheck disable=SC2086
      "$play" "$core" "$rom" --frames "$frames" $opts 2>/dev/null | tail -1
   fi
}

cases="$here/baseline-cases.txt"
if [ "$record" = 1 ] && [ ! -f "$cases" ]; then
   echo "no $cases to record from" >&2
   exit 2
fi
[ "$record" = 1 ] || cases=$baseline

if [ ! -f "$cases" ]; then
   echo "no $cases; run with --record first" >&2
   exit 2
fi

out=$(mktemp)
trap 'rm -f "$out"' EXIT
fail=0

while IFS="$sep" read -r id romkey frames input opts want; do
   case "$id" in ''|\#*) continue ;; esac
   rom=$(rom_for "$romkey")
   if [ ! -f "$rom" ]; then
      printf '  skip  %-18s %s not found\n' "$id" "$rom"
      continue
   fi
   got=$(play_case "$rom" "$frames" "$input" "$opts")
   if [ -z "$got" ]; then
      printf '  FAIL  %-18s no output from the harness\n' "$id"
      fail=1
      continue
   fi
   if [ "$record" = 1 ]; then
      printf '%s|%s|%s|%s|%s|%s\n' "$id" "$romkey" "$frames" "$input" "$opts" "$got" >> "$out"
      printf '  rec   %-18s %s frames\n' "$id" "$frames"
   elif [ "$got" = "$want" ]; then
      printf '  ok    %-18s %s frames\n' "$id" "$frames"
   else
      printf '  DIFF  %-18s %s frames\n      want: %s\n      got:  %s\n' \
         "$id" "$frames" "$want" "$got"
      fail=1
   fi
done < "$cases"

if [ "$record" = 1 ]; then
   mv "$out" "$baseline"
   echo "wrote $baseline"
fi

exit $fail
