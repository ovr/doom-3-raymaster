# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Codex, etc.) when working with code in this repository. `CLAUDE.md` is a symlink to this file.

`AGENTS_LOCAL.md` exists in the repository root, read it as well: it holds local, machine-specific instructions and is not committed (git-ignored).

DOOM 3 BFG Edition (id Tech 4, GPL) ported to a CMake build on modern Windows. Only 64-bit MSVC (x64) builds are supported; the x86 build was dropped. You need VS2022 or its Build Tools, the Windows 10 SDK, and XAudio2 2.9 (Windows 10+). The legacy DirectX SDK is not used.

## Build

```
cmake --preset vs2022-x64            # configure (VS 2022 generator, x64)
cmake --build --preset release       # also: debug, retail
```

- zlib is a git submodule in `third_party/zlib` (upstream madler/zlib at a release tag), compiled into the `external` target from its `.c` files. After cloning run `git submodule update --init`, or configure stops with that hint.
- The output is `build\vs2022-x64\bin\<Config>\Doom3BFG.exe`. The generated `.sln` in `build\vs2022-x64` can also be opened in Visual Studio.
- For Ninja, run these from an "x64 Native Tools Command Prompt for VS 2022": `cmake --preset ninja-x64`, then `cmake --build --preset ninja-release` (or `ninja-debug`, `ninja-retail`).
- `Retail` is `Release` plus `ID_RETAIL`.
- The language is **C++23**. The root `CMakeLists.txt` sets `CMAKE_CXX_STANDARD 23` and pins `/std:c++23preview` (CMake would otherwise pick `/std:c++latest`), plus `/Zc:__cplusplus`. You need MSVC 19.43 (VS 2022 17.13) or newer.
- C++20 and later imply the `/permissive-` conformance mode. That means:
  - string literals are `const char[]`;
  - two-phase name lookup is enforced;
  - `requires` is a keyword;
  - `==` has reversed candidates, so comparison operators need `const` parameters.
- `doomclassic` and `timidity` keep `/Zc:strictStrings-`, because the 1993 C code passes literals as `char *` everywhere.
- Build all three configs after language-level changes. `Debug` and `Retail` compile code that `Release` doesn't (`assert` bodies, `#ifndef ID_RETAIL`).
- Engine code builds at /W4. `-DD3BFG_WARNINGS_AS_ERRORS=ON` adds /WX.
- The Bink videos play through FFmpeg (optional). `tools\ffmpeg\build-ffmpeg-x64.cmd` builds a minimal 64-bit LGPL FFmpeg 7.1 into `external\ffmpeg-x64` (gitignored; needs MSYS2 in `C:\msys64` with `pacman -S make diffutils`), then re-run the configure. CMake finds it there (`-DD3BFG_FFMPEG_ROOT=<dir>` to point elsewhere), defines `USE_FFMPEG` and copies `avformat-61.dll`, `avcodec-61.dll` and `avutil-59.dll` next to the exe. Copy them into the game install with the exe. Without FFmpeg, or without the DLLs at runtime (they are delay-loaded), the videos stay black.

## Tests

`idlibTests` is a console exe (`neo/tests/`) that links the idLib objects plus engine stubs (`idlib_test_stubs.cpp`: NULL `common`/`cvarSystem`/`fileSystem` and `Sys_Milliseconds` etc.). It covers math, MatX, LCP solvers, trace-model mass properties, SIMD-vs-generic joint routines and the global `operator new`/`delete`.

```
cmake --build --preset release --target idlibTests
ctest --test-dir build/vs2022-x64 -C Release
build/vs2022-x64/bin/Release/idlibTests.exe LCP_Symmetric   # one test (substring filter), or "all"
build/vs2022-x64/bin/Release/idlibTests.exe all noftz       # run without FTZ/DAZ
```

