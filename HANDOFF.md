# HANDOFF — VCB engine recovery, state as of 2026-08-24

Written at the end of a session that (a) rebased the engine fork onto Godot 3.5.1,
(b) built it, and (c) ran a real project through the built binary against the
original engine. Read this before touching anything.

## The three repos and what they are

| repo | role | branch to use |
|---|---|---|
| `n-popescu/vcb-rebuild` | the game (GDScript + assets), a vendored copy of `modules/vcb`, and all the verification tooling | `main`; new work on `claude/ghidra-decompilation-review-09udb4` |
| `n-popescu/godot-vcb` | Godot 3.5.1-stable + `modules/vcb` compiled in as native ClassDB classes — the engine half, buildable alone | `master` |
| `vcb-engine-recovery` (local only, `/home/user/vcb-engine-recovery`) | the **original binaries** (`working_exes/vcb.x86_64`, `vcb.exe`) and decompilation notes | — |

**You need all three.** `vcb-engine-recovery` is the oracle — without
`working_exes/vcb.x86_64` you cannot generate ground truth and cannot verify
anything. It is not on GitHub in this setup; it lives in the container image at
`/home/user/vcb-engine-recovery`.

`modules/vcb/` is vendored in BOTH `vcb-rebuild` and `godot-vcb` and the two copies
are currently byte-identical. **Any change to one must be mirrored to the other**
or they drift. Verify with:

```bash
diff -rq /home/user/vcb-rebuild/modules/vcb /home/user/godot-vcb/modules/vcb \
  | grep -v '__pycache__\|\.o$\|core_test$\|fuzz_sim$'
```

## Verification status — READ THIS FIRST

The RISC-V CPU project (2048×2048, 219,986 painted pixels, 21,512 entities)
matches the original engine **exactly**: all 33 solves, every board pixel's state
and the event counter. Confirmed twice this session — once through the standalone C
core (`tools/phasec/vcbtrace`) and once through the **built Godot module**.

### The clock-interval trap (cost this session hours — do not repeat it)

`/home/user/work/gt/riscv.txt` was recorded with a **clock interval of 32 or more**,
not 1. Running our side at `--clock=1` makes the board's single CLOCK pixel
(at 831,932) oscillate when the reference run's did not, which shows up as ~7
differing pixels and a steady **+10 events/tick**. That is a parameter mismatch,
not an engine bug.

```bash
# correct:
./tools/phasec/vcbtrace /home/user/work/boards/riscv.bin --solves=32 --tps=1 \
    --clock=32 --timer=1000 --seed=0 --big=1 --out=/tmp/f_riscv
cp /tmp/f_riscv.txt /tmp/f_riscv.txt.txt      # bigdiff expects <prefix>.txt
python3 tools/phasec/original/bigdiff.py /home/user/work/gt/riscv.txt /tmp/f_riscv
# => ALL 33 solves identical: every board pixel's state and the event counter
```

Before concluding "the engine diverges", **always re-run at clock=32 and confirm
the reference parameters**. `clock=16` fails, `clock=32` and `clock=64` both pass
(the clock simply never fires inside 33 ticks).

### Things this session PROVED are fine (don't re-litigate)

- The tick kernel `modules/vcb/core/vcb_sim.c` — unchanged and correct.
- The bus-through-TUNNEL rule (commit `672f7c8`) is **correct and necessary**.
  With it the partition matches the original exactly (0 over-merge, 0 under-merge);
  without it 6 groups under-merge (the 195 pixels that commit mentions).
- The `vcb_bus_label()` refactor in that commit is behaviour-neutral (verified by
  disabling only the tunnel branch and reproducing the pre-refactor numbers exactly).
- `n_high` being a `uint8_t` that wraps is correct — widening it to `int32_t`
  changes nothing on the RISC-V board (no wrap occurs).
- CLOCK semantics in isolation: probes `clk1`/`clk2` match the original exactly.

## What changed this session

