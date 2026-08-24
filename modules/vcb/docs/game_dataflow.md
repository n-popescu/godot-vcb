# Original game data flow (`vcb-original/src`)

A map of how the shipped VCB game is wired — what links to what, and where the
native engine's **outputs** go — from a full pass over `vcb-original/src` (190
`.gd` files + the render shader). This is the context for wiring `modules/vcb/`
back into the game and for Phase C. It complements
[`gdscript_api_audit.md`](gdscript_api_audit.md) (the exact native method surface).

## 1. Architecture: an event bus

Entry point is `src/main/main.tscn` (`project.godot` → `run/main_scene`). Eight
GDScript autoloads (no native ones): **`E`** (`events.gd`, the bus), **`C`**
(`constants.gd`), `Actions`, `BetterInput`, `L` (`log`), `G` (`globals`), `Q`
(`queries.gd`), `U` (`utilities`).

`E` is a global signal hub. Every lowercase `const … := {…}` in `events.gd` becomes
a user signal; the dict keys (`p_*`) are its payload fields. Components:
- **produce**: `E.echo(E.some_event, { E.some_event.p_x: value })`
- **consume**: `E.follow_events(self, [E.some_event])` + a `_ev_some_event(mode, args)` handler.
- `E.ask` / `E.order` are the two-way (`_tw`) request variants.

So "what links where" is: **who echoes an event ↔ who follows it.** Event prefixes:
`mn_` main, `ed_` editor, `sm_` simulator, `vd_` virtual devices (vmem/vinput/vdisplay),
`fs_` filesystem, `mi_` mouse input, `ui_` UI, `as_` assembler.

`systems.gd` wires the top-level nodes: `Compiler.FileSystemClass = FileSystem`,
`Compiler.EditorClass = Editor`.

## 2. Compile flow (editor drawing → model + textures)

`main/compiler.gd` is a state machine (`WAITING_YIELD → BEGIN → SETUP → POLL →
DISPATCH → FINISH`, or `ABORT`), driven by `mi_mode_change_requested(true)`:

```
Editor draws into layered Godot Images (editor/editor.gd)
   │  EditorClass.get_building_image()   (or get_visible_building_image())
   ▼
SETUP:    TC = TransistorCompiler.new(); TC.setup(image)
          thread = Thread.new(); thread.start(TC, "compute")     ← runs the pipeline off-thread
POLL:     loop TC.get_progress() until 1000 (or -1 → ABORT, prints TC.get_errors() via errmsg[])
DISPATCH: thread.wait_to_finish()
          textures = TC.get_textures()  →  { texture_on, texture_off, texture_die,
                                             texture_inverse_entitylut, texture_buslut,
                                             texture_busentities }
                        └─ echo sm_rendering_textures_update  → circuit_renderer + simulator
          TC.compute_vmem_data(live_vmem, assembly_binary,
                               generate_vmem_queues(…, texture_die.get_data(), sidelength))
          TC.set_vinput_entities_indexes(
                               generate_vinput_indexes(…, texture_die.get_data(), sidelength))
          circuit_model = TC.get_circuit_model()
                        └─ echo sm_circuit_model_built        → simulator (builds the engine)
          echo sm_statistics_change: TC.get_stats()           → card_statistics.gd
FINISH:   emit mi_mode_change_confirmed(true)
```

Note both `compute_vmem_data` and `set_vinput_entities_indexes` are fed **entity
indices decoded from `texture_die`** — i.e. the die texture (pixel→entity LUT) is
the key the GDScript uses to turn drawn VMEM/VINPUT pixels into entity indices.

## 3. Simulate flow (model → engine → per-frame outputs)

`main/simulator.gd`. On `sm_circuit_model_built` it builds the engine:

```
TE = TransistorEngine.new()
TE.set_circuit_model(circuit_model)
TE.set_clock_timer_intervals([clock_interval, timer_interval])
TE.set_random_seed(seed)
TE.set_vdisplay_settings(size, pointer, word_size, color_depth, palette)
thread.start(TE, "compute", null)     ← the background simulation worker (see §6)
TE.solve(1, [], 0, 0, 0)              ← prime one tick
is_engine_ready = true
```

Then every `_physics_process(delta)` while running:
- computes `ticks_this_frame` from `simulation_speed * MAX_TICKS_PER_SECOND` (=5,000,000) and `delta` (a tick accumulator); step mode uses `skip_tick_step`;
- handles step back/forward via `snapshot_restore_prev/next` + `snapshot_is_*_possible`;
- **`result = TE.solve(ticks_this_frame, override_set.keys(), vinput_value, vmem_range, time_paused)`** — the main call;
- reads the outputs (below) and fans them out over the bus.

`override_set` is filled by `set_mouse_override(pos, state)`: it samples
`texture_die` at the clicked board pixel → entity-LUT coords `(r8+g8·256, b8+a8·256)`,
checks `TE.is_entity_latch(x,y)`, and stores a key `x<<32 | y<<8 | state`.

## 4. The engine's outputs (result array + getters) and where they go

`solve()` returns the `TE_RESULT` array; per frame `simulator.gd` distributes:

