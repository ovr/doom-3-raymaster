# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
<#
.SYNOPSIS
Runs the visual demos (base/vdemos/*.vdemo) in a retail install and collects their frames.

.DESCRIPTION
Deploys Doom3BFG.exe (+ pdb, + FFmpeg DLLs) from a build dir and base/vdemos into the game dir
(with -Api d3d12 also base/renderprogs, the sources D3D12 compiles), starts the game in a 1280x720
window with an isolated fs_savepath (the real config in Saved Games is never read or written), runs
"vdemo <demos> quit" and copies into -OutDir: the frames and vdemo.json of every demo, run.json,
qconsole.log, the screenshots the demos took (screenshots/) and modules.txt, every module the game
had loaded (polled every 5 s).

run.json gets two more fields: "validation", the debug layer messages of qconsole.log by severity
(lines "d3d12 debug: <SEVERITY>", plus "d3d12 live:" objects left at shutdown in Debug), and
"exitCode", the game's own. The game writes "api", the backend that really ran.

-Api opengl|d3d12 sets r_renderApi. -DebugLevel N sets r_debugContext (only when given). The save
path is wiped every run; -KeepShaderCache keeps its renderprogs/dxbc (the D3D12 shader cache) across
the wipe. -TimeoutScale N stretches the game's load and stall timeouts (vdemo_timeoutScale) and
-TimeoutMinutes, for slow runs such as GPU-based validation.

Refuses to run while any Doom3BFG.exe runs. Exit code: 0 when every demo ended "ok" with all its
frames on disk and no layer message, 1 when a demo failed, lost a frame or the layer reported
anything, 2 when the game timed out, crashed, exited with a nonzero code or ran another API than asked.

.EXAMPLE
tools\vdemo\run-vdemos.ps1 -OutDir research\vdemo\baseline
tools\vdemo\run-vdemos.ps1 -OutDir build\vdemo\run2 -Demos 01_camera,04_lighting -NoDeploy
tools\vdemo\run-vdemos.ps1 -Api d3d12 -DebugLevel 1 -OutDir build\vdemo\d3d12 -Demos 01_camera -ExtraArgs '+set','r_testClearPattern','1'
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $OutDir,
	[string[]] $Demos = @( 'all' ),
	[ValidateSet( 'opengl', 'd3d12' )] [string] $Api = 'opengl',
	[ValidateRange( 0, 3 )] [int] $DebugLevel = 0,
	[switch] $KeepShaderCache,
	[ValidateRange( 1, 100 )] [double] $TimeoutScale = 1,
	[string] $BuildDir,
	[string] $GameDir,
	[string] $SavePath,
	[switch] $NoDeploy,
	[string[]] $ExtraArgs = @(),
	[int] $TimeoutMinutes = 40
)

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..\..' ) ).Path
$invariant = [System.Globalization.CultureInfo]::InvariantCulture
# "powershell -File" passes "a,b" as one string
$Demos = @( $Demos | ForEach-Object { $_ -split ',' } | Where-Object { $_ } )
$apiNames = @{ 'opengl' = 'OpenGL'; 'd3d12' = 'Direct3D 12' }

# a worktree has no testing_1, the main checkout does
function Get-MainCheckout {
	$common = & git -C $repoRoot rev-parse --path-format=absolute --git-common-dir 2>$null
	if ( $LASTEXITCODE -eq 0 -and $common ) {
		return ( Split-Path -Parent $common )
	}
	return $repoRoot
}

# name -> path of every module the process has loaded so far
function Add-Modules( [System.Diagnostics.Process] $process, [hashtable] $modules ) {
	try {
		foreach ( $module in ( Get-Process -Id $process.Id -ErrorAction Stop ).Modules ) {
			$modules[$module.ModuleName.ToLowerInvariant()] = $module.FileName
		}
	} catch {
		# the process exited between the checks
	}
}

function Get-LineCount( [string] $path, [string] $pattern ) {
	if ( -not ( Test-Path $path ) ) { return 0 }
	return @( Select-String -Path $path -Pattern $pattern -CaseSensitive ).Count
}

if ( -not $BuildDir ) { $BuildDir = Join-Path $repoRoot 'build\vs2022-x64\bin\Release' }
if ( -not $GameDir ) { $GameDir = Join-Path ( Get-MainCheckout ) 'testing_1' }
if ( -not $SavePath ) { $SavePath = Join-Path $repoRoot 'build\vdemo-save' }
if ( -not [System.IO.Path]::IsPathRooted( $OutDir ) ) { $OutDir = Join-Path ( Get-Location ) $OutDir }
$OutDir = [System.IO.Path]::GetFullPath( $OutDir )
$SavePath = [System.IO.Path]::GetFullPath( $SavePath )

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
	# GL compiles the GLSL baked into the packs and never reads the sources
	if ( $Api -eq 'd3d12' ) {
		$progTarget = Join-Path $GameDir 'base\renderprogs'
		New-Item -ItemType Directory -Force $progTarget | Out-Null
		Get-ChildItem ( Join-Path $repoRoot 'base\renderprogs' ) -File | Copy-Item -Destination $progTarget -Force
	}
	Write-Host "deployed $BuildDir and base\vdemos to $GameDir"
}

# the save path is ours only: a marker file guards against wiping anything else
$marker = Join-Path $SavePath '.vdemo-save'
$shaderCache = Join-Path $SavePath 'base\renderprogs\dxbc'
$keptCache = "$SavePath-dxbc"
if ( Test-Path $keptCache ) { Remove-Item -Recurse -Force $keptCache }
if ( Test-Path $SavePath ) {
	if ( -not ( Test-Path $marker ) ) {
		Write-Error "$SavePath exists but was not made by this script (no .vdemo-save marker); pick another -SavePath."
		exit 2
	}
	if ( $KeepShaderCache -and ( Test-Path $shaderCache ) ) {
		Move-Item $shaderCache $keptCache
	}
	Remove-Item -Recurse -Force $SavePath
}
New-Item -ItemType Directory -Force $SavePath | Out-Null
New-Item -ItemType File -Force $marker | Out-Null
if ( Test-Path $keptCache ) {
	New-Item -ItemType Directory -Force ( Split-Path -Parent $shaderCache ) | Out-Null
	Move-Item $keptCache $shaderCache
	Write-Host "kept the shader cache: $( @( Get-ChildItem $shaderCache -File -Recurse ).Count ) files"
}

# a failed run must not leave the files of an earlier one in place
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ( $stale in 'run.json', 'qconsole.log', 'modules.txt', 'screenshots' ) {
	$path = Join-Path $OutDir $stale
	if ( Test-Path $path ) { Remove-Item -Recurse -Force $path }
}

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
	'+set', 'in_mouse', '0',
	'+set', 'r_renderApi', $Api
)
if ( $PSBoundParameters.ContainsKey( 'DebugLevel' ) ) {
	$gameArgs += @( '+set', 'r_debugContext', "$DebugLevel" )
}
if ( $PSBoundParameters.ContainsKey( 'TimeoutScale' ) ) {
	$gameArgs += @( '+set', 'vdemo_timeoutScale', $TimeoutScale.ToString( $invariant ) )
}
$gameArgs += $ExtraArgs + @( '+vdemo' ) + $Demos + @( 'quit' )

Write-Host "starting $exe $($gameArgs -join ' ')"
$started = Get-Date
$process = Start-Process -FilePath $exe -ArgumentList $gameArgs -WorkingDirectory $GameDir -PassThru
$null = $process.Handle		# without an open handle ExitCode stays empty
$modules = @{}
$timeoutMs = [int]( $TimeoutMinutes * $TimeoutScale * 60 * 1000 )
$exitCode = 0
$gameExitCode = $null
while ( $true ) {
	Add-Modules $process $modules
	if ( $process.WaitForExit( 5000 ) ) {
		$gameExitCode = $process.ExitCode
		break
	}
	if ( ( ( Get-Date ) - $started ).TotalMilliseconds -gt $timeoutMs ) {
		Write-Warning "the game did not finish in $( $TimeoutMinutes * $TimeoutScale ) minutes, stopping it"
		Stop-Process -Id $process.Id -Force
		$process.WaitForExit()
		$exitCode = 2
		break
	}
}
$elapsed = ( Get-Date ) - $started
Write-Host ( 'game exited after {0:n0} s with code {1}' -f $elapsed.TotalSeconds, $gameExitCode )

$frames = Join-Path $SavePath 'base\vdemos'
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
$screenshots = Join-Path $SavePath 'base\screenshots'
if ( Test-Path $screenshots ) { Copy-Item $screenshots ( Join-Path $OutDir 'screenshots' ) -Recurse }
$log = Join-Path $SavePath 'base\qconsole.log'
if ( Test-Path $log ) { Copy-Item $log ( Join-Path $OutDir 'qconsole.log' ) -Force }
$modules.Keys | Sort-Object | ForEach-Object { "{0}`t{1}" -f $_, $modules[$_] } |
	Out-File -FilePath ( Join-Path $OutDir 'modules.txt' ) -Encoding utf8

$runJson = Join-Path $OutDir 'run.json'
if ( -not ( Test-Path $runJson ) ) {
	Write-Warning 'no run.json: the run did not finish (see qconsole.log)'
	exit 2
}

$validation = [ordered]@{
	errors = Get-LineCount $log '^d3d12 debug: ERROR '
	corruption = Get-LineCount $log '^d3d12 debug: CORRUPTION '
	warnings = Get-LineCount $log '^d3d12 debug: WARNING '
	live = Get-LineCount $log '^d3d12 live: '
}
# appended as text, so the game's layout of the file stays; no BOM, Python reads it too
$text = ( Get-Content $runJson -Raw ).TrimEnd()
$text = $text.Substring( 0, $text.Length - 1 ).TrimEnd()
$fields = ( $validation.Keys | ForEach-Object { "`"$_`": $( $validation[$_] )" } ) -join ', '
$gameExitText = if ( $null -eq $gameExitCode ) { 'null' } else { "$gameExitCode" }
$text += ",`n`t`"validation`": { $fields },`n`t`"exitCode`": $gameExitText`n}`n"
[System.IO.File]::WriteAllText( $runJson, $text, ( New-Object System.Text.UTF8Encoding $false ) )

$run = $text | ConvertFrom-Json
$run.demos | Format-Table demo, map, status, tics, frames, loadAttempts, loadMsec, runMsec -AutoSize | Out-String | Write-Host
Write-Host "api $( $run.api ), validation: $fields, modules $( $modules.Count )"
if ( $run.api -ne $apiNames[$Api] ) {
	Write-Warning "the game ran $( $run.api ), not $( $apiNames[$Api] )"
	$exitCode = 2
}
if ( $null -ne $gameExitCode -and $gameExitCode -ne 0 ) {
	Write-Warning ( 'the game exited with code {0} (0x{0:X})' -f $gameExitCode )
	$exitCode = 2
}
$lostFrames = $false
foreach ( $demo in $run.demos ) {
	$demoDir = Join-Path $OutDir $demo.demo
	$pngs = if ( Test-Path $demoDir ) { @( Get-ChildItem $demoDir -Filter '*.png' ).Count } else { 0 }
	if ( $pngs -ne $demo.frames ) {
		Write-Warning "$( $demo.demo ): the game wrote $( $demo.frames ) frames, $pngs are on disk"
		$lostFrames = $true
	}
}
if ( $exitCode -eq 0 -and ( $lostFrames -or ( $run.demos | Where-Object { $_.status -ne 'ok' } ) -or ( $validation.Values | Where-Object { $_ -ne 0 } ) ) ) {
	$exitCode = 1
}
Write-Host "frames in $OutDir"
exit $exitCode