- Run the tests in `Release`. Some tests feed out-of-range inputs on purpose. In `Debug`, those trip `assert`, and `AssertFailed` calls `common->Warning` on the NULL stub `common`, so the test crashes.
- The runner enables FTZ/DAZ by default, like the engine, and `noftz` turns that off.
- Set `IDLIB_TESTS_VERBOSE=1` to print every failed check. By default each test prints its first 20.
- `lcp_generic.cpp` recompiles `Lcp.cpp` under renamed classes (`idLCP_*_Generic`) with the SSE macros undefined, so the solvers can be compared. Right now both copies are the same code (see SIMD below).
- `ctest` also runs `renderApiLeaks` (`tools/check-render-api-leaks.ps1`): it fails when a file outside `neo/renderer/OpenGL/`, `win_glimp.cpp` and `win_qgl.cpp` uses an OpenGL name, or a file outside `neo/renderer/D3D12/` and `neo/tests/d3d12_*` a Direct3D 12, DXGI or D3DCompiler name. `precompiled.h` includes neither API's headers, so their types don't compile there either.
- `ctest` also runs `d3d12Tests` (see Direct3D 12 below).
- `ctest` also runs `editorModules` (`tools/check-editor-modules.ps1`): it fails when a file of `editor/ui/` includes an engine or bridge header, one of `editor/bridge/` a Qt header, or one of `editor/api/` and `editor/mtr/` either. `renderApiLeaks` scans `editor/` as well.
- There is no lint step and no other test suite. Game code is checked by running the game and the visual demos (below).

## Architecture

**Single executable.** `neo/CMakeLists.txt` builds `idLib`, `external` (zlib, jpeg, etc.), `Game-d3xp` and `engine` (everything else but `WinMain`) as CMake **OBJECT** libraries and links them straight into `Doom3BFG.exe` with `d3bfg_link_engine`; the exe's own source is `sys/win32/win_entry.cpp`, a `WinMain` that calls `Sys_GameMain`. Static libraries would let the linker drop translation units that hold only self-registering globals (`idCVar`, `idTypeInfo`/`CLASS_DECLARATION`, `idEventDef`, `idCommandLink`), which would silently remove cvars, entity classes and commands. `doomclassic` and `timidity` (classic DOOM 1/2) are ordinary static libs. `amplitude` is a separate tool exe.

**Source lists are explicit.** New `.cpp`/`.h` files must be added to `neo/sources.cmake` (`IDLIB_SOURCES`, `EXTERNAL_SOURCES`, `GAME_D3XP_SOURCES`, `ENGINE_SOURCES`, `DOOMEXE_SOURCES`, `AMPLITUDE_SOURCES`) or `doomclassic/sources.cmake`. Engine files use the precompiled header `neo/idlib/precompiled.h`.

**Layout of `neo/`:**
- `idlib/`: the base library (math, containers, strings, the `idSIMD` processor, the parallel job system).
- `framework/`: common, cvars, the console, the file system, and the main frame loop (`common_frame.cpp`).
- `renderer/`: the frontend, the backend interface `RenderBackend.h` (its contract is the comment at its top) and two backends: `OpenGL/` (the default; it converts `base/renderprogs/*.vertex` and `*.pixel` to GLSL, kept in `renderprogs/glsl/`) and `D3D12/` (below).
- `d3xp/`: game logic (entities, AI, physics, scripts), compiled into the same exe rather than as a game DLL.
- `cm/`: the collision model.
- `aas/`: bot/AI navigation.
- `sound/`: XAudio2.
- `swf/`: the Flash-based UI.
- `ui/`: legacy GUIs.
- `sys/win32/`: the platform layer (the window in `win_window.cpp`, the WGL context in `win_glimp.cpp`, input, the wndproc, the sampling profiler `win_profiler.cpp`).

