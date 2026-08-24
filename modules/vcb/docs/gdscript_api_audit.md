# GDScript API audit — the exact native surface the game requires

Audited **every** `.gd` file in the game `src/` (190 files) to enumerate every
method, constant, and property the GDScript calls on the native `Transistor*`
classes, so the module registers the complete and correct surface. This supersedes
the inferred method names in `vcb-engine-recovery/MASTER_PLAN.md` where they
differed from the real calls.

## Native classes actually used

| Class | Instance var | Where |
|---|---|---|
| `TransistorCompiler` | `TC` | `main/compiler.gd` |
| `TransistorEngine` | `TE` | `main/simulator.gd` |
| `TransistorEditorHelper` | `TEH` (via `ED.TEH`) | `editor/editor.gd`, `editor/tool_bucket.gd`, `editor/tool_selection.gd` |
| `TransistorCircuitModel` | (return type) | `main/compiler.gd`, `main/simulator.gd` |
| `TransistorBuilderHelper` | **unused** | — (registered for completeness; the shipped game never instantiates it) |

The autoloads `E, C, Actions, BetterInput, L, G, Q, U` are **GDScript** singletons
(`src/singletons/`), not native — no module work needed. There is no `T` singleton.

## TransistorCompiler (`TC`)  — all bound ✅

| Method | Args | Returns |
|---|---|---|
| `setup` | `Image` | void |
| `compute` | `userdata` (Variant, ignored) | void — started via `Thread.start(TC, "compute")` |
| `get_progress` | — | int (0..1000, -1 = error) |
| `get_entitylist_sidelength` | — | int |
| `get_circuit_model` | — | `TransistorCircuitModel` |
| `get_textures` | — | Array (6 Images) |
| `get_stats` | — | Array |
| `get_errors` | — | Array `[[code, Vector2], ...]` |
| `compute_vmem_data` | `PoolByteArray, PoolIntArray, Array` | void |
| `set_vinput_entities_indexes` | `Array` | void |

Constants (bound): `UNEXPECTED_TUNNEL_ENTRANCE=0`, `UNMATCHED_TUNNEL_LEFT=1`,
`UNMATCHED_TUNNEL_RIGHT=2`, `UNMATCHED_TUNNEL_UP=3`, `UNMATCHED_TUNNEL_DOWN=4`.

## TransistorEngine (`TE`)  — all bound ✅

`set_circuit_model(TransistorCircuitModel)`, `set_clock_timer_intervals(Array)`,
`set_random_seed(int)`, `set_vdisplay_settings(Vector2,int,int,int,Array)`,
`solve(int ticks, Array override_keys, int vinput, int vmem_range, int time_paused)`,
`compute(userdata)`, `stop()`, `get_texture()`, `get_vdisplay_texture()`,
`get_vmem_section()`, `get_vmem_persistent(int,int)`, `get_entity_state(int,int)`,
`is_entity_latch(int,int)`, `snapshot_take()`, `snapshot_clear_all()`,
`snapshot_clear_next()`, `snapshot_restore_next()`, `snapshot_restore_prev()`,
`snapshot_is_next_possible()`, `snapshot_is_prev_possible()`.

**Correction (full-src re-audit):** both classes have a **`compute`** method invoked
via `Thread.start(instance, "compute", …)` — `TransistorCompiler.compute` on
`compiler.gd:102` and `TransistorEngine.compute` on `simulator.gd:134`. Because the
first audit grepped `TC.`/`TE.` (dotted) it missed the string-invoked engine worker
and bound `TransistorCompiler.compute` with **zero** arguments. `Thread.start` always
passes a `userdata` argument, so a zero-arg method errors with `TOO_MANY_ARGUMENTS`.
Fixed: both `compute` methods now take a `userdata` parameter, and
`TransistorEngine.compute` is registered (a no-op worker — the tick advance is driven
synchronously by `solve()` in this reconstruction).

(`TE.connect/set/release_focus/text/values` seen in the grep are inherited
`Object`/`Control` methods or unrelated vars, not engine methods.)

## TransistorEditorHelper (`TEH`)  — corrected ✅

| Method | Args | Source |
|---|---|---|
| `initialize` | `int span` | `editor.gd:62` `TEH.initialize(CIRCUIT_SPAN)` |
| `bucket_flood_fill` | `Color, Color, Vector2, Image, Image, bool, bool` | `tool_bucket.gd:53` |
| `bucket_replace` | `Color, Color, Image` | `tool_bucket.gd:63` |
| `transpose` | `Image` | `tool_selection.gd:331` |

**Fix applied:** the module originally bound the inferred names
`update_image/update_mouse/update_brush/set_visible_latch` (same arities). They are
replaced with the real names above so `ED.TEH.*` calls resolve at runtime.

## TransistorCircuitModel

No GDScript-callable methods — a data container passed by `Ref` from the compiler
(`get_circuit_model`) to the engine (`set_circuit_model`). Field layout in
`vcb-engine-recovery/docs/transistor_circuit_model.md`.
