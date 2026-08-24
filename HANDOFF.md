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
| C core (`tools/phasec/vcbtrace`) | 13 probes: every board pixel, event counter, VMem section | **all match** |
| built Godot module | the same 13 probes through the same harness | **all match** |
| built Godot module | 3 real projects, per board pixel, 32 frames each | **`ALL 32 frames identical`** |
| C core | the same 3 projects (`bigdiff`) | **`ALL 33 solves identical`** |
| C core | 210 random corpus boards | 192 match; **all 18 failures have ≥2 RANDOM inks** (the one known divergence) |
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
   is only testable through the built module.

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
  though the kernel must not schedule it from one. Recorded (never wired up), so the
  count matches without touching the verified scheduler.

Tooling: two VMem probes (p12 write path, p13 read-back with preloaded memory) plus
the harness support they need (`vmem_bits`, `vmem_dump`, `vmem_start`, `asm_words`),
`tools/phasec/cmp_small.py` (module vs original on small boards), `tools/smoke_test.gd`
(the whole game), and three fixes to `diff.py` (a syntax error, and it ignored the
job's own output path so **every corpus board silently SKIPped**).

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
- **VMem breadth.** The two directions are now verified on small boards with 1–2
  address bits and 4 content bits. Not covered: wide address buses, the occurrence /
  lock path under rapid address changes, `get_vmem_persistent` ranges, a project whose
  assembly is actually loaded.
- **RANDOM ordering.** Boards with 0 or 1 RANDOM ink match. With ≥2, the two engines
  interleave draws from the board's shared MT19937 differently — 18/18 of the corpus
  failures are exactly this. The one known genuine divergence. Use
  `run_isolated.sh` for anything with RANDOM (the original seeds MT once per compute
  thread behind a TLS guard, so Godot thread reuse carries state across jobs).

Rough confidence for an arbitrary user project: **~97% logic-only, ~90% if VMem is
used, ~95% blended.** What remains is breadth (VINPUT, display, snapshots, wide VMem),
not a known defect.

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