| Output | Source | Fanned out to |
|---|---|---|
| **state texture** | `TE.get_texture()` → `process_state_texture()` | `echo sm_circuit_state_process` → `circuit_renderer.gd` sets shader `smp_sm_state` (**the on-screen board**) |
| tick / event / TPF / EPF | `result[CURRENT_TICK/CURRENT_EVENT/TPF/EPF]` | `echo sm_telemtry_change` → telemetry labels + `label_current_tick.gd` |
| VMEM address / ready | `result[VMEM_ADDRESS/VMEM_IS_READY_STATE]` | `echo vd_vmem_telemetry_change` → vmem editor / assembly code editor / telemetry label |
| VMEM section | `TE.get_vmem_section()` | `echo vd_vmem_editor_section_update` → `vmem_editor.gd` |
| VMEM persistent | `TE.get_vmem_persistent(begin,end)` (on stop) | `echo vd_vmem_persistent_data_recover` → vmem editor |
| vdisplay texture | `TE.get_vdisplay_texture()` (if enabled) | `echo vd_vdisplay_texture_render` → `world/vdisplay_renderer.gd` |
| **breakpoints** | `result[BREAKPOINT_SIMTEXTURE_POSITIONS]` | if non-empty → `order sm_pause_continue_toggle_tw` (pause) + `snapshot_take()` + `push_breakpoints_to_eventlog()` (maps each entity-LUT coord back through **`texture_inverse_entitylut`** to a board position for the Event Log) |
| VMEM occurrences | `result[VMEM_OCCURRENCES_ADDRESS/CONTENT]` | `push_vmem_occurrences_to_eventlog()` |

`TE_RESULT` enum (simulator.gd): `CURRENT_TICK, TPF, CURRENT_EVENT, EPF,
VMEM_ADDRESS, VMEM_IS_READY_STATE, BREAKPOINT_SIMTEXTURE_POSITIONS,
VMEM_OCCURRENCES_ADDRESS, VMEM_OCCURRENCES_CONTENT`.

## 5. Rendering — the final output (`world/circuit_renderer.gd` + the shader)

`graphics/shaders/circuit_renderer.shader.gd` (canvas_item) is where it all lands.
Uniforms and how each texture is read:

- `smp_sm_die` (board→entity): `coords = (die.r + die.g·256, die.b + die.a·256)`; `(65535,65535)` flags a **bus** pixel.
- `smp_sm_state` (from `TE.get_texture()`, sampled at `coords`): **`r` = on/off**, **`g·255` = raw type** (`==12` ⇒ LED), **`a·255` = LED palette index**.
- `smp_sm_on` / `smp_sm_off`: the display colours (mixed by `is_on`).
- `smp_sm_buslut` (board→bus index) + `smp_sm_busentities` (per bus, a `(0,0)`-terminated list of connected entity coords): the shader walks up to 32 entities and lights the bus if any is on.
- `smp_sm_led_palette`: LED colour lookup by index.

So the six compiler textures + the per-tick state texture are exactly the shader's
inputs; the die/state channel meanings above are what drove the module's
`texture_die`, `texture_inverse_entitylut`, and state-texture formats.

## 6. The two `compute` worker threads (important integration detail)

Both native compile/simulate loops are started on a background `Thread`:
- `compiler.gd:102` → `thread.start(TC, "compute")`
- `simulator.gd:134` → `thread.start(TE, "compute", null)`

`Thread.start` **always** calls the target with one `userdata` argument, so both
`compute` methods must accept it. The first API audit (which grepped dotted
`TC.`/`TE.`) missed the string-invoked engine worker and bound
`TransistorCompiler.compute` with zero args. **Fixed** (see `gdscript_api_audit.md`):
both `compute` methods now take `userdata`, and `TransistorEngine.compute` is
registered. In this reconstruction the tick advance is driven synchronously by
`solve()`, so the engine's `compute` is a no-op worker; a future threaded kernel
would run its loop there.

## 7. The virtual devices (`world/` + `gui/sidepanels/`)

- **vinput** (`vinput_processor.gd`, `vinput_interface_renderer.gd`): edits a value → `echo vd_vinput_value_change` → simulator passes it into `solve(…, vinput, …)`; the compiler records the vinput entity indices (`set_vinput_entities_indexes`).
- **vmem** (`vmem_pixels_renderer.gd`, `gui/sidepanels/vmem_editor|vmem_settings`): the addressable-memory editor; consumes `get_vmem_section` / `get_vmem_persistent` and the `VMEM_*` result fields; feeds `compute_vmem_data`.
- **vdisplay** (`vdisplay_renderer.gd` + `graphics/shaders/vdisplay_renderer.shader`, `gui/sidepanels/virtual_display`): a "screen" region rendered from `get_vdisplay_texture()`.

These map to the native pieces (VMem, virtual display) whose exact bit-level timing
is the remaining Phase-C work, tracked in [`../../../ARCHITECTURE.md`](../../../ARCHITECTURE.md) §6.

## 8. Key files

| File | Role |
|---|---|
| `main/main.tscn`, `main/main.gd`, `main/systems.gd` | scene root + node wiring |
| `singletons/events.gd` | the event bus (all signals + payload schemas) |
| `singletons/constants.gd` | `C.*` — palette (EDITOR/ON/OFF hex), circuit size, enums |
| `main/compiler.gd` | drives `TransistorCompiler` (compile state machine) |
| `main/simulator.gd` | drives `TransistorEngine` (the per-frame `solve()` loop) |
| `editor/editor.gd` + `editor/tool_*.gd` | drawing; produces the building Image; uses `TransistorEditorHelper` |
| `world/circuit_renderer.gd` + `graphics/shaders/circuit_renderer.shader.gd` | renders the board from the textures + state |
| `world/vdisplay_renderer.gd`, `vmem_pixels_renderer.gd`, `vinput_processor.gd` | the virtual devices |
| `assembler/` | assembles programs → `as_program_assemble` → `compute_vmem_data` |
