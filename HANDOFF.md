# HANDOFF — VCB engine recovery, state as of 2026-08-24 (session 2)

Read this before touching anything. It supersedes the previous handoff; the open bug
it described (the module's `get_texture()` diverging per pixel) is **fixed and
verified**, and the VMem path is now covered too.

## The three repos and what they are

| repo | role | branch to use |
|---|---|---|
| `n-popescu/vcb-rebuild` | the game (GDScript + assets), a vendored copy of `modules/vcb`, and all the verification tooling | `main` |
| `n-popescu/godot-vcb` | Godot 3.5.1-stable + `modules/vcb` compiled in as native ClassDB classes — the engine half, buildable alone | `master` |
| `vcb-engine-recovery` | the **original binaries** (`working_exes/vcb.x86_64`, `vcb.exe`) and decompilation notes | — |

**You need all three.** `vcb-engine-recovery` is the oracle — without
`working_exes/vcb.x86_64` you cannot generate ground truth and cannot verify
anything.

`modules/vcb/` is vendored in BOTH `vcb-rebuild` and `godot-vcb` and the two copies
must stay byte-identical. **Any change to one must be mirrored to the other.**
Verify with:

```bash
diff -rq <vcb-rebuild>/modules/vcb <godot-vcb>/modules/vcb \
  | grep -v '__pycache__\|\.o$\|core_test$\|fuzz_sim$'
```

## Verification status

Everything below was re-established from scratch this session against
`working_exes/vcb.x86_64` driven headlessly, because the previous session's scratch
directory (`/home/user/work/`: the RISC-V board and its ground truth) does not exist
outside that container. **Ground truth is now reproducible from what is in the repos**
— the boards come from `sample_projects/*.vcb`, so this does not depend on a scratch
directory surviving again.

| path | what was compared | result |
|---|---|---|
| C core (`tools/phasec/vcbtrace`) | 14 probes: every board pixel, event counter, VMem section | **all match** |
| built Godot module | the same 14 probes through the same harness | **all match** |
| built Godot module | 3 real projects, per board pixel, 32 frames each | **`ALL 32 frames identical`** |
| C core | the same 3 projects (`bigdiff`) | **`ALL 33 solves identical`** |
| C core | **400 random corpus boards**, one job per process | **369 match**; all 31 failures have ≥2 RANDOM inks (the one known divergence). By RANDOM-ink count: 0 → 28/28, 1 → 49/49, ≥2 → 292/323 |
| C core | the `n_in` / in-degree change, A/B on all 400 corpus boards | **400/400 byte-identical**, twice (vs `inert_inputs` excluded, and vs the exact pre-change `n_in`) |
| built Godot module | **a real project genuinely driving VMem** (20 address + 32 content latches): `02_32_bit_computer` both memory variants, `01_..._compact` with zeroed memory | **`ALL 32 frames identical`**, and the VMem telemetry + 1024-word memory image identical |
| whole game, built binary | opens each sample project, compiles it, simulates, renders | **`SMOKE TEST OK`** |

### Reproducing all of it

```bash
# 1. the algorithm alone, no Godot (CI-able)
make -C modules/vcb/core/test

# 2. probes: the original engine vs the C core, per pixel + per event + VMem
cd tools/phasec/original
VCB_WORK=/tmp/vcb-probes VCB_EXE=<recovery>/working_exes/vcb.x86_64 ./run.sh probes
#   -> 13 boards, "0 board(s) differ"

# 3. the same probes through the BUILT module, vs the same ground truth
cd <godot-vcb> && xvfb-run -a ./bin/godot.x11.opt.tools.64 \
    --main-pack /tmp/vcb-probes/harness.pck --jobs=<jobs with out= redirected>
python3 tools/phasec/cmp_small.py /tmp/vcb-probes/gt/p12.txt /tmp/vcb-probes/mod/p12.txt

# 4. whole projects. Boards come straight from the sample projects: the logic layer
#    of a .vcb is base64(zstd(2048x2048 RGBA8)) with the decompressed size in the
#    trailing 8 bytes (var2bytes) -- see tools/phasec/README.md.
#    Run both engines with the same harness pack and compare per board pixel:
python3 tools/phasec/cmp_shift.py /tmp/work/gt/computer32.txt /tmp/work/mod/computer32.txt 1
#   -> "ALL 32 frames identical ... every board pixel"

# 5. the whole game, through its own pipeline
xvfb-run -a <godot-vcb>/bin/godot.x11.opt.tools.64 --path . \
    -s res://tools/smoke_test.gd --report=/tmp/smoke.txt
```

## Traps that will cost you hours (all of them cost this session or the last one)

1. **The original's `solve()` is asynchronous and `get_texture()` publishes the
   worker's PREVIOUS frame.** With the worker keeping up, the texture read after
   solve number `i+1` is the state at tick `i`. That is the `shift` argument in the
   comparators — not a bug on either side. `get_vmem_section()` has the same
   one-frame lag; `get_vmem_persistent()` and the counters do not.
2. **The worker only keeps up if you let it.** On a whole project (thousands of
   events per tick) or on a loaded machine, 32 back-to-back solves leave it thousands
   of ticks behind: the trace reports `tick=0` throughout and every dumped texture is
   the same frame, so a frame-indexed comparison silently compares garbage. The
   harness now sleeps `settle_ms` (default 5) after each solve and prints a `FINAL`
   line with the settled counters — **check that `FINAL tick` equals the ticks you
   asked for.** Building Godot with `-j$(nproc)` in the background is enough to break
   this.
3. **Do not poll with `solve(0)` to wait for the worker.** `solve()` *overwrites* the
   tick budget rather than adding to it, so a `solve(0)` cancels the pending ticks and
   the worker never advances — the harness deadlocks.
4. **The original engine is not bit-deterministic run to run at the frame level.**
   Two runs of probe p9 differ at frame 15 (proven by running it twice). It is the
   same publication race. A single differing frame is not automatically your bug —
   re-run before believing it.
5. **Clock interval.** A board's single CLOCK pixel oscillating when the reference's
   did not is a parameter mismatch, not an engine bug. Always confirm the reference's
   `clock`.
6. **`PoolByteArray.resize()` does not zero the new bytes.** A VMem image padded that
   way carries heap garbage past the file, which differs between two engine builds and
   looks exactly like a VMem divergence. The harness pads explicitly now.
7. **A real-time TIMER cannot be compared against `vcbtrace`**, which has no wall
   clock to feed it. Probes p2/p10 use `timer=1000` so it never fires; a firing TIMER
   is only testable through the built module. **This bites hard on whole projects:**
   the harness's `settle_ms` makes real time elapse, so a project carrying a TIMER ink
   (`01_32_bit_computer_compact` has one, `02_32_bit_computer` has none) will fire it on
   the original and not on the C core. It cost an hour this session and looked exactly
   like a VMem bug. Set `timer` large enough that it cannot fire (100 s) for anything
   being compared, unless the TIMER is the thing under test.
8. **A clocked project does nothing until the clock fires.** `01_..._compact` has
   `clock_interval` 36, so a 32-tick run leaves the CPU idle and every comparison
   passes while testing nothing. Run enough ticks for several clock edges (2048 ticks =
   ~57 edges) before believing a whole-project VMem result.
9. **Check that the comparison compared something.** Three separate vacuous-pass bugs
   have now been found in this tooling: a syntax error that killed every probe run, a
   ground-truth path that made every corpus board `SKIP`, and `MATCH` reported for a
   board the original refuses to compile. `diff.py` now fails on an empty comparison —
   keep it that way, and be suspicious of a clean sweep you did not see the counts for.

## What this session found and fixed

**The state texture is a straight copy of the engine's `circuit_data` cell,
`{state, ink, n_conn, n_high}`** (confirmed by dumping the original's own texture
bytes, not by inference). Three of the four channels were wrong:

- `R` — raw state, **0 or 1**. We wrote `255`. The shader is `is_on = ceil(r)`, so 255
  renders identically; that is exactly why this looked right and still made every lit
  pixel differ. **This was the whole open bug.**
- `B` — `n_conn` = the entity's **in-degree**: a gate's input-net count, a net's driver
  count. We wrote 0. `vcb_model_indegree` is now the single definition, shared by the
  texture, the emitted `circuit_data` (which had used the undirected `conns.count`) and
  the kernel's `n_in`.
- `A` — raw `n_high`. We clamped to 15 and only for LEDs; the shader does that clamp
  itself (`round(min(a * 255.0, 15.0))`).

Also fixed, all real API divergences found by the new probes:

- `get_vmem_section()` ignored `solve()`'s `vmem_range` and returned the whole image.
  It is the VMem editor's visible window, packed `address | count << 32`
  (`vmem_editor.gd`), and the editor indexes the result from 0 — so the editor showed
  the wrong words at any scroll position but the top.
- **The compiled VMem image's word 0 is a reserved slot**: the original always leaves
  it 0, dropping both the live bytes *and* the assembly word there. Measured three
  ways (live word 0 = `0xab` with `assembly[0] = 0x55` still reads back 0, while every
  word from 1 up carries `live | assembly` exactly). Runtime writes to address 0 *do*
  land — it is only the compiled image that skips it.
- `vmem_ready` was hardcoded true; it is the inverse of the kernel's VMem lock, false
  on exactly the address-change ticks.
- The VMem occurrence lists (`result[7]`/`[8]`) were stubbed empty although the kernel
  collects them.
- `get_texture()` returned null until the first `solve()`; in the original it is valid
  as soon as the model is set.
- A gate's `n_conn` counts a READ junction drawn against a CLOCK / VINPUT / TIMER even
  though the kernel must not schedule it from one. Those nets are recorded in
  `inert_inputs`: they are **deliberately counted in `n_in`** (that is the whole point
  — it is what makes the byte match the original) but never added to the scheduler's
  adjacency, so nothing is ever scheduled from them. That is safe only because
  `vcb_gate_eval` reads `n_inputs` for AND (0x02) and NAND (0x06) alone and neither ink
  can carry an inert input; both halves of that are now unit-tested, and the whole
  change is confirmed behaviour-neutral on 400 corpus boards (see below).

Tooling: two VMem probes (p12 write path, p13 read-back with preloaded memory) plus
the harness support they need (`vmem_bits`, `vmem_dump`, `vmem_start`, `asm_words`),
`tools/phasec/cmp_small.py` (module vs original on small boards), `tools/smoke_test.gd`
(the whole game), and three fixes to `diff.py` (a syntax error, and it ignored the
job's own output path so **every corpus board silently SKIPped**).

## THE OPEN BUG — bus nets through long tunnel runs

`01_32_bit_computer_compact` with a patterned VMem image diverges from the original
(5/32 frames). **It is not a VMem bug**: the memory image is identical on every dump
and the interface words (address 1, content `1866972598`) agree across the divergence.
It is the compiler's **bus grouping**, which `core/vcb_bus.c` has always described as
"a best-effort model ... pending Phase-C validation".

It is now *measured* rather than assumed. The harness dumps the original's
`texture_buslut` (`"dump_bus": true` on a big job), which encodes each bus pixel's net
directly — otherwise invisible, since the die marks every bus pixel with the same
(65535, 65535) sentinel. On that board:

- the original has **669** bus nets, we have 673;
- we **split 6** of its nets and **over-merge 2**;
- that leaves **64 trace nets under-merged**, of which **8** surface as a state
  divergence (the rest happen to hold equal values);
- one of our bus "nets" is a **single pixel** (net 427 at (804,1042)), which is the
  clearest tell.

### What it is not

Probe **p14** pins down nine bus-adjacency rules and every one already agrees with the
original: orthogonal same-colour merges, diagonal (same *and* different colour) does
not, a one-cell gap does not, an adjacent TUNNEL pair does, straight through a CROSS
does, diagonally across a CROSS does not, through a MESH does, and a different trace
colour on the same bus is a separate channel. So it is not local adjacency, not
diagonal connectivity, and not the per-colour channel rule.

### What it looks like

The under-merged nets are **≥ 15 px apart** — the link is not local. The board carries
no MESH or WIRELESS near them (0 mesh-adjacent pixels) but 2004 TUNNEL pixels, and the
connection is a long tunnel run. Column x=804, scanning up from that single-pixel net:

```
 y=1042  BUS_3     <- our net 427 (a net of one pixel)
 y=1041  TUNNEL    <- the entrance
 y=1040  (empty)
 y=1039..1036  trace
 y=1034  TUNNEL  }
 y=1033  CROSS   }  a "TUNNEL, CROSS, TUNNEL" bridge
 y=1032  TUNNEL  }
 y=1030..1018  trace
 y=1016  TUNNEL / 1015 CROSS / 1014 TUNNEL      (again)
 y= 998  TUNNEL /  997 CROSS /  996 TUNNEL      (again)
 y= 980  TUNNEL /  979 CROSS /  978 TUNNEL      (again)
 y= 971  TUNNEL    <- the partner the original evidently uses
 y= 970  BUS_3     <- our net 409, which the original merges with 427
```

Our scan takes the **first** tunnel in the direction (y=1034) and emits the cell just
past it (y=1033), a CROSS, so the chain dies. Ten tunnels lie between the two bus
pixels; pairing them greedily from the near end leaves y=971 as the partner, which is
exactly the one that lands on the bus — but that is one hypothesis (bracket-style
nesting) and the intervening tunnels are probably serving *horizontal* traffic, which
would imply a different rule entirely.

**Recursively resolving the tunnel exit (step over a CROSS, tunnel again through a
TUNNEL) was tried and does NOT fix it** — the under/over-merge counts do not move. That
attempt was reverted rather than shipped: this function is already a guess, and
guessing again is what produced the bug. Get the rule from the binary
(`TC_tunnel_resolve` is the reconstruction of `0x3dff40`) or from probes built on
tunnel geometry the original *accepts*, then re-measure against `texture_buslut`.

### Reproducing it in about a minute

```bash
# assembles the board the way editor.gd::get_building_image does, and emits the
# address/content bit pixel lists for the job's "vmem_bits"
python3 tools/phasec/original/mkprojboard.py \
    sample_projects/01_32_bit_computer_compact.vcb board.bin meta.json
# job: {"board": board.bin, "big": true, "solves": 32, "tps": 64, "clock": 36,
#       "timer": 100000000, "vmem": <a patterned image>, "vmem_dump": 1024,
#       "vmem_bits": {"address": ..., "content": ...}, "dump_bus": true}
# run it through the original and through the built module, then:
python3 tools/phasec/cmp_shift.py gt/...-pattern.txt mod/...-pattern.txt 1
#   -> 5/32 frames identical
```

## Things proven fine — don't re-litigate

- The tick kernel `modules/vcb/core/vcb_sim.c` — the event-driven two-list update, the
  gate handlers, CLOCK, the VMem write/read-back order and the VMem lock.
- The bus-through-TUNNEL rule and the `vcb_bus_label()` refactor.
- `n_high` being a `uint8_t` that wraps.
- The die texture's encoding: `r+g*256 = idx % sidelength`, `b+a*256 = idx / sidelength`
  — exactly what `compiler.gd::get_entity_id` decodes, and byte-identical to the
  original's on a project where neither side merges nets.
- The partition difference (we keep bus/mesh-linked nets separate and tie their states
  through `net_rep`; the original folds them at compile time) is benign for state:
  every board pixel agrees. It is **not** benign for anything keyed on entity *index*.

## Still unverified (the honest confidence ladder)

- **VINPUT** — no probe drives it yet (the plumbing is there: the harness can pass
  entity lists the same way the VMem probes do).
- **A TIMER that actually fires**, mouse overrides, snapshots (step back/forward),
  long runs, the virtual display, `get_stats()`'s ink-0 bucket.
- **VMem is now verified on a real project**, not just probes: 20 address bits and 32
  content latches, non-empty `compute_vmem_data` queues, 2048 ticks, 10 distinct
  addresses with reads and writes, memory image and telemetry identical. What is still
  not covered: a project whose **assembly** is actually assembled and loaded (the
  sample programs are source text; the runs above preload either zeros or a synthetic
  pattern), the occurrence / lock path under rapid address changes, and
  `get_vmem_persistent` ranges.
- **The VMem latch pixels are painted, not stored.** `editor.gd::get_building_image`
  overlays them from `vmem_settings` every compile, so a board extracted from
  `layer_logic` alone has no VMem cells and silently tests nothing. Any future VMem run
  must assemble the board the same way (bit *i* of the address at
  `(A_POS_X - i*A_OFFSET_X, A_POS_Y + i*A_OFFSET_Y)`, bit 0 first, and likewise for
  content) and pass those pixels as `vmem_bits`.
- **RANDOM ordering.** Boards with 0 or 1 RANDOM ink match. With ≥2, the two engines
  interleave draws from the board's shared MT19937 differently — 18/18 of the corpus
  failures are exactly this. The one known genuine divergence. Use
  `run_isolated.sh` for anything with RANDOM (the original seeds MT once per compute
  thread behind a TLS guard, so Godot thread reuse carries state across jobs).

Rough confidence for an arbitrary user project: **~90% logic-only, ~88% if VMem is
used.** That is *lower* than the previous estimate on purpose — not because anything
regressed, but because a real project now demonstrates a real defect that no probe and
no spacious board reaches. VMem went up; buses went down, and buses are common. A
project that uses buses routed through tunnels can mis-simulate, silently, and the
symptom is a wrong net rather than a crash.

## Tooling map

- `tools/phasec/vcbtrace.c` — our side, standalone C, no engine build needed. Now also
  takes `--vmemaddr` / `--vmemcontent` / `--vmemdump` and always builds the VMem image.
- `tools/phasec/original/` — the ground-truth side: `mkpck.py` builds a GDPC v1 pack
  that `--main-pack` loads into the ORIGINAL binary, replacing its game while keeping
  its ClassDB classes. `pack/harness.gd` is the driver — and the same pack drives our
  built binary, which is what makes the two directly comparable.
- `tools/phasec/original/bigdiff.py` — whole project, original vs `vcbtrace`.
- `tools/phasec/cmp_shift.py` — whole project, original vs our built module.
- `tools/phasec/cmp_small.py` — small board, original vs our built module (per pixel,
  event counter and VMem section).
- `tools/phasec/original/diff.py` — the probe/corpus sweep comparator.
- `tools/smoke_test.gd` — boots the real game with a build, opens a sample project,
  compiles and simulates it. The "is this build usable" check.