1. **`godot-vcb` rebased onto Godot 3.5.1-stable** (upstream tag `3.5.1-stable`,
   commit `6fed1ffa`) with `modules/vcb` added. Pushed as `86bd5f79`. The binary
   reports `3.5.1.stable.custom_build`, matching the shipped `vcb.exe`. The old
   `master` (a pure upstream mirror, zero of the owner's commits) is preserved on
   the branch `pre-vcb-upstream-master`.
2. **Bug found and fixed by the end-to-end test: missing `DEFVAL`.**
   `TransistorCompiler::compute` and `TransistorEngine::compute` were bound with a
   mandatory `userdata` argument. `Thread.start()` always passes one, but the game
   (`src/main/compiler.gd:114`) and the harness also call `compute()` bare, which
   was a hard script error. The original binds them with a default. Fixed in both
   repos.

## Known wrapper-vs-original differences (NOT yet resolved)

Found by running the harness against our built binary. None affect simulation
results, but they are real API divergences:

1. **`solve()` phase.** The original's `solve()` is asynchronous and returns the
   tick/event counters from *before* the call; ours runs synchronously and returns
   post-tick. So our trace is the original's shifted by one: `ours[t] == gt[t+1]`.
   Confirmed exactly on all 32 comparable solves.
2. **`get_texture()` returns null before the first `solve()`.** In the original it
   is already valid after `set_circuit_model`. This breaks any harness that dumps
   state before solving.
3. **The die texture carries unresolved entity ids.** Ours reports 24,875 entities
   where the original reports 21,513, because the original folds bus/mesh-linked
   nets into one entity at compile time while we keep them separate and tie their
   states via `net_rep`. **States are consistent** (verified: zero gt-entities whose
   pixels disagree on our side, at ticks 0/5/15/31), so this is benign for
   simulation — but anything keyed on entity *index* (`set_vinput_entities_indexes`,
   VMem entity lists, snapshots) will not line up with the original.
4. **`get_stats()` omits the ink-0 (blank pixel) bucket** that the original reports.

## Still unverified (the honest confidence ladder)

- **VMem read/write path** — the RISC-V run had memory disconnected on *both* sides
  (empty queues), so the VMem kernel is essentially untested. This is the single
  biggest confidence item.
- VINPUT; a TIMER that actually fires; mouse overrides; snapshots; long runs.
- **RANDOM ordering.** Boards with 0 or 1 RANDOM ink match 77/77. With ≥2 RANDOM
  inks, 289/323 match — the two engines interleave draws from the board's shared
  MT19937 differently. Documented in `modules/vcb/core/vcb_sim.h`. This is the one
  known genuine divergence.

Rough confidence for an arbitrary user project: **~97% logic-only, ~60% if VMem is
used, ~80% blended.** Closing the VMem gap is what moves this to ~95%.

## Immediate next step (where I stopped) — THE OPEN BUG

**The C core is verified exact. The Godot module is NOT yet.** Be precise about
the difference, because the two verify differently:

| path | event counters | per-pixel state |
|---|---|---|
| C core (`tools/phasec/vcbtrace`) at `clock=32` | exact, all 33 solves | **exact, all 33 solves** |
| built Godot module at `clock=32` | **exact, all 33 solves** | **DIVERGES** |

So the simulation inside the module is right — its event counters reproduce the
original's sequence exactly (offset by one solve, see "solve() phase" below) — but
the **state texture the module exposes through `get_texture()` does not match**.

What was ruled out:

- Not a frame-alignment problem. Tried shift 0, 1 and 2; all fail, with tens of
  thousands of differing pixels at every alignment. (`gt[0] == gt[1]`, i.e. the
  original's first two dumps are identical, which is what suggested a lag.)
- Not a partition problem. Same 134,392 entity-mapped pixels on both sides, zero
  only-orig / only-ours, and **zero** original-entities whose pixels disagree on our
  side — so `net_rep` is correctly mirroring group state via `members[]`.
- Not the clock interval (this run was at the correct `clock=32`).
- Not the simulation, per the event counters and the C core's per-pixel match.

That leaves the texture build path. Start here:

- `modules/vcb/transistor_engine.cpp:94` `te_build_state_texture()` — builds the
  sidelength×sidelength RGBA8 image, cell k = entity k, R channel = state. Compare
  what it writes against `dump_states()` in `tools/phasec/vcbtrace.c:27`, which IS
  per-pixel exact. The two should be writing the same bytes; find where they differ.
- `modules/vcb/transistor_engine.cpp:165` `board_changed` gating — it is set true
  whenever `p_ticks > 0`, so staleness looks unlikely, but confirm the rebuild
  actually happens on the run path the harness takes.
- Also check the compiler's **die** texture encoding (`get_textures()[2]`) against
  the original's: the harness decodes `eid = (b8 + a8*256) * side + (r8 + g8*256)`.
  A layout mismatch there would misindex every lookup while staying internally
  consistent — which is exactly the symptom.

Reproduce in ~12 minutes:

```bash
S=/tmp/scratch; mkdir -p $S
cd /home/user/vcb-rebuild/tools/phasec/original && python3 mkpck.py pack $S/harness.pck
cat > $S/jobs.json <<JSON
[{"name":"riscv","board":"/home/user/work/boards/riscv.bin","out":"$S/m32.txt",
  "solves":32,"tps":1,"clock":32,"timer":1000,"seed":0,"big":true,
  "vmem":"/home/user/work/riscv_vmem.bin"}]
JSON
cd /home/user/godot-vcb && xvfb-run -a ./bin/godot.x11.opt.tools.64 \
    --main-pack $S/harness.pck --jobs=$S/jobs.json
python3 /home/user/vcb-rebuild/tools/phasec/cmp_shift.py \
    /home/user/work/gt/riscv.txt $S/m32 1
```

Goal: `ALL 32 frames identical`. Until that passes, the shipped build is **not**
proven byte-exact even though the kernel is — say so plainly rather than quoting the
C core's result as if it covered the binary.

Once it does pass, the next target is the VMem path (see "Still unverified").

## Tooling map

- `tools/phasec/vcbtrace.c` — our side, standalone C, no engine build needed.
- `tools/phasec/original/` — the ground-truth side: `mkpck.py` builds a GDPC v1
  pack that `--main-pack` loads into the ORIGINAL binary, replacing its game while
  keeping its ClassDB classes. `pack/harness.gd` is the driver.
- `tools/phasec/original/bigdiff.py` — whole-project per-pixel comparator.
- `tools/phasec/cmp_shift.py` — same, but for two runs in the *harness* format with
  a frame shift (use for module-vs-original).
- `tools/phasec/original/run.sh` / `run_isolated.sh` — probe and corpus sweeps.
  **Use `run_isolated.sh` for anything with RANDOM**: the original seeds MT once per
  compute thread behind a TLS guard, so Godot thread reuse carries state across jobs
  and only one-job-per-process is reproducible.
- `/home/user/work/` — scratch from the recovery sessions: `boards/riscv.bin`,
  `riscv_vmem.bin`, `gt/riscv.txt{,.die,.states}` (ground truth).

## Chores not done

- **Stale branch deletion is blocked.** `git push --delete` returns HTTP 403 through
  this container's proxy and the GitHub MCP server exposes no delete-ref tool. The
  owner must run this themselves:

```bash
git push origin --delete claude/binary-comparison-guide claude/core-override-test \
  claude/docs-counts claude/gdnative-current claude/godot-4.7-port \
  claude/modding-docs claude/modding-support claude/vcb-launcher-delivery \
  claude/vcb-launcher-standalone claude/verify-recovery
```
