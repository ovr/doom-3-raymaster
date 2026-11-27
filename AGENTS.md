# AGENTS.md

Guidance for AI coding agents (Claude Code, Codex, etc.) in this repository. `CLAUDE.md` is a symlink to this file. Read `AGENTS_LOCAL.md` in the repository root as well: it holds local, machine-specific instructions and is not committed (git-ignored).

DOOM 3 BFG Edition (id Tech 4, GPL) ported to a CMake build on modern Windows. Only 64-bit MSVC (x64) builds are supported. You need VS2022 or its Build Tools, the Windows 10 SDK, and XAudio2 2.9 (Windows 10+). The legacy DirectX SDK is not used.

This file holds what applies to every change. Before you change one of these areas, read its document:
- `docs/d3d12.md`: the Direct3D 12 backend, its render programs, caches, descriptors, render graph, cvars, `d3d12Tests` and the visual gate.
- `docs/hdr.md`: the HDR scene, its colours, the resolve, eye adaptation, bloom and local exposure.
- `docs/ray-tracing.md`: the ray traced passes (R14-R20): G-buffer, GPU scene, acceleration structures, traced shadows and their history, indirect light, reflections, debug views.
- `docs/editor.md`: the editor's modules, hosting, document, previews, material editor, Play in Editor, brushes, and the map and AAS compilers.

Keep them current: a change of what they describe changes their text in the same commit.

## Build

```
cmake --preset vs2022-x64            # configure (VS 2022 generator, x64)
cmake --build --preset release       # also: debug, retail
```

