@echo off
rem build-ffmpeg-win32.cmd [--asm] [--prefix DIR] [--work DIR] [--jobs N]: the x86 VS 2022
rem environment, then build-ffmpeg-win32.sh in MSYS2 bash (MSYS2_ROOT, default C:\msys64,
rem with pacman -S make diffutils).
setlocal

if "%MSYS2_ROOT%"=="" set "MSYS2_ROOT=C:\msys64"
if not exist "%MSYS2_ROOT%\usr\bin\bash.exe" (
	echo error: MSYS2 bash not found in %MSYS2_ROOT%, set MSYS2_ROOT
	exit /b 1
)

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
	echo error: vswhere.exe not found, install VS 2022 or its Build Tools
	exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
if "%VSDIR%"=="" (
	echo error: no VS installation with the C++ x86/x64 tools found
	exit /b 1
)

rem vcvarsall looks vswhere up on PATH
set "PATH=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer;%PATH%"
call "%VSDIR%\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1

set MSYS2_PATH_TYPE=inherit
set MSYSTEM=MSYS
set CHERE_INVOKING=1
rem bash's dirname needs forward slashes
set "SCRIPT=%~dp0build-ffmpeg-win32.sh"
set "SCRIPT=%SCRIPT:\=/%"
"%MSYS2_ROOT%\usr\bin\bash.exe" -l "%SCRIPT%" %*
exit /b %ERRORLEVEL%
