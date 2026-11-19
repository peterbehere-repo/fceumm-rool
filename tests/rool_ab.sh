#!/bin/sh
# Compare two builds of the core on the same content.
#
# Usage: tests/rool_ab.sh coreA.so coreB.so [-n FRAMES] content [content ...]
#
# Each content file is played through both cores under several option
# sets, and the frame count, video hash, audio hash and sample total of
# the two runs are compared. Any difference is printed and the exit
# status is 1. Set FRAMES in the environment to change the default 600.
#
#   make -f Makefile.libretro -j2 && cp fceumm_libretro.so /tmp/new.so
#   git stash && make -f Makefile.libretro -j2 && git stash pop
#   tests/rool_ab.sh fceumm_libretro.so /tmp/new.so content.nes

set -u

if [ $# -lt 3 ]; then
   echo "usage: $0 coreA.so coreB.so [-n FRAMES] content [content ...]" >&2
   exit 2
fi

a=$1
b=$2
shift 2

frames=${FRAMES:-600}
if [ "${1:-}" = "-n" ]; then
   frames=$2
   shift 2
fi

here=$(dirname "$0")
play="$here/../play_rom"
if [ ! -x "$play" ]; then
   echo "build the harness first:" >&2
   echo "  cc -std=c89 -pedantic -Wall -O2 -rdynamic \\" >&2
   echo "     -Isrc/drivers/libretro/libretro-common/include \\" >&2
   echo "     tests/play_rom.c -o play_rom -ldl" >&2
   exit 2
fi

# Option sets that reach different paths in the core.
sets="fceumm_region=Auto
fceumm_region=PAL
fceumm_ntsc_filter=composite
fceumm_sndquality=High
fceumm_show_adv_options=enabled fceumm_sndstereodelay=15_ms_delay"

diffs=$(mktemp)
trap 'rm -f "$diffs"' EXIT
status=0
for rom in "$@"; do
   echo "== $(basename "$rom")"
   echo "$sets" | while IFS= read -r opts; do
      line_a=$("$play" "$a" "$rom" --frames "$frames" $opts 2>/dev/null | tail -1)
      line_b=$("$play" "$b" "$rom" --frames "$frames" $opts 2>/dev/null | tail -1)
      if [ "$line_a" = "$line_b" ] && [ -n "$line_a" ]; then
         printf '  ok    %s\n' "${opts:-default options}"
      else
         printf '  DIFF  %s\n         A: %s\n         B: %s\n' \
            "${opts:-default options}" "$line_a" "$line_b"
         echo x >> "$diffs"
      fi
   done
done

if [ -s "$diffs" ]; then
   status=1
fi
rm -f "$diffs"
exit $status