- The build needs CMake 3.30 or newer (NRD's own `CMakeLists.txt` asks for it).
- zlib is a git submodule in `third_party/zlib` (upstream madler/zlib at a release tag), compiled into the `external` target from its `.c` files. After cloning run `git submodule update --init`, or configure stops with that hint.
- NVIDIA NRD 4.18.0, the denoiser of the ray traced passes, is a submodule in `third_party/NRD`, with the MathLib (v11) and ShaderMake it pins in `third_party/MathLib` and `third_party/ShaderMake`.
  - `third_party/CMakeLists.txt` points NRD's FetchContent at those submodules, so the configure needs no network. It builds NRD as a static library whose DXIL ShaderMake compiles with the `dxc.exe` of `external\d3d12-x64`.
  - `D3BFG_NRD` (ON when the submodule is there) links it into the exe and the tests and defines `USE_NRD`; `-DD3BFG_NRD=OFF` builds without it.
  - NRD is under the NVIDIA RTX SDKs License, not the GPL: an exe built with it can't be passed on as GPL software (README.txt).
  - The configure copies `NRD.hlsli` and its generated `NRDConfig.hlsli` into `base/shaders/include/nrd/` (gitignored), from where the render programs include them at run time.
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
- The Direct3D 12 backend needs DXC, the Agility SDK and WARP: `tools\d3d12\install-d3d12-sdk.ps1` downloads them at pinned versions (SHA-256 and Microsoft signatures checked) into `external\d3d12-x64` (gitignored; `-DD3BFG_D3D12_SDK_ROOT=<dir>` points elsewhere), and the configure stops without them. The build copies `dxcompiler.dll` and `dxil.dll` next to the exe and `D3D12Core.dll` and `d3d12SDKLayers.dll` into `D3D12\` beside it: copy both into the game install with the exe (`run-vdemos.ps1` does). `d3d10warp.dll` (WARP with feature level 12_2) goes only next to `d3d12Tests.exe`: its license allows testing, not shipping it.
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
- `ctest` also runs `renderApiLeaks` (`tools/check-render-api-leaks.ps1`): it fails when any file uses an OpenGL name (the OpenGL backend is removed), or a file outside `neo/renderer/D3D12/` and `neo/tests/d3d12_*` a Direct3D 12, DXGI or DXC name. `precompiled.h` includes no API header, so their types don't compile there either. GL's conventions keep their names (`GLS_*`, `GLMatrix`, `GLClipToD3D`).
- `ctest` also runs `d3d12Tests` (see Direct3D 12 below).
- `ctest` also runs `editorModules` (`tools/check-editor-modules.ps1`): it fails when a file of `editor/ui/` includes an engine or bridge header, one of `editor/bridge/` a Qt header, or one of `editor/api/` and `editor/mtr/` either. `renderApiLeaks` scans `editor/` as well.
- There is no lint step and no other test suite. Game code is checked by running the game and the visual demos (below).

## Architecture

**Single executable.** `neo/CMakeLists.txt` builds `idLib`, `external` (zlib, jpeg, etc.), `Game-d3xp` and `engine` (everything else but `WinMain`) as CMake **OBJECT** libraries and links them straight into `Doom3BFG.exe` with `d3bfg_link_engine`; the exe's own source is `sys/win32/win_entry.cpp`, a `WinMain` that calls `Sys_GameMain`. Static libraries would let the linker drop translation units that hold only self-registering globals (`idCVar`, `idTypeInfo`/`CLASS_DECLARATION`, `idEventDef`, `idCommandLink`), which would silently remove cvars, entity classes and commands. `doomclassic` and `timidity` (classic DOOM 1/2) are ordinary static libs. `amplitude` is a separate tool exe.

**Source lists are explicit.** New `.cpp`/`.h` files must be added to `neo/sources.cmake` (`IDLIB_SOURCES`, `EXTERNAL_SOURCES`, `GAME_D3XP_SOURCES`, `ENGINE_SOURCES`, `DOOMEXE_SOURCES`, `AMPLITUDE_SOURCES`) or `doomclassic/sources.cmake`. Engine files use the precompiled header `neo/idlib/precompiled.h`.

**Layout of `neo/`:**
- `idlib/`: the base library (math, containers, strings, the `idSIMD` processor, the parallel job system).
- `framework/`: common, cvars, the console, the file system, and the main frame loop (`common_frame.cpp`).
- `renderer/`: the frontend, the backend interface `RenderBackend.h` (its contract is the comment at its top) and its one backend, `D3D12/` (below). The OpenGL backend is removed; `git show d647250:neo/renderer/OpenGL/<file>` has its last state.
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
- An engine error inside a job (`idParallelJobList::InJob()`) only throws its message; the thread that waits for the list makes a fatal error of it (`... (in a job)`). Unwinding a job thread would end the process without a word.

**SIMD paths are disabled.** `neo/idlib/sys/sys_defines.h` doesn't define `ID_WIN_X86_SSE*_INTRIN`, so idLib's `#ifdef ID_WIN_X86_SSE*_INTRIN` branches (LCP, MatX, etc.) compile to their generic C++ fallbacks. The shipped game used SSE, so these generic paths are barely tested and have hidden real bugs. For example, a wrong unrolled loop in `DotProduct_SIMD` in `idlib/math/Lcp.cpp` produced NaN ragdolls, and NaN bounds then made every collision query test the whole world. `idSIMD_SSE` in `Simd_SSE.cpp` still uses intrinsics unconditionally.

**Physics notes.**
- `idPhysics_AF` (`d3xp/physics/Physics_AF.cpp`) builds row-padded `idMatX` systems with `MATX_ALLOCA`, whose padding is uninitialized, and solves them with `idLCP`.
- `idLCP::Solve` leaves the matrix columns permuted, so callers must not reuse the matrix.
- `af_checkInvalid 1` (the default) reports the first solver stage that produced a non-finite AF state.

## Direct3D 12

`neo/renderer/RenderBackend.h` is the backend's contract (the comment at its top); `docs/d3d12.md`, `docs/hdr.md` and `docs/ray-tracing.md` hold the detail.

- The only renderer is Direct3D 12 (`neo/renderer/D3D12/`, made by `R_CreateRenderBackend`). It needs a DX12 Ultimate GPU (feature level 12_2, shader model 6.6, resource binding tier 3); on another one Init fails and names what the GPU has.
- Windows' `d3d12.dll` and `dxgi.dll` are loaded at Init only from System32, never linked and never from the exe's folder; the DLLs that ship with the exe (FFmpeg, DXC's `dxcompiler.dll` and `dxil.dll`) load only from its folder. The runtime is the Agility SDK's `D3D12\D3D12Core.dll` when it is newer than the system's.
- Ask the device through `D3D12_CheckFeature`: the Agility debug layer warns at every plain `CheckFeatureSupport` (#989).
- The render programs are HLSL 2021 in `base/shaders`, compiled by DXC at run time: `<name>` is `shaders/<name>.hlsl` with `VSMain` (`vs_6_6`) and `PSMain` (`ps_6_6`), `<name>_skinned` the same file with `SKINNED=1`. The backend's own passes are `shaders/internal/*.hlsl` (compute stages `CSMain`, `cs_6_6`). The programs are the table `renderer/RenderProgTable.cpp`. Copy `base/shaders` into the game install with the exe.
- A stage's parms are the `float4` members of its cbuffer (b0 VS, b1 PS) named like `renderParm_t`, in that order; b2 is the joint block, b3 the bindless slots of `include/bindless.hlsli`, b4 only in programs that trace. Everything is read through `ResourceDescriptorHeap`/`SamplerDescriptorHeap`: a `register( t# )`, `s#` or `u#` binding is refused. Only NRD's own compiled DXIL binds registers, under root signatures of its own (`d3d12_Nrd.h`).
- Ports of id's programs are one to one: the same math in the same order, with GL's conventions (`include/common.hlsli`). `Hlsl_ProgramInterfaces` holds every program to `neo/tests/d3d12_golden/renderProgInfo.txt` (a wanted change of an interface is a change of its line); `Hlsl_SourcesCompile` allows no DXC warning.
- Shaders compile on worker threads: code there must not call `va` (shared buffers) or `fileSystem`.
- Caches: the DXIL in `fs_savepath/base/shaders/cache`, the PSOs in `fs_savepath/base/renderprogs/dxbc`. Delete both folders for a cold start; a stale or broken file is rebuilt on its own.
- A descriptor slot that a submitted command list may read is never written again: a replaced resource gets a new slot, the old one goes through the graveyard.
- The HDR scene (`r_hdr 1`) and ray tracing (`r_rayTracing 1`, Settings > System > Ray Tracing) are on by default. `r_rayTracing 0` gives the stencil frames of before, `r_hdr 0` the LDR frames, which the OpenGL reference holds. Their passes read no cvar: shared code hands them their settings (`R_DefaultPostProcessSettings`, `R_DefaultRayTracingSettings`).
- Frames repeat byte for byte from run to run. GPU results are read once their fence has passed (`WaitedFence()`), never by polling. The scripts that write data (`tools/rt/*.py`: maps, textures, the blue noise) give the same bytes on every run: change the script, never its output.
- A WRL `ComPtr`'s `&` releases what it holds: take its address with `std::addressof`.
- `r_debugContext` (read at Init): 1 the debug layer (`d3d12 debug: ...` lines in the log), 2 plus GPU-based validation (give the demo scripts `-TimeoutScale 6`), 3 plus DRED. `r_d3d12Stats 1` prints draws, PSOs, uploads, submits and GPU ms once a second.
- `d3d12Tests` (`neo/tests/d3d12_tests.cpp`, in ctest) runs the backend's parts without a window, on WARP with the debug layer; the `Gpu_` twins of the `Warp_` tests run on the GPU. `build/vs2022-x64/bin/Release/d3d12Tests.exe <filter>` runs the tests whose names contain the filter, `all` every test.
- The visual gate is `tools\vdemo\compare-apis.ps1 -Stage <name> -Demos <list> [-Gbv] [-Hdr] [-Warmup <demo>] [-ExtraArgs <cvars>] [-Baseline <run>]`. Two runs, with and without the debug layer, must give byte-identical frames and no layer message; `analyze.py` then holds the frames to `tools/vdemo/bars-d3d12.json`. Without `-Hdr` the runs get `r_hdr 0` and the baseline is `research/vdemo/gl-ref`, the last OpenGL run, which cannot be made again: keep it. Read `docs/d3d12.md` before you run it or judge a failure: some demos need flags of their own, and it lists the known noise (AMD's lone 2x2 and 4x2 blocks, `window: client`, `profile save failed`).
- Direct3D 12 has to look no worse than OpenGL, not the same: driver filtering differences, compiler last-bit noise and thin edge lines are fine. Judge a bar miss by eye on the heat maps and contact sheets that `analyze.py` writes.
- Names stay in their folder: Direct3D 12, DXGI and DXC names only in `neo/renderer/D3D12/` and `neo/tests/d3d12_*`, OpenGL names nowhere (`renderApiLeaks`). Shared code calls `renderBackend` only.

## Editor

`docs/editor.md` holds the detail.

- `editor/` is the level and material editor: `Doom3Editor.exe` (the game's engine objects plus `editor/bridge/`) loads the Qt 6 interface `editor/Doom3EditorUI.dll` (`editor/ui/`). Sources are listed in `editor/sources.cmake`. Qt is not in the repo: `tools\qt\install-qt.ps1` downloads a kit, then re-run the configure; without it only `--bare` works. Copy the `editor` folder into the game install with the exe.
- The two halves share only `editor/api/EditorEngineApi.h`: pure interfaces, plain data and `const char *`, no ownership across it. No Qt code may share a module with the engine (its `operator new`, the `Str.h` macros, the static CRT), so `editor/ui/` includes no `neo/` header and `editor/bridge/` no Qt header (`editorModules`).
- The engine is embedded through `idEngineHost` (`neo/framework/EngineHost.h`); `engineHost` is `nullptr` in the game, so every branch on it is dead there. The editor runs with `com_smp 0`: between two frames the bridge may change the world, the decls and the render world.
- Every action of the viewport and the menus has a console command (`editorSelect`, `editorSetKey`, `editorCreateBrush`, `editorBuild`, ...), so edits can be tested without a mouse. Scripted input proves no real input, though: check a new mouse feature once with the real mouse, with the editor in front.
- What the editor writes (maps, `.proc`, `.cm`, AAS files, materials, generated files) goes into `fs_savepath`, never into the game install. The game needs `fs_resourceLoadPriority 0` to load it.
- After a change they cover, these must report 0 failed: `Doom3Editor.exe +editorCheckMaps +quit`, `Doom3Editor.exe +editorCheckMaterials +quit`, `Doom3BFG.exe +set fs_resourceLoadPriority 0 +dmapCheck +quit` (about 11 minutes) and `tools\editor\run-ui-smoke.ps1`. `aasCheck` has one known miss (`game/erebus6.aas96`).
- A change of the map or AAS compiler must not change its output: compile `editor/box2` and a few retail maps (`dmapCheck <map> keep`) before and after and compare the bytes.

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
- `renderEntity_t`, `renderLight_t` and `renderView_t` are zeroed with `memset`, never `= {}`: `idVec3` and `idMat3` have empty constructors of their own, so `= {}` leaves them unset (a light with a garbage `lightCenter` lights nothing).
- Settings live in cvars (`idCVar` globals with flags such as `CVAR_ARCHIVE` or `CVAR_SYSTEM`). Externs for game cvars go in `d3xp/gamesys/SysCvar.h`.

## Running and diagnosing

- Game data is not in the repo. You need a retail DOOM 3 BFG install, and the built exe is copied into it. `testing_1/` … `testing_5/` are gitignored local installs. `testing_1/Doom3BFG_perf.bat` launches with logging and the perf cvars below.
- The repo's `base/` holds `default.cfg`, `shaders/` (the HLSL of Direct3D 12), the visual demos `vdemos/` and our own test maps `maps/rt/` with their materials (`materials/rt_lab.mtr`) and textures (`textures/rt/`).
- `tools/rt/make_gallery.py`, `make_lightlab.py` and `make_lab_textures.py` write those maps, materials and textures, the same bytes on every run: change the script, never its output. `rt/gallery` (demo 20) is built of retail materials; `rt/lightlab` (demos 27-30: shadows, bounce, emissive, glossy) of materials whose albedo, specular and emission are known numbers, in closed rooms that share no wall. A diffuse map is read as YCoCg: a flat colour is `textures/rt/white.tga` times a stage colour, never `_white` (a little yellow there) or `_black` (blue).
- Visual demos are scripted scenes on the retail maps (`base/vdemos/*.vdemo`), stepped by a virtual 240 fps clock, so their frames repeat exactly from run to run. They are for regression checks:
  - `tools\vdemo\run-vdemos.ps1 -OutDir <dir> [-Demos 01_camera,...]` deploys the Release exe and the demos to `testing_1`, runs them and collects the frames;
  - `tools/vdemo/analyze.py <run> --baseline <run>` compares two runs (SSIM, repeats, pops);
  - in the game, `vdemo <name>|all [quit]` runs them, `vdemo_capture 0` skips saving the frames and `vdemo_realtime 1` plays them at real speed for watching (`testing_1/vdemo_<name>.bat` and `vdemo_all.bat` start them that way).
- A model that is not found is never written to `generated/`: with `fs_resourceLoadPriority 0` its loose stub would hide the real binary of a resource file from then on. A "Model ... defaulted" for a retail model means such stubs (about 1.4 KB each) lie in `base/generated` of the install: delete them. Generated models go to `fs_savepath`. A stub also changes the game: 03_physics spawns `moveable_cartonbox1`, whose `tbox1.ase` the retail data lacks, and a stub `tbox1.bcmodel` gives it another collision, so the scene drifts from tic 155. An exe of an older commit writes such stubs again.
- With `+set logFile 1`, the console log is written to `%USERPROFILE%\Saved Games\id Software\DOOM 3 BFG\base\qconsole.log`. The log survives a game crash.
- `logFile 2` also calls `FlushFileBuffers` after every print. That costs milliseconds per line, so it skews perf measurements. Use it only when Windows itself crashes or hangs.
- Useful diagnostics:
  - `com_showFPS 1`: the perf overlay. `com_showFPSScale` sets its size.
  - `com_speeds 1`.
  - `g_frametime 1` and `g_timeEntities <ms>`: per-entity think timing.
  - `r_showPrimitives 1`.
  - `r_pix 1`: GPU and CPU ms per render pass (the `renderLog.OpenMainBlock( MRB_* )` blocks), printed about once a second from GPU timestamps read back a few frames late, so nothing stalls. It also adds the top 3 GPU passes to `com_showFPS`. `r_pix 2` wraps the passes and render log blocks in debug groups (PIX events) for PIX, RenderDoc, Radeon GPU Profiler and Nsight, and `r_pix 3` does both.
  - `cm_traceStats 1|2`: per-second collision query cost by model, and with 2 also the slowest queries.
  - `sys_profile 1` together with `sys_profileTop`, and the commands `profileStart`, `profileStop` and `profileReport`: the game-thread stack sampling profiler. It resolves symbols via dbghelp, which needs the PDB next to the exe.
  - `testVideo <video/file.bik>` (cheat) plays a video over the 3D view; `exportVideo <video/file.bik>` writes its frames as TGA files to `fs_savepath`. The startup log says whether the FFmpeg DLLs were found ("Bink videos: FFmpeg ...").

## Not included (from README.txt)

- Steam integration, Bink video and the patented "depth fail" stencil shadows (Carmack's Reverse) have been removed.
- These third-party pieces are under their own licenses: jpeg-6 (`neo/renderer/jpeg-6`), zlib (`third_party/zlib`, a submodule), Base64 (`neo/idlib/Base64.cpp`), and the optional NRD (`third_party/NRD`, NVIDIA RTX SDKs License) with MathLib and ShaderMake (MIT, submodules).
