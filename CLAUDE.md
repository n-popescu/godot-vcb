# CLAUDE.md — godot-vcb

Quick context for an agent landing in this repo.

## What this repo is

**Godot Engine 3.5.1-stable** (upstream tag `3.5.1-stable`, commit `6fed1ffa`) with
[`modules/vcb/`](modules/vcb/) added: the recovered VCB simulation engine compiled
straight into the binary as native `ClassDB` classes. Build it and you get a Godot
binary that knows what a `TransistorEngine` is — no `.so`, no `.dll`, no GDNative.

That is how the original `vcb.exe` was built, which is the whole reason this repo
exists. 3.5.1 specifically because the shipped `vcb.exe` reports
`3.5.1.stable.custom_build`.

**Everything outside `modules/vcb/` is stock upstream Godot.** Do not refactor it, do
not "modernise" it, and do not fix upstream bugs here — the value of this tree is that
it differs from upstream in exactly one directory.

## The four repos

| repo | role |
|---|---|
| **`godot-vcb`** (here) | the engine half: Godot 3.5.1 + `modules/vcb`, buildable alone. Branch `master`. |
| **`vcb-rebuild`** | the game (GDScript + assets), a vendored copy of `modules/vcb`, and **all** the verification tooling. Branch `main`. |
| **`vcb-engine-recovery`** | the original binaries and the Ghidra decompilation notes — the oracle. |
| **`vcb-traces`** | a downstream mod (64 trace colours) carrying its own repacked copy of the engine. |

## The one rule that matters here

`modules/vcb/` is vendored in **both** this repo and `vcb-rebuild`, and the two copies
must stay **byte-identical**:

```bash
diff -rq modules/vcb <vcb-rebuild>/modules/vcb \
  | grep -v '__pycache__\|\.o$\|core_test$\|fuzz_sim$'
```

**Any change to one must be mirrored to the other, in the same session.** Engine work
normally happens in `vcb-rebuild` (that is where the tests and the differential harness
live) and is then copied here. Read `vcb-rebuild/CLAUDE.md` and `vcb-rebuild/HANDOFF.md`
before touching engine behaviour.

## What the module provides

Five classes, registered by [`modules/vcb/register_types.cpp`](modules/vcb/register_types.cpp):

| class | role |
|---|---|
| `TransistorCompiler` | board image → circuit graph (classify, flood, link, finalize) |
| `TransistorCircuitModel` | the compiled graph |
| `TransistorEngine` | the tick kernel, the render textures, snapshots |
| `TransistorEditorHelper` | editor paint operations (flood fill, transpose) |
| `TransistorBuilderHelper` | the builder's cell store |

The simulation itself is Godot-free C in [`modules/vcb/core/`](modules/vcb/core/); the
C++ around it is only the binding. That split is deliberate — it is what lets the
algorithm be unit-tested and diffed against the original engine with no engine build at
all.

## Commands

```bash
# the algorithm alone, no Godot, seconds
make -C modules/vcb/core/test

# the engine binary (Godot 3.5 platform names: x11 / windows / osx)
scons platform=x11 tools=yes target=release_debug bits=64 -j$(nproc)
#   -> bin/godot.x11.opt.tools.64

# run the game with it
bin/godot.x11.opt.tools.64 --path <vcb-rebuild>
```

A full build is slow (tens of minutes on a few cores) and memory-hungry. If you only
changed `modules/vcb/core/`, the unit tests in `vcb-rebuild` will tell you far more, far
faster, than a rebuild will.

**There is no CI on ordinary pushes.** The eight inherited upstream workflows pinned
the retired `ubuntu-20.04` runner image and built *stock* Godot for platforms this repo
does not care about, so they failed on every push and were removed. They are still in
the history and on the `pre-vcb-upstream-master` branch, together with the untouched
upstream tree. See `README.vcb.md`.

The one workflow that remains is `.github/workflows/release.yml`, and it is a *release*
workflow, not a check: it fires when `vcb_version.py` changes on `master`, gates on
`make -C modules/vcb/core/test`, builds the editor for x11/windows/osx (arm64 and
x86_64), and publishes them as `<godot>-vcb-<vcb>` — e.g. `3.5.1-vcb-1.0.0`, the Godot
base from `version.py` spliced onto the module version from `vcb_version.py`. So a
branch push still tells you nothing; it is on you to run the core tests locally.

`vcb_version.py` lives at the repo root rather than inside `modules/vcb/` on purpose —
that directory has to stay byte-identical to the vendored copy in `vcb-rebuild`, so it
cannot carry a file this repo alone needs.

## Conventions

- Inside `modules/vcb/`: C11 for the core, tabs, and every non-obvious rule carries a
  comment naming how it was established (a probe, an RVA in the binary, or the in-game
  user guide). Keep that.
- Outside `modules/vcb/`: upstream Godot style, and ideally no diff at all.
