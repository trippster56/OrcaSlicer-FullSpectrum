# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Overview

**Snapmaker Orca FullSpectrum** — a fork of Snapmaker Orca (itself forked from OrcaSlicer → Bambu Studio → PrusaSlicer → Slic3r). C++17, wxWidgets GUI, CMake build system, ~500k+ lines. AGPL-3.0.

The fork's purpose is **mixed-color filament support for the Snapmaker U1** multi-tool printer: virtual "mixed" filaments that create new apparent colors by alternating layers (or same-layer stripes) of two or more physical filaments. Most fork-specific work lives in the mixed-filament / Local-Z / cloud-sync code listed under "Fork-specific architecture" below.

## Build Commands

### macOS
```bash
./build_release_macos.sh          # deps + slicer
./build_release_macos.sh -d       # deps only
./build_release_macos.sh -s       # slicer only (after deps built)
./build_release_macos.sh -x       # use Ninja generator (faster)
./build_release_macos.sh -a arm64 # arch: arm64 | x86_64 | universal
./build_release_macos.sh -t 11.3  # macOS deployment target
```
Build output goes to `build/<arch>/` (e.g. `build/arm64/`), deps to `deps/build/<arch>/`. The app bundle lands in `build/<arch>/src/<config>/Snapmaker_Orca.app`. Use `-sx` when reproducing macOS build issues.

### Windows
```bash
build_release_vs2022.bat          # everything
build_release_vs2022.bat debug    # debug symbols
build_release_vs2022.bat deps     # deps only
build_release_vs2022.bat slicer   # slicer only
```
Requires `git lfs pull` after cloning. CMake max 3.31.x on Windows.

### Linux
```bash
./build_linux.sh -u    # first-time: install system deps
./build_linux.sh -dsi  # deps + slicer + AppImage
# other flags: -j N (cores), -1 (single core), -b (debug), -c (clean), -r (skip RAM checks), -l (Clang)
```

### Tests
Catch2-based, in `tests/` grouped by domain (`libslic3r/`, `fff_print/`, `sla_print/`, `libnest2d/`, `slic3rutils/`). Fixtures in `tests/data/`.

```bash
cd build/<arch> && ctest --output-on-failure   # all tests
./tests/libslic3r/libslic3r_tests              # single suite
./tests/libslic3r/libslic3r_tests "[Geometry]" # filter by Catch2 tag
```

### Formatting
`.clang-format` enforces 4-space indent, 140-column limit. Run `clang-format -i <file>` on touched files. Naming: PascalCase classes, snake_case functions/variables, SCREAMING_CASE constants. Use `#pragma once` in new headers.

## Architecture

### Layering
- **`src/libslic3r/`** — platform-independent slicing engine. Key classes: `Print`, `PrintObject`, `Layer`, `GCode`, and configuration in `PrintConfig.cpp/.hpp` (defines every print/printer/filament setting). Subdirectories: `GCode/` (generation, cooling, tool ordering), `Fill/` (infill patterns), `Support/` (tree + traditional), `Geometry/`, `Format/` (3MF/STL/AMF/OBJ/STEP I/O — native project format is `Format/bbs_3mf.cpp`), `Arachne/` (variable-width walls), `SLA/`.
- **`src/slic3r/GUI/`** — wxWidgets application. `Plater.cpp` is the central hub (scene, sidebar, filament list). `GUI_App.cpp` is the app object. `Tab.cpp` builds the settings tabs.
- **`src/slic3r/Utils/`** — printer-host integrations (OctoPrint, Moonraker, etc.) and networking.
- Entry point: `src/Snapmaker_Orca.cpp`.

### Fork-specific architecture (FullSpectrum)
This is where this fork diverges from upstream — understand these before touching multi-material code:

