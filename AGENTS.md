# AGENTS.md

This file provides guidance to AI coding agents (Claude Code, Codex, etc.) when working with code in this repository. `CLAUDE.md` is a symlink to this file.

`AGENTS_LOCAL.md` exists in the repository root, read it as well: it holds local, machine-specific instructions and is not committed (git-ignored).

DOOM 3 BFG Edition (id Tech 4, GPL) ported to a CMake build on modern Windows. Only 32-bit MSVC (Win32/x86) builds are supported. You need VS2022 or its Build Tools, the Windows 10 SDK, and XAudio2 2.9 (Windows 10+). The legacy DirectX SDK is not used.

## Build

```
cmake --preset vs2022-win32          # configure (VS 2022 generator, Win32)
cmake --build --preset release       # also: debug, retail
```

- zlib is a git submodule in `third_party/zlib` (upstream madler/zlib at a release tag), compiled into the `external` target from its `.c` files. After cloning run `git submodule update --init`, or configure stops with that hint.
- The output is `build\vs2022-win32\bin\<Config>\Doom3BFG.exe`. The generated `.sln` in `build\vs2022-win32` can also be opened in Visual Studio.
- For Ninja, run these from an "x86 Native Tools Command Prompt for VS 2022": `cmake --preset ninja-x86`, then `cmake --build --preset ninja-release` (or `ninja-debug`, `ninja-retail`).
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
- The Bink videos play through FFmpeg (optional). `tools\ffmpeg\build-ffmpeg-win32.cmd` builds a minimal 32-bit LGPL FFmpeg 7.1 into `external\ffmpeg-win32` (gitignored; needs MSYS2 in `C:\msys64` with `pacman -S make diffutils`), then re-run the configure. CMake finds it there (`-DD3BFG_FFMPEG_ROOT=<dir>` to point elsewhere), defines `USE_FFMPEG` and copies `avformat-61.dll`, `avcodec-61.dll` and `avutil-59.dll` next to the exe. Copy them into the game install with the exe. Without FFmpeg, or without the DLLs at runtime (they are delay-loaded), the videos stay black.

## Tests

`idlibTests` is a console exe (`neo/tests/`) that links the idLib objects plus engine stubs (`idlib_test_stubs.cpp`: NULL `common`/`cvarSystem`/`fileSystem` and `Sys_Milliseconds` etc.). It covers math, MatX, LCP solvers, trace-model mass properties, SIMD-vs-generic joint routines and the global `operator new`/`delete`.

```
cmake --build --preset release --target idlibTests
ctest --test-dir build/vs2022-win32 -C Release
build/vs2022-win32/bin/Release/idlibTests.exe LCP_Symmetric   # one test (substring filter), or "all"
build/vs2022-win32/bin/Release/idlibTests.exe all noftz       # run without FTZ/DAZ
```

- Run the tests in `Release`. Some tests feed out-of-range inputs on purpose. In `Debug`, those trip `assert`, and `AssertFailed` calls `common->Warning` on the NULL stub `common`, so the test crashes.
- The runner enables FTZ/DAZ by default, like the engine, and `noftz` turns that off.
- Set `IDLIB_TESTS_VERBOSE=1` to print every failed check. By default each test prints its first 20.
- `lcp_generic.cpp` recompiles `Lcp.cpp` under renamed classes (`idLCP_*_Generic`) with the SSE macros undefined, so the solvers can be compared. Right now both copies are the same code (see SIMD below).
- `ctest` also runs `renderApiLeaks` (`tools/check-render-api-leaks.ps1`): it fails when a file outside `neo/renderer/OpenGL/`, `win_glimp.cpp` and `win_qgl.cpp` uses an OpenGL name. `precompiled.h` does not include `qgl.h`, so GL types don't compile there either.
- There is no lint step and no other test suite. Game code is checked by running the game and the visual demos (below).

## Architecture

**Single executable.** `neo/CMakeLists.txt` builds `idLib`, `external` (zlib, jpeg, etc.) and `Game-d3xp` as CMake **OBJECT** libraries and links them straight into `Doom3BFG.exe`. Static libraries would let the linker drop translation units that hold only self-registering globals (`idCVar`, `idTypeInfo`/`CLASS_DECLARATION`, `idEventDef`, `idCommandLink`), which would silently remove cvars, entity classes and commands. `doomclassic` and `timidity` (classic DOOM 1/2) are ordinary static libs. `amplitude` is a separate tool exe.

**Source lists are explicit.** New `.cpp`/`.h` files must be added to `neo/sources.cmake` (`IDLIB_SOURCES`, `EXTERNAL_SOURCES`, `GAME_D3XP_SOURCES`, `DOOMEXE_SOURCES`, `AMPLITUDE_SOURCES`) or `doomclassic/sources.cmake`. Engine files use the precompiled header `neo/idlib/precompiled.h`.

**Layout of `neo/`:**
- `idlib/`: the base library (math, containers, strings, the `idSIMD` processor, the parallel job system).
- `framework/`: common, cvars, the console, the file system, and the main frame loop (`common_frame.cpp`).
- `renderer/`: the frontend and an OpenGL backend. Shaders are loaded at runtime from `base/renderprogs/gl/`.
- `d3xp/`: game logic (entities, AI, physics, scripts), compiled into the same exe rather than as a game DLL.
- `cm/`: the collision model.
- `aas/`: bot/AI navigation.
- `sound/`: XAudio2.
- `swf/`: the Flash-based UI.
- `ui/`: legacy GUIs.
- `sys/win32/`: the platform layer (window/GL creation in `win_glimp.cpp`, input, the wndproc, the sampling profiler `win_profiler.cpp`).

**Threading and frame loop.**
- With `com_smp 1` (the default), `idGameThread` (`framework/Common_local.h`) runs game logic and the renderer frontend, while the main thread runs the render backend for the previous frame.
- Game tics are fixed-rate at `com_engineHz` (default 60). The frame loop runs a catch-up of several tics per frame when it falls behind, so one slow tic snowballs into visible hitches.
- Parallel work goes through `idParallelJobList` (`idlib/ParallelJobList.h`).
- `common->Printf` from non-main threads is queued and flushed by the main thread.

**SIMD paths are disabled.** The `ID_WIN_X86_SSE*`/`ID_WIN_X86_MMX*` defines are commented out in `neo/idlib/sys/sys_defines.h`, so idLib's `#ifdef ID_WIN_X86_SSE*_INTRIN` branches (LCP, MatX, etc.) compile to their generic C++ fallbacks. The shipped game used SSE, so these generic paths are barely tested and have hidden real bugs. For example, a wrong unrolled loop in `DotProduct_SIMD` in `idlib/math/Lcp.cpp` produced NaN ragdolls, and NaN bounds then made every collision query test the whole world. `idSIMD_SSE` in `Simd_SSE.cpp` still uses intrinsics unconditionally.

**Physics notes.**
- `idPhysics_AF` (`d3xp/physics/Physics_AF.cpp`) builds row-padded `idMatX` systems with `MATX_ALLOCA`, whose padding is uninitialized, and solves them with `idLCP`.
- `idLCP::Solve` leaves the matrix columns permuted, so callers must not reuse the matrix.
- `af_checkInvalid 1` (the default) reports the first solver stage that produced a non-finite AF state.

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
