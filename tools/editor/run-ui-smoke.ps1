# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.

# Runs the editor's UI regression script (ui-smoke.txt) with the build directory's editor on a game
# install, in a save path of its own, and prints the checks. Exit code 0: every check passed.
[CmdletBinding()]
param(
	[string] $Game = '',
	[string] $Config = 'Release',
	[string] $Script = '',
	[int] $TimeoutSec = 900
)

$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$root = ( Resolve-Path ( Join-Path $here '..\..' ) ).Path
if ( -not $Game ) { $Game = Join-Path $root 'testing_1' }
if ( -not $Script ) { $Script = Join-Path $here 'ui-smoke.txt' }
$exe = Join-Path $root "build\vs2022-x64\bin\$Config\Doom3Editor.exe"
$game = ( Resolve-Path $Game ).Path
$save = Join-Path $root 'build\ui-smoke-save'
$base = Join-Path $save 'base'

# a map left by the run before would make New Map ask
foreach ( $dir in 'maps', 'generated' ) {
	$path = Join-Path $base $dir
	if ( Test-Path $path ) { Remove-Item $path -Recurse -Force }
}
New-Item -ItemType Directory -Force -Path $base | Out-Null
# the map with a patch: the game install may not have it yet, the save path is searched first
New-Item -ItemType Directory -Force -Path ( Join-Path $base 'maps\editor' ) | Out-Null
Copy-Item ( Join-Path $root 'base\maps\editor\patch1.map' ) ( Join-Path $base 'maps\editor\patch1.map' ) -Force
$scriptCopy = Join-Path $save ( Split-Path $Script -Leaf )
Copy-Item $Script $scriptCopy -Force
$log = "$scriptCopy.log"
if ( Test-Path $log ) { Remove-Item $log -Force }

$argList = @( '--ui-script', "`"$scriptCopy`"",
	'+set', 'fs_basepath', "`"$( $game -replace '\\', '/' )`"", '+set', 'fs_savepath', "`"$( $save -replace '\\', '/' )`"",
	'+set', 'logFile', '1', '+set', 's_noSound', '1', '+set', 'fs_resourceLoadPriority', '0' )
$process = Start-Process -FilePath $exe -ArgumentList $argList -WorkingDirectory $game -PassThru
if ( -not $process.WaitForExit( $TimeoutSec * 1000 ) ) {
	Stop-Process -Id $process.Id -Force
	Write-Host "TIMEOUT after $TimeoutSec s, the editor was stopped"
}

# the retail map the build saved must not shadow the game's own
$loose = Join-Path $base 'maps\game'
if ( Test-Path $loose ) { Remove-Item $loose -Recurse -Force }

if ( -not ( Test-Path $log ) ) {
	Write-Host 'no log: the script did not start'
	exit 2
}
$lines = Get-Content $log
$lines | Where-Object { $_ -match '^FAIL' } | ForEach-Object { Write-Host $_ }
$summary = $lines | Where-Object { $_ -match '^ui-script: (\d+) passed, (\d+) failed' } | Select-Object -Last 1
if ( -not $summary ) {
	Write-Host "the script did not finish; its last line: $( $lines | Select-Object -Last 1 )"
	exit 2
}
Write-Host $summary
if ( $summary -match ', 0 failed' ) { exit 0 }
exit 1
