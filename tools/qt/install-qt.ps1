# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
<#
.SYNOPSIS
Downloads the prebuilt Qt 6 (MSVC 2022, x64) the editor's user interface is built with.

.DESCRIPTION
Qt comes from the official Qt mirrors through aqtinstall, which runs in its own Python virtual
environment (build\qt-venv), so nothing is installed into the system Python. Only the archives the
editor needs are fetched (qtbase with windeployqt, qtsvg for the icons), release and debug DLLs.

The result is <OutDir>\<Version>\msvc2022_64, by default under external\qt-x64 (git-ignored), where
CMake looks (D3BFG_QT_ROOT points elsewhere). Run the configure again afterwards. Without Qt the
build skips the Doom3EditorUI target and says so.

Qt is used under the LGPL v3 and linked dynamically: the DLLs are copied next to the editor.

.EXAMPLE
tools\qt\install-qt.ps1
tools\qt\install-qt.ps1 -Version 6.8.3 -OutDir D:\Qt
#>
[CmdletBinding()]
param(
	[string] $Version = '6.8.3',
	[string] $OutDir,
	[string] $Arch = 'win64_msvc2022_64',
	[string[]] $Archives = @( 'qtbase', 'qtsvg' )
)

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..\..' ) ).Path
if ( -not $OutDir ) { $OutDir = Join-Path $repoRoot 'external\qt-x64' }
$OutDir = [System.IO.Path]::GetFullPath( $OutDir )
$kit = Join-Path $OutDir "$Version\msvc2022_64"

if ( Test-Path ( Join-Path $kit 'lib\cmake\Qt6\Qt6Config.cmake' ) ) {
	Write-Host "Qt $Version is already in $kit"
	exit 0
}

$python = Get-Command python -ErrorAction SilentlyContinue
if ( -not $python ) {
	Write-Error 'python is not on PATH: install Python 3, or unpack a Qt 6 msvc2022_64 kit yourself and set D3BFG_QT_ROOT.'
	exit 2
}

$venv = Join-Path $repoRoot 'build\qt-venv'
$venvPython = Join-Path $venv 'Scripts\python.exe'
if ( -not ( Test-Path $venvPython ) ) {
	Write-Host "creating $venv"
	& $python.Source -m venv $venv
	if ( $LASTEXITCODE -ne 0 ) { exit 2 }
}
& $venvPython -m pip install --quiet --disable-pip-version-check aqtinstall
if ( $LASTEXITCODE -ne 0 ) {
	Write-Error 'pip could not install aqtinstall'
	exit 2
}

New-Item -ItemType Directory -Force $OutDir | Out-Null
Write-Host "downloading Qt $Version (${Arch}: $( $Archives -join ', ' )) into $OutDir"
# aqtinstall writes its log into the current directory
Push-Location $OutDir
& $venvPython -m aqt install-qt windows desktop $Version $Arch --outputdir $OutDir --archives @Archives
Pop-Location
if ( $LASTEXITCODE -ne 0 -or -not ( Test-Path ( Join-Path $kit 'lib\cmake\Qt6\Qt6Config.cmake' ) ) ) {
	Write-Error "aqtinstall did not produce $kit"
	exit 2
}
Write-Host "Qt $Version is in $kit; run cmake --preset vs2022-x64 again"
