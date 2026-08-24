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

> **There is no CI here.** This fork inherited upstream Godot's eight build workflows,
> which pin the `ubuntu-20.04` runner image GitHub has since retired — so every job
> failed before it started, on every push, for all eight platforms. They built *stock*
> Godot for Android/iOS/JS/server anyway, which is not what this repo is for. They are
> removed (they are still in the history, and on the `pre-vcb-upstream-master` branch
> together with the untouched upstream tree). Build with the command below instead; it
> is one scons invocation and it is what the published binaries are made with.

Dependencies (Debian/Ubuntu):

```bash
sudo apt-get update
sudo apt-get install -y python3 python3-pip pkg-config build-essential xvfb \
  libx11-dev libxcursor-dev libxinerama-dev libxi-dev libxrandr-dev \
  libgl1-mesa-dev libglu1-mesa-dev libasound2-dev libpulse-dev libudev-dev
pip install scons     # or: apt-get install -y scons
```

Godot 3.5.1 builds fine with a **current** scons and Python — verified with scons 4.11
on Python 3.11 — so there is no need to pin old versions. If the build stops early with
a confusing configure error, it is almost always one of the X11 or audio `-dev` packages
above missing.

For the **Windows cross-build**, add mingw and switch it to the POSIX-threads variant;
Godot's `Thread` uses `std::thread` / `std::mutex`, which the win32-threads mingw lacks:

```bash
sudo apt-get install -y mingw-w64
sudo update-alternatives --set x86_64-w64-mingw32-gcc /usr/bin/x86_64-w64-mingw32-gcc-posix
sudo update-alternatives --set x86_64-w64-mingw32-g++ /usr/bin/x86_64-w64-mingw32-g++-posix
```

Then, from this directory:

```bash
scons platform=x11     tools=yes target=release_debug bits=64 -j$(nproc)
scons platform=windows tools=yes target=release_debug bits=64 use_mingw=yes module_fbx_enabled=no -j$(nproc)
scons platform=osx     tools=yes target=release_debug -j$(sysctl -n hw.ncpu)
```

Roughly 20–30 minutes from scratch on 4 cores; a rebuild after touching only
`modules/vcb` is about a minute. The binary lands in `bin/`. `strip --strip-all` it
before shipping (768 MB → 85 MB).

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

To check a build without a screen, `vcb-rebuild` carries a smoke test that boots the
real game, opens a sample project, compiles it through the game's own pipeline,
simulates it and reads back the state texture:

```bash
xvfb-run -a ./bin/godot.x11.opt.tools.64 --path vcb-rebuild \
    -s res://tools/smoke_test.gd --report=/tmp/smoke.txt
cat /tmp/smoke.txt   # -> SMOKE TEST OK: ... compiled, simulated N ticks and rendered
```

Building export templates and exporting the game as a single self-contained file is
documented in `BUILD.md` in `vcb-rebuild`.

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

A binary built from this tree matches the original **through the module's own API**,
not just in the C core: 13 of 13 structured probes agree on every board pixel, the
event counter and the VMem section, and three real sample projects (up to 641k painted
pixels / 9112 entities) give `ALL 32 frames identical — every board pixel`. A real
2048×2048 RISC-V CPU project — 219,986 painted pixels, 21,512 entities — matches
exactly as well.

The known remaining divergence is **RANDOM ordering**: boards with zero or one RANDOM
ink match, but with two or more the two engines can interleave draws from the board's
shared MT19937 differently. Every mismatch in a 210-board random corpus (18 of them) is
of that kind. Documented in [`modules/vcb/core/vcb_sim.h`](modules/vcb/core/vcb_sim.h).

The VMem read and write paths are now covered by dedicated probes. Still not
differentially verified: VINPUT, a TIMER that actually fires, snapshots, the virtual
display, and wide VMem configurations — breadth rather than a known defect. See
[`HANDOFF.md`](HANDOFF.md) for the current ladder and the traps involved, and
`tools/phasec/README.md` in `vcb-rebuild` for the harness that runs it.

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
