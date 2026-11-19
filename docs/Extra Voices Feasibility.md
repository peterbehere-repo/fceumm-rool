# More than five channels, and audio that does not overwrite itself

Feasibility study for the `fceumm-rool` fork, 2026-11-19.

## Short answer

Three separate things are being asked for, and they have very different
answers.

1. **Support for more than five sound channels.** Already done, upstream,
   and working in this fork. FCEUmm emulates all six Famicom expansion
   audio chips, from three extra channels up to eight. They activate
   automatically for the games that use them. I verified the path end to
   end here with a VRC6 test ROM.
2. **Stopping a game from overwriting a channel's audio** (a new note or
   sound effect cutting off one that is still playing). Feasible, and I
   built a working spike: it keeps the old note alive in a spare voice
   while the new one plays. It is an enhancement, not an emulation
   accuracy fix, and it is off by default.
3. **Giving an existing game's music more voices, so it can play parts it
   currently has to drop.** Not possible from the emulator alone. The
   game's sound engine decides which registers get written; an emulator
   cannot invent the missing harmony line. That needs the game's music
   code rewritten (a ROM hack), or new music written for expansion
   hardware.

## What the hardware actually has

The stock APU has five channels and no more. A cartridge can add its
own audio chip, wired between the APU and the TV, but only on a
Famicom: Nintendo removed the cartridge audio pin on the NES, so
expansion audio never worked on US or European consoles.

| Chip | Adds | Total | Used by |
|---|---|---|---|
| — (stock APU) | 2 pulse, triangle, noise, DMC | 5 | everything else |
| FDS (2C33) | 1 wavetable | 6 | disk system titles |
| MMC5 | 2 pulse, 1 PCM | 8 | a handful of Japanese releases |
| Konami VRC6 | 2 pulse, 1 sawtooth | 8 | 3 games (Castlevania 3 JP) |
| Sunsoft 5B (YM2149F) | 3 square | 8 | Gimmick (JP) |
| Konami VRC7 (OPLL) | 6 FM | 11 | Lagrange Point |
| Namco 163 | 1 to 8 wavetable | 6 to 13 | 10 games (Megami Tensei II) |

