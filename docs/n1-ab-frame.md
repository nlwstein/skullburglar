# Same-frame A/B parity (`BOTS_N1_AB`)

**Short version:** n1 can render one frame twice in one process, once with the old code (A) and once with the new code (B),
and compare the two pictures pixel by pixel. Use it to prove that a shader or pass rewrite changes nothing, or to measure
exactly what it changes. It needs no deterministic scene: both renders use the identical frame state, so any moment of
normal play works.

**Why it exists:** two runs of the same build differ from each other by up to 253/255 in a live scene (animation, vehicles,
timing), so a pose tour of two builds cannot show per-pixel parity. The A/B compares inside one frame, so the only
difference left is the code under test.

This page is for developers. Nothing changes for players: with `BOTS_N1_AB` unset there is no extra pass, no extra memory,
no extra shader code and no push-constant range (see [Cost when off](#cost-when-off)).

## Use it

Set these in the game's environment, then play (or script the game with `botsctl`):

| Switch | Meaning |
|---|---|
| `BOTS_N1_AB=<feature>[,<feature>...]` | Turns it on. A renders each feature's alternative (old) path, B the shipping path. `none` is the self-test A against A. |
| `BOTS_N1_AB_EVERY=<n>` | Compare every n-th scene frame (default 0: only on request). |
| `hybrid n1debug 16777216` | On request: the rising edge of debug bit 24 compares one frame. Set the bits back to 0 to arm it again. (From `botsctl`: `exec hybrid n1debug 16777216`.) |
| `BOTS_N1_AB_DIR=<dir>` | Writes `ab.csv` (one row per compared frame) there, and `abNNNN-a.png`, `-b.png`, `-diff.png` for a frame that differs. `BOTS_N1_AB_SAVE=all` writes them for every frame. The diff image is `|A - B| x 64` per channel, so one 8-bit step shows as 64/255. |

Each compared frame logs one line (read `log` or the game's log file):

```
summary: n1: ab pwl frame 340 (1280x720, 912 draws): max 0.000 mean 0.000000 differ 0 of 921600 px, mean B 41.27 | 6 compared, worst max 0.000
```

`max` and `mean` are per-channel differences in 8-bit steps (1.000 = one step), `differ` counts pixels where any colour
channel differs at all, `mean B` is B's mean channel value (a blank frame would read 0). At exit it logs the totals.

A compared frame costs three renders (A, B and the normal one the player sees); the other frames cost nothing extra. Don't
combine it with `BOTS_N1_GPU_PROF`: it then stays off. The target format must be 8-bit (RGBA or BGRA) or RGBA16F.

## Timing two paths in one process (`BOTS_N1_AB_TIME`)

`BOTS_N1_AB_TIME=1` turns the same mechanism into a GPU stopwatch: every frame, A and B are each recorded between two GPU
timestamps (scene plus post), nothing is copied or compared, and the pairs are logged 20 to a line:

```
summary: n1: abtime pwl 08.395/8.382 18.014/8.070 ...        (<order><A ms>/<B ms>; order 1 = B ran first)
```

Why it is quieter than comparing builds: both variants see the same frame state, the same clocks and the same thermal state, and
the order alternates every frame, so a drifting boost clock or a "second pass is warm" effect cancels. On a frozen scene (a paused
game) a pair ratio varies by about 1 % per frame, and a few hundred pairs resolve a 0.2 % cost difference. The cost of an A/B
frame is three renders, so use it on a paused or idle scene, not for fps. Read it with `ops/bench/gate2/abtime.py` in the
private ops repo, or by hand: ratio = sum(B) / sum(A). `BOTS_N1_AB_EVERY` still sets the interval (default 1 in this mode).

Limits: both variants live in one shader (a push-constant branch), so the timing says what the alternative costs *inside that
shader*, not what a separately compiled shader would cost; confirm with a live-play A/B of two builds. The first run with a new
shader text still pays the pipeline compile (minutes on a low-end GPU).

## Features

A feature is a bit in a push constant the shaders read. A renders with the bit set, B with it clear.

| Feature | A (bit set) | B (bit clear) |
|---|---|---|
| `pwl` | the per-segment select PWL gamma decode that 1.15 to 1.18 used | the branch-free decode (`xe_pwl3` in `n1/matdef/src/scene.wgsl`), bit-identical to A: max 0 over every frame on GTX 1070, T480 and Deck. The same check over every f32 from 0 to 1 is `n1/n1ffi/tests/pwl_gpu.rs` (it skips without a Vulkan device) |
| `lsb` | self-test: adds one 8-bit step to the red output | nothing |
| `pwl189` | !189's branch-free decode (before the bit-exact change), to time it against the shipping one: `BOTS_N1_AB=pwl189` is !189 against shipping, `BOTS_N1_AB=pwl BOTS_N1_AB_B=pwl189` is 1.18.0's decode against !189's |
| `alu` | timing self-test: A adds a dependent ALU loop of `BOTS_N1_AB_ALU` iterations to every decode call (the picture is unchanged); a known cost the timing must find |
| `pwlprobe` | self-test: the old decode plus 4/1023, to show the hook reaches the scene's pixels | nothing |

**Self-tests:** `BOTS_N1_AB=none` must report exactly 0 differing pixels, and `lsb` must report `max 1` with a differing
pixel count near the frame's size. Run both before trusting a green `pwl` result.

## Add a pair for another rewrite

1. Give the feature a bit and a name in `FEATURES` in `n1/n1ffi/src/ab.rs`.
2. Put the old code in `scene_text_with` or `post_text_with` in the same file (declarations, plus a hook that returns
   early when `ab_pc.flags` has the bit), and a `{{AB_...}}` token at the place in `scene.wgsl` or `post.wgsl` where the
   shader should branch. The new code stays in the shader as it ships; the old code lives only in `ab.rs`.
3. Run `none`, the new feature and a probe that you know changes pixels.

Passes that are not shaders (a fog volume rewrite, a god-ray pass) can read `AbFrame.flags` in `record` instead: it is the
per-variant flag word for the pass being recorded (`vkr.rs`, the `ab_flags` in the pass loop).

## Cost when off

By construction: `BOTS_N1_AB` unset gives `ab::config() == None`, so the hook tokens expand to empty text (the shaders are
the shipping text), `scene.wgsl` and `post.wgsl` carry no push constant, the pipeline layouts have no push-constant range,
`Renderer::ab` is `None`, `record` runs its single pass (`ab_passes == 1`) and no target or buffer is allocated. The unit
test `ab_hooks_compile_in_and_leave_no_trace_out` checks the text. With it on, the SPIR-V cache lives in its own
`spv-...-ab` folder so it never mixes with a normal run's.
