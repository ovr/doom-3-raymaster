<#
.SYNOPSIS
Runs the visual demos (base/vdemos/*.vdemo) in a retail install and collects their frames.

.DESCRIPTION
Deploys Doom3BFG.exe (+ pdb, + FFmpeg DLLs) from a build dir and base/vdemos into the game dir,
starts the game in a 1280x720 window with an isolated fs_savepath (the real config in Saved Games
is never read or written), runs "vdemo <demos> quit" and copies the frames, the per-demo
vdemo.json files, run.json and qconsole.log into -OutDir.

Refuses to run while any Doom3BFG.exe runs. Exit code: 0 when every demo ended "ok", 1 when one
failed, 2 when the game timed out or crashed.

.EXAMPLE
tools\vdemo\run-vdemos.ps1 -OutDir research\vdemo\baseline
tools\vdemo\run-vdemos.ps1 -OutDir research\vdemo\run2 -Demos 01_camera,04_lighting -NoDeploy
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $OutDir,
	[string[]] $Demos = @( 'all' ),
	[string] $BuildDir,
	[string] $GameDir,
	[string] $SavePath,
	[switch] $NoDeploy,
	[string[]] $ExtraArgs = @(),
	[int] $TimeoutMinutes = 40
)

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..\..' ) ).Path

# a worktree has no testing_1, the main checkout does
function Get-MainCheckout {
	$common = & git -C $repoRoot rev-parse --path-format=absolute --git-common-dir 2>$null
	if ( $LASTEXITCODE -eq 0 -and $common ) {
		return ( Split-Path -Parent $common )
	}
	return $repoRoot
}

if ( -not $BuildDir ) { $BuildDir = Join-Path $repoRoot 'build\vs2022-win32\bin\Release' }
if ( -not $GameDir ) { $GameDir = Join-Path ( Get-MainCheckout ) 'testing_1' }
if ( -not $SavePath ) { $SavePath = Join-Path $repoRoot 'build\vdemo-save' }
if ( -not [System.IO.Path]::IsPathRooted( $OutDir ) ) { $OutDir = Join-Path ( Get-Location ) $OutDir }
$OutDir = [System.IO.Path]::GetFullPath( $OutDir )

if ( Get-Process -Name 'Doom3BFG' -ErrorAction SilentlyContinue ) {
	Write-Error 'Doom3BFG.exe is running; close it first (this script never stops a game it did not start).'
	exit 2
}
$exe = Join-Path $GameDir 'Doom3BFG.exe'
if ( -not ( Test-Path ( Join-Path $GameDir 'base\_common.resources' ) ) ) {
	Write-Error "$GameDir is not a DOOM 3 BFG install (no base\_common.resources)."
	exit 2
}

if ( -not $NoDeploy ) {
	foreach ( $file in 'Doom3BFG.exe', 'Doom3BFG.pdb', 'avformat-61.dll', 'avcodec-61.dll', 'avutil-59.dll' ) {
		$source = Join-Path $BuildDir $file
		if ( Test-Path $source ) {
			Copy-Item $source $GameDir -Force
		} elseif ( $file -eq 'Doom3BFG.exe' ) {
			Write-Error "no Doom3BFG.exe in $BuildDir"
			exit 2
		}
	}
	$vdemoTarget = Join-Path $GameDir 'base\vdemos'
	New-Item -ItemType Directory -Force $vdemoTarget | Out-Null
	Get-ChildItem $vdemoTarget -Filter '*.vdemo' | Remove-Item -Force
	Copy-Item ( Join-Path $repoRoot 'base\vdemos\*.vdemo' ) $vdemoTarget -Force
	Write-Host "deployed $BuildDir and base\vdemos to $GameDir"
}

# the save path is ours only: a marker file guards against wiping anything else
$marker = Join-Path $SavePath '.vdemo-save'
if ( Test-Path $SavePath ) {
	if ( -not ( Test-Path $marker ) ) {
		Write-Error "$SavePath exists but was not made by this script (no .vdemo-save marker); pick another -SavePath."
		exit 2
	}
	Remove-Item -Recurse -Force $SavePath
}
New-Item -ItemType Directory -Force $SavePath | Out-Null
New-Item -ItemType File -Force $marker | Out-Null

$savePathArg = $SavePath -replace '\\', '/'
$gameArgs = @(
	'+set', 'fs_savepath', "`"$savePathArg`"",
	'+set', 'logFile', '1',
	'+set', 'r_fullscreen', '0',
	'+set', 'r_windowWidth', '1280',
	'+set', 'r_windowHeight', '720',
	'+set', 'r_windowX', '40',
	'+set', 'r_windowY', '40',
	'+set', 's_noSound', '1',
	'+set', 'com_skipStartupVideos', '1',
	'+set', 'in_mouse', '0'
) + $ExtraArgs + @( '+vdemo' ) + $Demos + @( 'quit' )

Write-Host "starting $exe $($gameArgs -join ' ')"
$started = Get-Date
$process = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $GameDir -PassThru
$exitCode = 0
if ( -not $process.WaitForExit( $TimeoutMinutes * 60 * 1000 ) ) {
	Write-Warning "the game did not finish in $TimeoutMinutes minutes, stopping it"
	Stop-Process -Id $process.Id -Force
	$exitCode = 2
}
$elapsed = ( Get-Date ) - $started
Write-Host ( 'game exited after {0:n0} s' -f $elapsed.TotalSeconds )

$frames = Join-Path $SavePath 'base\vdemos'
New-Item -ItemType Directory -Force $OutDir | Out-Null
if ( Test-Path $frames ) {
	Get-ChildItem $frames -Directory | ForEach-Object {
		$target = Join-Path $OutDir $_.Name
		if ( Test-Path $target ) { Remove-Item -Recurse -Force $target }
		Copy-Item $_.FullName $target -Recurse
	}
	if ( Test-Path ( Join-Path $frames 'run.json' ) ) {
		Copy-Item ( Join-Path $frames 'run.json' ) $OutDir -Force
	}
}
$log = Join-Path $SavePath 'base\qconsole.log'
if ( Test-Path $log ) { Copy-Item $log ( Join-Path $OutDir 'qconsole.log' ) -Force }

$runJson = Join-Path $OutDir 'run.json'
if ( -not ( Test-Path $runJson ) ) {
	Write-Warning 'no run.json: the run did not finish (see qconsole.log)'
	exit 2
}
$run = Get-Content $runJson -Raw | ConvertFrom-Json
$run.demos | Format-Table demo, map, status, tics, frames, loadAttempts, loadMsec, runMsec -AutoSize | Out-String | Write-Host
if ( $exitCode -eq 0 -and ( $run.demos | Where-Object { $_.status -ne 'ok' } ) ) {
	$exitCode = 1
}
Write-Host "frames in $OutDir"
exit $exitCode
