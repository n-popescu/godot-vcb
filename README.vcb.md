# godot-vcb — Godot 3.5.1 with the VCB simulation engine built in

This is **Godot Engine 3.5.1-stable** (upstream tag `3.5.1-stable`, commit `6fed1ffa`)
with [`modules/vcb/`](modules/vcb/) added: the recovered VCB simulation engine,
compiled straight into the binary as native `ClassDB` classes. Build it and you get
an editor that knows what a `TransistorEngine` is — no `.so`, no `.dll`, no GDNative.

That is how the original `vcb.exe` was built, which is why this repo exists: it is
the engine half of the game, standalone and buildable on its own. Point it at the
game project ([`vcb-rebuild`](https://github.com/n-popescu/vcb-rebuild)) and the
thing runs.

**Why 3.5.1 specifically.** The shipped `vcb.exe` reports
`3.5.1.stable.custom_build`. Matching it removes a variable when comparing against
the original, and it is the version the engine has actually been verified on.

## What the module provides

Five classes, registered natively by [`modules/vcb/register_types.cpp`](modules/vcb/register_types.cpp):

| class | role |
|---|---|
| `TransistorCompiler` | board image → circuit graph (classify, flood, link, finalize) |
| `TransistorCircuitModel` | the compiled graph |
| `TransistorEngine` | the tick kernel, plus the render textures and snapshots |
| `TransistorEditorHelper` | editor paint operations (flood fill, transpose) |
| `TransistorBuilderHelper` | the builder's cell store |

The simulation itself lives in [`modules/vcb/core/`](modules/vcb/core/) as
Godot-free C, so it can be unit-tested and diffed against the original engine
without an engine build at all. The C++ around it is only the Godot binding.

## Build

Dependencies (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y scons pkg-config build-essential \
  libx11-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libglu1-mesa-dev libasound2-dev libpulse-dev libudev-dev
```

Then, from this directory:

```bash
scons platform=x11     tools=yes target=release_debug bits=64 -j$(nproc)
scons platform=windows tools=yes target=release_debug bits=64 use_mingw=yes module_fbx_enabled=no -j$(nproc)
scons platform=osx     tools=yes target=release_debug -j$(sysctl -n hw.ncpu)
```

Roughly 25–30 minutes on 4 cores. The binary lands in `bin/`.

`module_fbx_enabled=no` is only needed for the MinGW Windows cross-build: stock
`modules/fbx` uses `int64_t` without including `<cstdint>`, which mingw's headers
don't pull in transitively. FBX import is irrelevant to a 2D circuit simulator.

## Check the engine is really in there

```bash
mkdir -p /tmp/vcbdoc
./bin/godot.x11.opt.tools.64 --doctool /tmp/vcbdoc --no-docbase
ls /tmp/vcbdoc/modules/vcb/doc_classes/
```

Five `Transistor*.xml` files means the classes are registered. On a headless
machine, prefix with `xvfb-run -a`.

## Run the game

```bash
git clone https://github.com/n-popescu/vcb-rebuild
./bin/godot.x11.opt.tools.64 --path vcb-rebuild --editor
```

The game's GDScript instantiates the classes by name (`TransistorCompiler.new()`,
`TransistorEngine.new()`), exactly as the shipped game did, so it opens, plays and
exports against this build.

## Test the engine without building Godot

The C core is standalone:

```bash
make -C modules/vcb/core/test         # unit tests
make -C modules/vcb/core/test fuzz    # 8000 random boards, delta-accumulator invariant
```

## How the engine was recovered, and how far it is verified

The engine was reverse-engineered from the original binary two independent ways —
Ghidra decompilation of `TransistorEngine::compute` (RVA `0x28f2c0`) and its per-ink
gate jump table (`0x28fc40`), and by driving the original engine itself headlessly
(the shipped Linux build honours Godot's `--main-pack`, so a harness scene can be
loaded into it while its own `ClassDB` classes stay put). The two were then held
together by a differential test comparing per board pixel, per tick, including the
event counter.

A real 2048×2048 RISC-V CPU project — 219,986 painted pixels, 21,512 entities —
matches the original exactly. So do 503 of 536 generated boards; every mismatch
carries two or more RANDOM inks, where the two engines can interleave draws from the
board's shared MT19937 differently. That ordering is the one known remaining
divergence and is documented in [`modules/vcb/core/vcb_sim.h`](modules/vcb/core/vcb_sim.h).

Not yet differentially verified: the VMem read/write path, VINPUT, and a TIMER that
actually fires. See `docs/VERIFICATION.md` in `vcb-rebuild` for the full ladder and
the harness that runs it.

## Relationship to the other repos

| repo | what |
|---|---|
| **godot-vcb** (here) | the engine: Godot 3.5.1 + `modules/vcb` |
| [`vcb-rebuild`](https://github.com/n-popescu/vcb-rebuild) | the game (GDScript + assets), the same module, and the verification tooling |
| `vcb-engine-recovery` | the decompilation notes and the original binaries |

`modules/vcb/` is currently vendored here as a copy of the one in `vcb-rebuild`.
Keep them in sync, or make one the source of truth and the other a submodule —
otherwise they will drift.

---

Everything outside `modules/vcb/` is stock Godot 3.5.1 under its own MIT licence;
see [`LICENSE.txt`](LICENSE.txt) and [`COPYRIGHT.txt`](COPYRIGHT.txt).