Sources: [NESdev APU](https://www.nesdev.org/wiki/APU),
[NESdev list of games with expansion audio](https://www.nesdev.org/wiki/List_of_games_with_expansion_audio),
and jsgroth's [Famicom Expansion Audio](https://jsgroth.dev/blog/posts/famicom-expansion-audio/)
write-up, which is the clearest description of each chip I found.

## What this fork already does

All six chips are implemented in `src/`:

| Chip | File |
|---|---|
| FDS | `fds_apu.c` |
| MMC5 | `boards/mmc5.c` |
| VRC6 | `boards/vrc6.c` |
| VRC7 | `boards/vrc7.c` |
| Namco 163 | `boards/n106.c` (all 8 channels) |
| Sunsoft 5B | `boards/69.c` |

Each also has a volume control in the core options (`fceumm_apu_vrc6`,
`fceumm_apu_n163` and so on), which matters because expansion audio is
loud relative to the APU and real cartridges differed in how loudly they
mixed it.

**Verified here, not assumed.** I generated a mapper 24 ROM that plays
one stock APU pulse plus the VRC6's two pulses and sawtooth. Muting the
VRC6 option changes the audio hash and leaves the video hash untouched:

```text
VRC6 at 100%   audio 465a4b09   video 15b9d875
VRC6 muted     audio 01347375   video 15b9d875
```

The ROM is `Rool VRC6 Test.nes` in `generated/fceumm-test/`, generated
by script, with no copyrighted content. That is the whole "more than
five channels" question answered: the capability is there, and it costs
nothing to ship.

## The interesting half: channels overwriting each other

A pulse channel plays one note at a time. Write a new pitch to `$4003`
and whatever was sounding stops. That is how the hardware works, and it
is why NES music is written the way it is: two pulse lines shared
between melody, harmony, bass and percussion, with a sound effect
stealing a channel and silencing the music underneath it.

I instrumented the APU to measure how often this happens in practice,
in The Goonies II over 60 seconds of gameplay:

| Channel | Note starts | Same pitch (articulation) | Different pitch (real steal) |
|---|---|---|---|
| Pulse 1 | 87 | 55 | 31 |
| Pulse 2 | 46 | 45 | 0 |

The distinction matters. Most retriggers are the engine restriking the
same note to shape it — a staccato or a drum. Only 31 per minute are
genuine steals where a different note is cut off. Duplicating the
same-pitch restrikes would turn every staccato into a flam, so a useful
implementation has to tell the two apart.

## Route A: extra voices in the emulator (spike built)

I implemented the mechanism to prove it works, behind a core option
(`Extra Voices`, off by default).

- On a retrigger at a **different pitch** while the channel is still
  sounding, the note being killed is copied into a spare voice: pitch,
  duty and phase.
- The spare voice is mixed into the same linear field the expansion
  chips already use, at an amplitude matched to a full-volume APU pulse.
- It fades out over 18 frames, so the two notes overlap instead of the
  second erasing the first.
- Nothing runs while the option is off.

Results, Goonies II, 1400 frames:

| | Stock | Extra voices |
|---|---|---|
| Audio hash | `7adfa551` | `148a0afd` |
| Video hash | `28109d42` | `28109d42` |
| Peak sample | 13197 | 13712 |
| Clipped samples | 0 | 0 |
| CPU, 2000 frames | 0.689 s | 0.681 s |

The difference signal is roughly 20% of the signal RMS, in bursts, and
the plot in `generated/review/Extra Voices Comparison.png` shows why:
where the stock output falls silent between phrases, the carried-over
notes keep sounding underneath the new ones.

The seven-case baseline still passes with the option off, and a
savestate taken mid-run resumes on an identical frame.

### Limits of this route

- **It cannot add music.** It sustains notes that were being cut. It
  never produces a note the game did not play.
- The spike has no per-voice envelope, sweep or length counter, so a
  carried-over note holds its capture volume and then fades. Real
  envelopes would need to run per voice, which is the bulk of the work
  in a proper implementation.
- Only the high-quality mixer path is covered; the default Low quality
  path would need the same treatment.
- Game state: the voice pool is not in the savestate yet, so a state
  load cuts the tails (video is unaffected).
- Loudness and phase: two identical pitches overlapping can sound
  chorusy or flange; overlapping tails raise peak level, and with
  expansion audio also playing there is a risk of clipping in the mix.
- Some games silence a channel by zeroing its volume before the next
  note. A tail captured before that would bleed through.

### Effort

The mechanism is about 150 lines and took an afternoon including
testing. A shippable version — per-voice envelope and length counter,
both mixer paths, savestates, option wording, a test case per game —
is realistically **2 to 4 days** of careful work, mostly spent on
making it sound good rather than making it run.

## Route B: real extra channels in a game

This is the only way to get genuinely more music, and it is game work,
not emulator work.

- **Retail games.** The game's music driver has to be rewritten to talk
  to a chip's registers, the music data re-ported, and the ROM rebuilt
  on a mapper that has the chip. This is a substantial reverse
  engineering project per game. People do it — the Castlevania 3
  Japanese version is the same game with a VRC6 soundtrack, and there
  are VRC6 conversions of other soundtracks circulating — but it is
  weeks of work by someone fluent in 6502, per game.
- **Homebrew and new music.** Very tractable. FamiStudio composes for
  VRC6, VRC7, N163, 5B, MMC5, FDS and EPSM, and its sound engine
  (FamiTone2) plays all of them on real hardware and in emulators.
  A new game or a rewritten soundtrack for an existing one gets the
  extra channels with no emulator changes at all.

## Route C: the one gap in expansion support

FCEUmm covers the six chips that existed in the 1980s. It does not
cover **EPSM**, a modern expansion board (YM2608-based, used by
FamiStudio and recent homebrew) that runs through a mapper 4 variant.
If the goal is "support modern homebrew too", that is a self-contained
addition in the same shape as the existing chip modules — a few hundred
lines, plus a way to test it, since no commercial ROM uses it.

## Recommendation

1. **Ship nothing to get more channels.** It is already there; the
   useful work is verifying each chip against hardware recordings and
   keeping the per-channel volume options honest.
2. **Treat "extra voices" as an audio enhancement, not an accuracy fix.**
   It changes how games sound by design, so it belongs behind an option
   that is off by default, with the existing baseline proving
   bit-identical output when off. Worth building if the overlap is
   something you actually enjoy hearing; it is not what the hardware
   did.
3. **If the goal is a specific game sounding fuller, that is a ROM hack.**
   Route B, not the emulator. The emulator side of that already works.
4. **Consider EPSM** if modern homebrew matters.

## Reproducing any of this

```sh
cd "/rool-drive/Retro Journey/fceumm-rool"
make -f Makefile.libretro -j2
cc -std=c89 -pedantic -Wall -O2 -rdynamic \
   -Isrc/drivers/libretro/libretro-common/include tests/play_rom.c -o play_rom -ldl

# expansion audio present?
./play_rom ./fceumm_libretro.so "../generated/fceumm-test/Rool VRC6 Test.nes" \
   --frames 240 fceumm_apu_vrc6=100

# extra voices, high quality mixer
./play_rom ./fceumm_libretro.so "../ROMs/Goonies II, The (USA).nes" --frames 1400 \
   --input "420:start,560:start,700:start,900-980:right,1000:a,1030-1100:left,1120-1140:up" \
   fceumm_sndquality=High fceumm_extra_voices=3 --wav voices.wav

# nothing regressed
tests/rool_baseline.sh ./fceumm_libretro.so
```