**Threading and frame loop.**
- With `com_smp 1` (the default), `idGameThread` (`framework/Common_local.h`) runs game logic and the renderer frontend, while the main thread runs the render backend for the previous frame.
- Game tics are fixed-rate at `com_engineHz` (default 60). The frame loop runs a catch-up of several tics per frame when it falls behind, so one slow tic snowballs into visible hitches.
- Parallel work goes through `idParallelJobList` (`idlib/ParallelJobList.h`).
- `common->Printf` from non-main threads is queued and flushed by the main thread.

**SIMD paths are disabled.** `neo/idlib/sys/sys_defines.h` doesn't define `ID_WIN_X86_SSE*_INTRIN`, so idLib's `#ifdef ID_WIN_X86_SSE*_INTRIN` branches (LCP, MatX, etc.) compile to their generic C++ fallbacks. The shipped game used SSE, so these generic paths are barely tested and have hidden real bugs. For example, a wrong unrolled loop in `DotProduct_SIMD` in `idlib/math/Lcp.cpp` produced NaN ragdolls, and NaN bounds then made every collision query test the whole world. `idSIMD_SSE` in `Simd_SSE.cpp` still uses intrinsics unconditionally.

**Physics notes.**
- `idPhysics_AF` (`d3xp/physics/Physics_AF.cpp`) builds row-padded `idMatX` systems with `MATX_ALLOCA`, whose padding is uninitialized, and solves them with `idLCP`.
- `idLCP::Solve` leaves the matrix columns permuted, so callers must not reuse the matrix.
- `af_checkInvalid 1` (the default) reports the first solver stage that produced a non-finite AF state.

## Direct3D 12

- `+set r_renderApi d3d12` runs the renderer on `neo/renderer/D3D12/`; OpenGL stays the default. `r_renderApi` is read once at startup and archived, so `set r_renderApi d3d12` in the console takes effect at the next start. It needs a GPU with feature level 11_0. `d3d12.dll`, `dxgi.dll` and `d3dcompiler_47.dll` are loaded from System32 at its Init, never linked, so an OpenGL run never loads the HLSL compiler.
- The backend emits HLSL from the render program sources `base/renderprogs/*.vertex` and `*.pixel` and compiles it with FXC, so copy `base/renderprogs` into the game install with the exe (`run-vdemos.ps1 -Api d3d12` does).
- Its caches live in `fs_savepath/base/renderprogs/dxbc`: the DXBC of every shader (`r_d3d12ShaderCache 1`), the PSO keys of earlier runs (`pipelines.journal`, made again when their program is created at Init or a level load) and the driver's pipeline library (`pipelines.bin`, `r_d3d12PipelineLibrary 1`). Delete the folder for a cold start; a stale or broken file is rebuilt on its own.
- `r_debugContext` (read at Init): 1 = the debug layer, whose messages appear in the log as `d3d12 debug: <SEVERITY> ...` (it needs the Windows optional feature Graphics Tools); 2 = plus GPU-based validation, several times slower (give the demo scripts `-TimeoutScale 6`); 3 = plus DRED, which prints where the GPU stopped when the device is lost.
- Cvars: `r_d3d12Adapter` (empty = the high-performance GPU, `warp`, or an index), `r_d3d12Stats 1` (draws, PSOs, uploads, submits, waits and GPU ms once a second, and a `com_showFPS` line), `r_d3d12SubmitDraws` (256), `r_d3d12FrameBuffers` (0: the per-frame vertex cache stays in UPLOAD heaps), `r_d3d12ShowImage <texture>`, `r_d3d12DumpHLSL 1`, `r_d3d12CaptureBackBuffer 1`, `r_d3d12CheckImages 1`. Commands: `d3d12_compileAll`, `d3d12_checkImages`, `d3d12_removeDevice`. `r_pix` works on both APIs.
- `d3d12Tests` (`neo/tests/d3d12_tests.cpp`, in ctest) runs the backend's parts without a window, on a WARP device with the debug layer (the ones a driver could get wrong also on the GPU), and the HLSL emitter and FXC over every render program against `neo/tests/d3d12_golden` (`D3D12_TESTS_WRITE_GOLDEN=1` rewrites them). `build/vs2022-x64/bin/Release/d3d12Tests.exe <filter>` runs the tests whose names contain the filter, `all` every test.
- The visual gate is `tools\vdemo\compare-apis.ps1 -Stage <name> -Demos <list> [-Gbv] [-Layer <layer>]`. Two D3D12 runs, with and without the debug layer (and with `-Gbv` a third under GPU-based validation), must give byte-identical frames and no layer message. `analyze.py` then holds the frames to the bars of `tools/vdemo/bars-d3d12.json` against the OpenGL reference (`-Baseline`, by default `research/vdemo/gl-ref`, a local OpenGL run of the same demos; `research/vdemo/d3d12-ref` keeps the D3D12 frames of the last gate). Run `_x4_gui` with `-Layer clear`, and `_x7_heathaze` in a call of its own. A lone 2x2 or 4x2 brighter block in one frame of one run is a known AMD driver flake: run the pair again, it must not repeat.
- Direct3D 12 has to look no worse than OpenGL, not the same: driver filtering differences, compiler last-bit noise and thin edge lines are fine. Judge a bar miss by eye on the heat maps and contact sheets that analyze.py writes. OpenGL's own frames stay byte-identical when shared code changes.
- Names stay in their folder: Direct3D 12, DXGI and D3DCompiler names only in `neo/renderer/D3D12/` and `neo/tests/d3d12_*`, OpenGL names never there (renderApiLeaks). Shared code calls `renderBackend` only.

