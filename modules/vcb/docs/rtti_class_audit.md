# Binary RTTI class audit — are there VCB classes we missed?

**Result: no.** The only VCB-specific classes in `vcb.exe` are the five
`Transistor*` classes the module already provides. Verified two independent ways
from the binary's MSVC RTTI (the exe keeps full type descriptors).

## Method 1 — all RTTI type descriptors, minus Godot

`vcb.exe` contains **6874** RTTI type descriptors (`.?AV...@@` / `.?AU...@@`),
of which **1116** are plain top-level class names. Subtracting the class names
present in the Godot 3.5.1 source tree (and dropping lowercase/`_`-prefixed CRT
helpers) leaves ~65 names — **all of them stock Godot internals** my source-grep
simply didn't name-match: generated GLES shader classes (`SceneShaderGLES3`,
`CanvasShaderGLES2`, `TonemapShaderGLES3`, …), software-physics shapes
(`ConcavePolygonShapeSW`, `HeightMapShapeSW`, …), Bullet integration callbacks
(`GodotClosestRayResultCallback`, …), ENet (`ENetDTLSClient`, `ENetUDP`),
platform (`OS_Windows`, `PowerWindows`, `WindowsTerminalLogger`), and
`JavaClass`/`JavaScript`/`IUnknown`.

**The only non-Godot names are:**
`TransistorEngine`, `TransistorCompiler`, `TransistorCircuitModel`,
`TransistorEditorHelper`, `TransistorBuilderHelper`.

## Method 2 — the GDScript-callable set (MethodBind templates)

Every `ClassDB`-registered class with bound methods appears in the binary as a
`?$MethodBind<N>@V<Class>@@` template instantiation. Extracting the class from
every such template gives **526 distinct classes** — and **all 526 are stock
Godot** (`Node`, `Control`, `Input`, `Physics2DServer`, `AudioEffect*`,
`VisualShaderNode*`, …) **except**:
`TransistorCompiler`, `TransistorEngine`, `TransistorEditorHelper`,
`TransistorBuilderHelper`. `TransistorCircuitModel` has no bound methods (it is
registered only as a `Ref` data type — it appears as the return type of
`TransistorCompiler::get_circuit_model`).

## VCB-keyword sweep (sanity check)

Searching all RTTI names for `transistor|vcb|circuit|vmem|vinput|assembler|latch|
trace|ink|tunnel|heatmap|breakpoint` returned only: the five `Transistor*`
names/templates; Godot's `GDScriptParser::BreakpointNode`; and the `Input*`
classes (false positives — MSVC mangles a class ref as `V<Name>`, so `VInput…`
contains "vinput"). No additional VCB class.

## Conclusion

The recovery/module scope is complete and correct: **exactly 5 native VCB classes**
(4 with methods + the data-only `TransistorCircuitModel`). Everything else the
engine calls is stock Godot/CRT, linked at build time. Reproduce with
`vcb-engine-recovery/scripts/classify_function.py` and the RTTI scan in this doc.
