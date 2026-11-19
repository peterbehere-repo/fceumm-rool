# Test rig

Three small tools for showing that a change to the core did not alter
what the emulated machine does, and for driving a game past its title
screen so a comparison covers real gameplay.

Nothing here is built by `Makefile.libretro`; the harnesses are plain C89
and load the core as a shared library.

## Building the harness

```sh
make -f Makefile.libretro -j2
cc -std=c89 -pedantic -Wall -O2 -rdynamic \
   -Isrc/drivers/libretro/libretro-common/include \
   tests/play_rom.c -o play_rom -ldl
```

## play_rom

Plays a content file for a fixed number of frames with no frontend
around it and prints one summary line:

```sh
./play_rom ./fceumm_libretro.so game.nes --frames 600
# frames 600 drawn 600 duped 356 null 0 video 19e6864e audio 432153e5 samples 479178
```

The video hash covers every visible pixel of every frame together with
the pitch, so a change in image or in buffer layout shows up. `duped`
counts frames identical to the one before, which tells you whether the
run is sitting on a static screen. `--time` hashes nothing and reports
CPU time instead, because hashing work does not belong in a run being
measured.

| Option | Effect |
|---|---|
| `--frames N` | how many frames to run (default 600) |
| `--input SCRIPT` | scripted pad input, see below |
| `--shot FRAME:PATH` | write that frame as a binary PPM |
| `--save FRAME:PATH` | write a savestate at that frame |
| `--load PATH` | restore a savestate before the first frame |
| `--offset N` | label the run's frames from N, for resuming |
| `--sysdir DIR` | where a BIOS (FDS `disksys.rom`) is looked for |
| `--verbose` | show the core's own log output |
| `key=value` | set a core option |

### Driving the pad

```sh
./play_rom ./fceumm_libretro.so game.nes --frames 1400 \
   --input "420:start, 560:start, 700:start, 900-980:right, 1000:a"
```

Steps are separated by commas or semicolons. `N` is a single frame,
`A-B` a range with both ends included, `A+` everything from A onwards.
Buttons joined by `+` are held together; names are `a b x y l r l2 r2
select start up down left right`. A step lists every button it wants
down, so steps are absolute rather than cumulative.

### Snapshots and states

`--shot` converts the core's output to a plain PPM, so a run can be
looked at with any image tool (`magick frame.ppm frame.png`).

`--save` and `--load` use the core's own savestates. Resuming from a
state and continuing must land on exactly the same frame as a continuous
run; that is worth checking whenever a change touches serialised state:

```sh
./play_rom core.so game.nes --frames 1000 --input "$IN" --save 1000:mid.state
./play_rom core.so game.nes --frames 400 --offset 1001 --input "$IN" \
   --load mid.state --shot 1399:resumed.ppm
./play_rom core.so game.nes --frames 1400 --input "$IN" --shot 1399:direct.ppm
cmp resumed.ppm direct.ppm    # must be byte-identical
```

The offset is needed because the script is written in absolute frames:
after restoring a state taken after frame 1000, the next frame to run is
1001, not 0.

## rool_ab.sh

Plays the same content through two builds under several option sets and
reports any difference in frames, video hash, audio hash or sample count.

```sh
make -f Makefile.libretro -j2 && cp fceumm_libretro.so /tmp/new.so
git stash && make -f Makefile.libretro -j2 && git stash pop
INPUT="420:start,560:start,700:start" tests/rool_ab.sh fceumm_libretro.so /tmp/new.so game.nes
```

It exits non-zero if anything differs. Set `INPUT` when comparing a game
that sits on a title screen without input.

## rool_baseline.sh

Replays recorded cases from `tests/baseline.txt` and compares them with
what the build under test produces.

```sh
tests/rool_baseline.sh ./fceumm_libretro.so        # check
tests/rool_baseline.sh --record ./fceumm_libretro.so   # rewrite recordings
```

The cases come from `tests/baseline-cases.txt`; `--record` on a build you
trust writes their summary lines into `tests/baseline.txt`. Fields are
separated by `|`, because a shell treats a tab as whitespace and drops
empty fields.

Recording against a build that is already broken records the breakage,
so always record from a build known to be good, and read the diff of
`baseline.txt` before committing it.

## Content

No ROMs are stored in this repository. The cases resolve their content
from these paths, which can be overridden with environment variables:

| Key | Default path | Override |
|---|---|---|
| `smoke` | `/rool-drive/generated/fceumm-test/Rool Smoke Test.nes` | `ROOL_SMOKE_ROM` |
| `goonies2` | `/rool-drive/Retro Journey/ROMs/Goonies II, The (USA).nes` | `ROOL_ROM_DIR` |
| `goonies2-ines1` | same folder, `[iNES 1.0 header]` variant | `ROOL_ROM_DIR` |

A case whose content is missing is reported and skipped, so the rig
still works on a machine without the ROMs.

### The Goonies II pair

`The Goonies II` is a UxROM (mapper 2) game with 8 KB of CHR RAM, and it
is the reason the two variants exist:

- `Goonies II, The (USA).nes` — NES 2.0 header, mapper 2 submapper 2,
  which tells the core to emulate **bus conflicts**.
- `Goonies II, The (USA) [iNES 1.0 header].nes` — the same PRG with a
  plain iNES header, which cannot express the submapper, so the core runs
  it without bus conflicts.

With a standard iNES header the second file hashes to No-Intro's
published dump (`aa9ca482` / `d38325cffb9ba2e6f57897c0e9564cc0` /
`0a5b8fd5e46f56203f8bf12e888f4f2ea1616aa8`), so the program data is the
verified dump and only the header was rewritten.

Both variants produce identical output through FCEUmm today, which is the
expected result: Konami's code does not rely on bus conflict behaviour.
That makes the pair a regression guard — work on the latch may change
either variant, but it must not make them disagree.