## Editor

- `editor/` holds the level and material editor, in one process and two modules. `Doom3Editor.exe` is the same engine objects as the game (`d3bfg_link_engine`) plus `editor/bridge/`, the engine-side half. `editor/ui/` is the Qt 6 interface, built as `bin/<Config>/editor/Doom3EditorUI.dll` with the Qt DLLs next to it; the exe loads it at start. Sources are listed in `editor/sources.cmake`.
- The two halves share only `editor/api/EditorEngineApi.h`: pure interfaces, plain data and `const char *`, no ownership across it. No Qt code may share a module with the engine (its `operator new`, the `Str.h` macros, the static CRT), so `editor/ui/` includes no `neo/` header and `editor/bridge/` no Qt header.
- Qt is not in the repo: `tools\qt\install-qt.ps1` downloads a kit into `external\qt-x64` (gitignored; `-DD3BFG_QT_ROOT=<kit>` points elsewhere), then re-run the configure. Without Qt the UI target is skipped and only `--bare` works. Copy the `editor` folder into the game install with the exe.
- The engine is embedded through `idEngineHost` (`neo/framework/EngineHost.h`). `engineHost` is `nullptr` in the game, so every branch on it is dead there. A host owns the top-level window and the message loop and drives `Sys_EngineInit`, `Sys_EngineFrame` and `Sys_EngineShutdown` (`sys/win32/win_entry.h`); the engine window is a child of the host's, with no intro, legal splash, main menu, gamma ramp or single-instance check. It is active (keys, mouse grab, sound) while it has the keyboard focus: a click gives it, Shift+Esc gives it back.
- Editing is not a running game. `editor/bridge/EditorWorld` holds the `idMapFile` as it is on disk (`Parse` with `asWritten`) and a render world of its own: the map's `.bproc` plus a render entity or light per map entity, made by `idGameEdit::ParseSpawnArgsToRenderEntity/RenderLight`. The host draws it through `idEngineHost::DrawScene()` while no game map runs. `EditorView` is the fly camera; in edit mode it takes the engine window's mouse and key messages, so the engine never becomes active.
- Editor console commands: `editorOpenMap <map>`, `editorCloseMap`, and `editorCheckMaps [filter]`, which reads, writes and reads again every retail map and compares the geometry CRC, entities and keys (`Doom3Editor.exe +editorCheckMaps +quit`; it must report 0 failed, or saved maps would lose their `.cm`/`.aas`).
- The map file is the document: an edit replaces an entity's key/values (`idEditorWorld::SetMapArgs`) and is one step of `idEditorHistory`. `editorSelect <entity>`, `editorSetKey <entity> <key> [value]`, `editorUndo`, `editorRedo` and `editorSaveMap` do from the console what the viewport and the menus do, so edits can be tested without a mouse.
- An entity keeps its index while the map is open: a removed one stays in the list without key/values (undo brings it back) and `idMapFile::Write` skips it. `editorCreate <entityDef>`, `editorDuplicate` and `editorDelete` work on the selection. The editor mounts every map's resource file (`idFileSystem::MountAllMapResources`), so media of any map can be used in any other.
- `editorSaveMap` writes `maps/<name>.map` into `fs_savepath`, never into the game install. The editor sets `fs_resourceLoadPriority 0` so the loose file wins over the resource files; the game needs the same cvar to load a saved map. Moving a brush entity changes only its `origin` key.
- Preview pictures (`idEditorEngine::RequestPreview`, `editor/bridge/EditorPreview`) are frames nobody sees: a world of one model and two lights is drawn into a corner of the output (`CropRenderSize`; a cropped `RenderScene` fills the crop and skips the post process), read back with `idRenderSystem::ReadLastFrame( ..., discard )` and never shown (`presentMode_t::NONE`), so the screen keeps the frame before. One picture a frame, every other frame while a map is open. `editorPreviewTest material|model|class <name> [size]` writes one to `editor_preview.tga` in `fs_savepath`.
- The Content Browser (`editor/ui/ContentBrowser`, `ThumbnailCache`) lists materials, models, entity classes, sounds and maps (`idEditorEngine::EnumAssets`; the retail models exist only as `generated/rendermodels/*.b*` in the resource files, `idFileSystem::ListResourceFiles`). Its pictures are previews, kept as PNG files in the user's cache folder; only what is seen is asked for.
- Images loaded for a preview carry `idImageManager`'s load mark and are purged every 64 pictures, so memory stays flat over the whole game data; a purged image loads again when something binds it. `editorPreviewSweep material|model|class [first] [count]` makes every picture and throws it away (the memory and crash test).
- An asset can stop its frame with an engine error (a material whose program does not exist): the preview is dropped (`OnPreviewReady` with width 0) and the next one waits 250 ms, because several errors within 100 ms become a fatal one. A material with a deform is shown on a quad: a tube deform on other geometry is a `common->Error` on a job thread, which ends the process.
- The material editor (`editor/ui/MaterialEditor`) edits one declaration as text and as a list of keys at once. `editor/mtr/MtrText.h` (header only, neither Qt nor the engine, used by both halves) parses the text into lines and blocks that only point into it, so an edit replaces the bytes of one value and comments, spacing and line ends survive. Apply is `idEditorEngine::ApplyMaterialText` (`SetText`, `Invalidate`, `EnsureNotPurged`: live, no file); Save is `idDecl::ReplaceSourceFileText`, which now writes the `.mtr` into `fs_savepath`.
- `editorCheckMaterials` parses every retail material and writes every value back (`Doom3Editor.exe +editorCheckMaterials +quit`; it must report 0 failed). `editorSetMaterialKey <material> <key> <value> [save]` and `editorEditMaterial <material>` do from the console what the panel does.
- Play in Editor (`editorPlay`, F5) saves the map when it has changes and runs `devmap` on it in the viewport; `editorStop`, Shift+Esc (`idEngineHost::OnEscapeToHost`) or the game's own exit ends it. While it plays, `idEditorWorld::Suspend` has freed the editor's render world and the preview's and kept the document, the history, the selection and the camera; `Resume` builds the world again. Both worlds at once do not work: a level load purges the models and images the other holds, and the map's geometry does not fit the static vertex cache twice (so the game's world is emptied after a stop). In a host `idCommonLocal::LeaveGame` makes no main menu.
- A loose `maps/<name>.proc` (one a map compiler wrote) decides what the render world loads: a `.bproc` with another timestamp is made again, also in production mode. `generated/*.bproc`, `.cm` and `.bcm` are written into `fs_savepath`. With `fs_resourceLoadPriority 0` `idFileSystem::GetTimestamp` answers with a loose file's own time (it was 0 for every name the resource files have). `renderWorldInfo` prints where the game's world was read from and what it holds.
- `editor/compilers/dmap/` is the map compiler of the original Doom 3 GPL release (the OBJECT library `MapCompilers`, linked into the game and the editor by `d3bfg_link_engine`, /W3 for now). `dmap <map>` writes `maps/<map>.proc` and `.cm` into `fs_savepath`, or a `.lin` when the map leaks; it refuses to run while a game map runs. `testmap <map>` compiles and loads. It works on float vertices of its own (`dmapVert_t`, `dmapPatch_t`): `idDrawVert` is packed. Not ported: the OpenGL debug views, the precomputed shadow volumes and light carving (the engine makes the shadows itself, 4-10 % of a frame, `research/perf.md`), AAS (monsters do not navigate a new map yet).
- A map without a resource file of its own gets every map's resource file mounted (`idFileSystemLocal::BeginLevelLoad`, and at the start of `dmap`): an image that is not found once stays the default one. The test maps are `base/maps/editor/box1.map`, `box2.map` (2 areas, 1 portal) and `leak.map`; two `dmap` runs must give the same bytes.
- Without a map the editor draws an empty viewport itself; left to the engine, it would show the game selection screen.
- `renderEntity_t`, `renderLight_t` and `renderView_t` are zeroed with `memset`, never `= {}`: `idVec3` and `idMat3` have empty constructors of their own, so `= {}` leaves them unset (a light with a garbage `lightCenter` lights nothing).
- The editor always runs with `com_smp 0`: nothing of a frame is pending between two frames, so the bridge may change the world, the decls and the render world there.
- `Doom3Editor.exe --bare +devmap <map>` is a plain Win32 frame around the viewport with the game's own input, for testing the hosting on both render APIs. It also runs the visual demos (`+vdemo <name> quit`): its Direct3D 12 frames are byte-identical to the game's, its OpenGL frames differ in thin filtering details because the GPU driver has a profile for the name `Doom3BFG.exe`.

## Code conventions that bite

- **Write modern C++, standard C++23.** New and changed code uses the current language: `nullptr`, `constexpr`, `auto`, range-for, lambdas, `enum class`, `std::unique_ptr`/move semantics, `override`, `[[nodiscard]]`, `std::span`/`std::string_view`, and so on. Don't add things deprecated or removed since C++17:
  - `register`;
  - `std::auto_ptr`;
  - arithmetic or comparisons between different enums, or between an enum and a float;
  - `==` on arrays;
  - literals into `char *`.
- The existing code is C++03-era id style. Modernize what you touch, and keep it compiling in all three configs.
- **Breaking changes are fine.** There is no public API: the engine, the game code and the tools build into executables from this repo, with no mods or outside callers to stay compatible with. Change interfaces (`idGame`, `idRenderWorld`, `idCommon`, ...) and update every caller. Rewrite or refactor bad code instead of working around it. Only the retail game data (maps, defs, scripts, GUIs) has to keep loading.
- **Comments: at most 3 lines, compact and to the point.** A comment says what the code can't: why, a constraint, a trap. Don't repeat the code or state the obvious. Exceptions: diagrams and architecture documentation.
- `idlib/Str.h` macro-redirects the CRT string compares. `strcmp` silently becomes `idStr::Cmp`, while `_stricmp`, `strcasecmp`, `strnicmp`, `_memicmp` and similar become undefined `use_idStr_*` tokens and fail to compile. Call `idStr::Cmp`, `idStr::Icmp` or `idStr::Icmpn` directly.
- Source files use CRLF line endings. Keep them CRLF when editing with scripts, because `sed -i` and heredoc appends introduce LF.
- `idlib/Heap.cpp` replaces every global `operator new`/`delete` form (sized, nothrow, `std::align_val_t`) once, out of line, plus the tagged `new ( TAG_X ) T`. All of them, `Mem_Alloc` and `Mem_ClearedAlloc` use `_aligned_malloc`, so that memory must never reach the CRT's `free()`, and `malloc` memory must never reach `delete` or `Mem_Free`. Don't define `operator new`/`delete` inline anywhere (C4595). `new` throws `std::bad_alloc` and never returns NULL.
- Settings live in cvars (`idCVar` globals with flags such as `CVAR_ARCHIVE` or `CVAR_SYSTEM`). Externs for game cvars go in `d3xp/gamesys/SysCvar.h`.

## Running and diagnosing

- Game data is not in the repo. You need a retail DOOM 3 BFG install, and the built exe is copied into it. `testing_1/` … `testing_5/` are gitignored local installs. `testing_1/Doom3BFG_perf.bat` launches with logging and the perf cvars below.
- The repo's `base/` holds `default.cfg`, `renderprogs/` and the visual demos `vdemos/`.
- Visual demos are scripted scenes on the retail maps (`base/vdemos/*.vdemo`), stepped by a virtual 240 fps clock, so their frames repeat exactly from run to run. They are for regression checks:
  - `tools\vdemo\run-vdemos.ps1 -OutDir <dir> [-Demos 01_camera,...]` deploys the Release exe and the demos to `testing_1`, runs them and collects the frames;
  - `tools/vdemo/analyze.py <run> --baseline <run>` compares two runs (SSIM, repeats, pops);
  - in the game, `vdemo <name>|all [quit]` runs them, `vdemo_capture 0` skips saving the frames and `vdemo_realtime 1` plays them at real speed for watching (`testing_1/vdemo_<name>.bat` and `vdemo_all.bat` start them that way).
- With `+set logFile 1`, the console log is written to `%USERPROFILE%\Saved Games\id Software\DOOM 3 BFG\base\qconsole.log`. The log survives a game crash.
- `logFile 2` also calls `FlushFileBuffers` after every print. That costs milliseconds per line, so it skews perf measurements. Use it only when Windows itself crashes or hangs.
- Useful diagnostics:
  - `com_showFPS 1`: the perf overlay. `com_showFPSScale` sets its size.
  - `com_speeds 1`.
  - `g_frametime 1` and `g_timeEntities <ms>`: per-entity think timing.
  - `r_showPrimitives 1`.
  - `r_pix 1`: GPU and CPU ms per render pass (the `renderLog.OpenMainBlock( MRB_* )` blocks), printed about once a second from GL timestamp queries read back a few frames late, so nothing stalls. It also adds the top 3 GPU passes to `com_showFPS`. `r_pix 2` wraps the passes and render log blocks in `KHR_debug` groups for RenderDoc, Radeon GPU Profiler and Nsight, and `r_pix 3` does both.
  - `cm_traceStats 1|2`: per-second collision query cost by model, and with 2 also the slowest queries.
  - `sys_profile 1` together with `sys_profileTop`, and the commands `profileStart`, `profileStop` and `profileReport`: the game-thread stack sampling profiler. It resolves symbols via dbghelp, which needs the PDB next to the exe.
  - `testVideo <video/file.bik>` (cheat) plays a video over the 3D view; `exportVideo <video/file.bik>` writes its frames as TGA files to `fs_savepath`. The startup log says whether the FFmpeg DLLs were found ("Bink videos: FFmpeg ...").

## Not included (from README.txt)

- Steam integration, Bink video and the patented "depth fail" stencil shadows (Carmack's Reverse) have been removed.
- These third-party pieces are under their own licenses: jpeg-6 (`neo/renderer/jpeg-6`), zlib (`third_party/zlib`, a submodule) and Base64 (`neo/idlib/Base64.cpp`).