- **`src/libslic3r/MixedFilament.cpp/.hpp`** — core model for virtual mixed filaments. A `MixedFilament` row pairs physical filaments (`component_a`/`component_b`, plus optional 3+ color gradients) with a distribution mode: `LayerCycle` (alternate by layer cadence), `SameLayerPointillisme` (interleaved stripes), or `Simple`. Rows have a `stable_id` so painted virtual-tool assignments survive list rebuilds. Virtual filament IDs start after physical ones (4 physical → first mixed ID is 5).
- **`src/libslic3r/filament_mixer.cpp/.h`** — perceptual color-blend model used so mixed-color previews match real printed mixing (Blue+Yellow → Green, not RGB-average). All created/cached/preview colors go through this unified helper.
- **`src/libslic3r/GCode/ToolOrdering.cpp`** — layer-based alternation for mixed filaments is resolved here during tool ordering.
- **Local-Z** — per-region Z-offset machinery that recesses one component of a mixed pair ("bias") to shift apparent color; spans `LocalZOrderOptimizer.hpp`, `PrintObjectSlice.cpp`, `GCode.cpp`, `WipeTower2.cpp`, and a direct multicolor Local-Z solver with carry-over error for 3+ color rows. Supports whole-object Local-Z, not just painted mixed zones.
- **Dithering settings** — cadence height A/B and step size (Print Settings → Others → Dithering), defined in `PrintConfig.cpp` like all settings.
- **`src/slic3r/Utils/SnapmakerCloudSync.cpp/.hpp`** — pulls loaded filaments off a Snapmaker U1 over its AWS-IoT cloud MQTT connection (no LAN mode) and feeds `filament_ams_list` so the existing `PresetBundle::sync_ams_list()` machinery maps them onto presets. Cloud I/O runs on a worker thread; completion callbacks dispatch to the UI thread via `wxGetApp().CallAfter()`. Device pairing/certs persist in `AppConfig` as `DeviceInfo` via the WebDeviceDialog flow. Wired into the sidebar in `Plater.cpp` (mirrors the existing Bambu `Sidebar::load_ams_list` pattern).
- **GUI integration** — the "Mixed Colors" sidebar panel and related UI live in `Plater.cpp`, `Tab.cpp`, `GUI_ObjectList.cpp`, and `Gizmos/GLGizmoMmuSegmentation.cpp` (multi-material painting with virtual tools).
- **3MF serialization** — mixed-filament data is persisted in `Format/bbs_3mf.cpp`; serialization has changed across FullSpectrum versions, so be careful with backward compatibility of project files.

Version numbers live in `version.inc` (`Snapmaker_VERSION` and `FULLSPECTRUM_VERSION` are bumped together).

### Key upstream algorithms
Arachne variable-width walls, tree supports, lightning infill, adaptive layer height, multi-material segmentation (`MultiMaterialSegmentation.cpp`), G-code post-processing (cooling, pressure advance, conflict checking).

### Dependencies
Built once into `deps/build/<arch>/`, then linked. Major: wxWidgets, TBB (parallelization — used heavily, mind shared state), Clipper2, CGAL, OpenVDB, Eigen, libigl, OpenGL. Treat `deps/` and `deps_src/` as vendored snapshots — don't modify without mirroring upstream tags and noting the upstream commit in your PR.

## Repository layout notes
- `resources/profiles/` — printer/material profiles by manufacturer (JSON). `resources/printers/` — printer configs and G-code templates.
- `localization/i18n/` — translation sources; regenerate via `scripts/run_gettext.sh`.
- `src/libslic3r/MacUtils.mm` and `src/slic3r/Utils/MacDarkMode.mm` — Objective-C++ for macOS.
- `sandboxes/` — experimental code; keep API tokens and printer credentials out of tracked configs.

## Conventions
- Commit subjects are concise sentence-style, optionally with issue refs: `Fix grid lines origin for multiple plates (#10724)`.
- Adding a print setting: define in `PrintConfig.cpp` (bounds, defaults, tooltip) → add UI in the right `Tab.cpp`/GUI component → handle config save/load → test across printer profiles.
- Backward compatibility matters: project files (3MF), profiles, and settings migrations must be handled carefully — this fork already has known 3MF compatibility breaks between versions.
- Cross-platform (Windows/macOS/Linux) support is required for all changes.
