# `vcb` — Godot 3.5.1 engine module

This module provides the five native `Transistor*` classes the VCB game expects
(`TransistorCompiler`, `TransistorEngine`, `TransistorCircuitModel`,
`TransistorEditorHelper`, `TransistorBuilderHelper`), reconstructed from the original
`vcb.exe` (see the `vcb-engine-recovery` repo / [`../../engine-recovery/`](../../engine-recovery/)).

`vcb.exe` was a **custom Godot 3.5.1 build** with these classes compiled in as a
module (ClassDB-registered, not GDNative), so the rebuild is: add this module to a
Godot 3.5.1 source tree and compile (see [`../../BUILD.md`](../../BUILD.md)). The
older GDNative delivery lives on the `gdnative-legacy` branch; both reuse this
`core/` verbatim.

## Layout

```
modules/vcb/
├── config.py, SCsub, register_types.*   # module registration
├── transistor_compiler.{h,cpp}          # Image -> model + 6 textures (wraps core/)
├── transistor_circuit_model.{h,cpp}     # compiled-graph data container
├── transistor_engine.{h,cpp}            # tick loop: solve() drives vcb_sim
├── transistor_editor_helper.{h,cpp}     # bucket fill/replace, transpose
├── transistor_builder_helper.{h,cpp}    # unused by the game (kept minimal for fidelity)
└── core/                                # the recovered algorithm, Godot-free C
    ├── vcb_types.h        vcb_vec.{h,c}          # structs + vector/alloc primitives
    ├── vcb_classifier.{h,c}  vcb_resolver.{h,c}  # verified colour->ink / ->(ON,OFF) tables
    ├── vcb_pipeline.{h,c}                         # prepare/scan/link/finalize + connectivity + net_table
    ├── vcb_model.{h,c}                            # entity graph + circuit_data/adjacency + bus/mesh/wireless merges
    ├── vcb_sim.{h,c}                              # event-driven tick (+ synchronous reference) + snapshots
    ├── vcb_bus.{h,c}  vcb_vmem.{h,c}  vcb_vdisplay.{h,c}   # bus textures / VMem image / display repack
    └── test/                                      # standalone unit tests (no Godot)
```

**Design rule:** the reverse-engineered algorithm is Godot-free C in `core/` so it
can be unit-tested standalone; the `.cpp` classes are thin wrappers translating Godot
types ↔ the C core.

## Verify the algorithm without Godot

```bash
make -C modules/vcb/core/test    # 54 checks, 0 failures
```

## More

The full engine reference (how the reconstruction maps onto `vcb.exe`, the verified
gate table, data flow, and what remains) is in
[`../../ARCHITECTURE.md`](../../ARCHITECTURE.md). Per-class API and RTTI audits are in
[`docs/gdscript_api_audit.md`](docs/gdscript_api_audit.md) and
[`docs/rtti_class_audit.md`](docs/rtti_class_audit.md); the full game data-flow /
render contract is in [`docs/game_dataflow.md`](docs/game_dataflow.md).
