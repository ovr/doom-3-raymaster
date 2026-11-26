# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
<#
.SYNOPSIS
The Direct3D 12 gate of a stage: two D3D12 runs of the demos, their frames against each other and
against the OpenGL reference, the debug layer messages, one summary and one exit code.

.DESCRIPTION
Runs, with run-vdemos.ps1 and the shader cache kept between the runs:
  a    -DebugLevel 1 (the debug layer)          -> <OutRoot>\d3d12-<Stage>-a
  b    -DebugLevel 0                            -> <OutRoot>\d3d12-<Stage>-b
  gbv  -DebugLevel 2 -TimeoutScale 6, with -Gbv -> <OutRoot>\d3d12-<Stage>-gbv
then checks:
  - every run ended ok, exit code 0, "validation" all zeros, Direct3D 12;
  - no "window: client" line of a run's log falls into a compared demo: another program moved or
    resized the window there, and the frames from there on do not compare;
  - cmp-frames.py a b (and a gbv): byte for byte, so the layer changes nothing and the runs repeat;
  - analyze.py a against -Baseline (research/vdemo/gl-ref) with the bars of -Bars on every frame,
    5 heat maps per demo, contact sheets <OutRoot>\sheets-<Stage>\<Stage>-<demo>-sheet.png.
The summary is printed and written to <OutRoot>\d3d12-<Stage>-summary.md.

-ExtraArgs go to every run. -GameDir (the install) and -BuildDir (the binaries) go to run-vdemos.ps1,
which picks its defaults when they are not given. The OpenGL backend is gone: research\vdemo\gl-ref
holds its last frames, and research\vdemo\gl-ref-clear those of _x4_gui with +set r_clear 2 (give
the same -ExtraArgs).

The runs get "+set r_hdr 0": OpenGL had no HDR scene, so only those frames can be held to its bars.
-Hdr keeps the HDR scene and holds the frames to research\vdemo\d3d12-hdr-ref instead (made by an
earlier -Hdr gate).

-Warmup <demo> runs that demo first in every run and leaves it out of every comparison: it takes
what happens at a fixed time after the start (a window that another program resizes).

Exit code 0 when every check passed, 1 otherwise.

.EXAMPLE
tools\vdemo\compare-apis.ps1 -Stage s8 -Demos _x4_gui -ExtraArgs '+set','r_clear','2' -Baseline research\vdemo\gl-ref-clear -Gbv
tools\vdemo\compare-apis.ps1 -Stage h6 -Demos 01_camera,04_lighting -Hdr -Gbv
tools\vdemo\compare-apis.ps1 -Stage l1 -Demos 27_rt_shadows,28_rt_bounce -Hdr -Warmup 05_weapons
tools\vdemo\compare-apis.ps1 -Stage rt1 -Demos 20_rt_gallery -Hdr -GameDir testing_rt -BuildDir build\rt-demo-bin
#>
[CmdletBinding()]
param(
	[Parameter(Mandatory = $true)] [string] $Stage,
	[Parameter(Mandatory = $true)] [string[]] $Demos,
	[switch] $Gbv,
	[switch] $Hdr,
	[string] $Warmup,
	[string[]] $ExtraArgs = @(),
	[string] $Baseline,
	[string] $Bars,
	[string] $OutRoot,
	[string] $GameDir,
	[string] $BuildDir
)

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..\..' ) ).Path
if ( -not $Baseline ) { $Baseline = Join-Path $repoRoot $( if ( $Hdr ) { 'research\vdemo\d3d12-hdr-ref' } else { 'research\vdemo\gl-ref' } ) }
if ( -not $Bars ) { $Bars = Join-Path $PSScriptRoot 'bars-d3d12.json' }
if ( -not $OutRoot ) { $OutRoot = Join-Path $repoRoot 'build\vdemo' }
$Demos = @( $Demos | ForEach-Object { $_ -split ',' } | Where-Object { $_ } )
if ( $Demos.Count -gt 1 -and $Demos -contains '_x7_heathaze' ) {
	# its GL reference ran in a process of its own (see the demo)
	Write-Error '_x7_heathaze runs alone: give it a compare-apis call of its own'
	exit 1
}
if ( $Warmup -and $Demos -contains $Warmup ) {
	Write-Error "-Warmup $Warmup is one of the compared demos"
	exit 1
}
$demoList = $Demos -join ','
$runVdemos = Join-Path $PSScriptRoot 'run-vdemos.ps1'
$cmpFrames = Join-Path $PSScriptRoot 'cmp-frames.py'
$analyze = Join-Path $PSScriptRoot 'analyze.py'
$prefix = Join-Path $OutRoot "d3d12-$Stage"
if ( -not $Hdr ) {
	$ExtraArgs = @( '+set', 'r_hdr', '0' ) + $ExtraArgs
}

$results = New-Object System.Collections.Generic.List[object]
function Add-Result( [string] $check, [bool] $passed, [string] $detail ) {
	$results.Add( [pscustomobject]@{ check = $check; result = $( if ( $passed ) { 'pass' } else { 'FAIL' } ); detail = $detail } )
}

# the demos that a "window: client" line of the run's log fell into
function Get-WindowDemos( [string] $outDir ) {
	$hit = @()
	$log = Join-Path $outDir 'qconsole.log'
	if ( Test-Path $log ) {
		$demo = ''
		foreach ( $line in Get-Content $log ) {
			if ( $line -match '^vdemo (\S+): loading ' ) {
				$demo = $Matches[1]
			} elseif ( $demo -and $line -match '^window: client' -and $hit -notcontains $demo ) {
				$hit += $demo
			}
		}
	}
	return $hit
}

function Invoke-Run( [string] $name, [hashtable] $options ) {
	$outDir = "$prefix-$name"
	$options['OutDir'] = $outDir
	$options['Demos'] = $( if ( $Warmup ) { @( $Warmup ) + $Demos } else { $Demos } )
	$options['ExtraArgs'] = $ExtraArgs
	if ( $GameDir ) { $options['GameDir'] = $GameDir }
	if ( $BuildDir ) { $options['BuildDir'] = $BuildDir }
	$code = 2
	try {
		# to the host: what the script writes must not become the return value of this function
		& $runVdemos @options | Out-Host
		$code = $LASTEXITCODE
	} catch {
		Write-Warning "run $name failed: $_"
	}
	$detail = "exit $code"
	$runJson = Join-Path $outDir 'run.json'
	if ( Test-Path $runJson ) {
		$run = Get-Content $runJson -Raw | ConvertFrom-Json
		$v = $run.validation
		$failed = @( $run.demos | Where-Object { $_.status -ne 'ok' } | ForEach-Object { "$( $_.demo ) $( $_.status )" } )
		$detail = "exit $code, api $( $run.api ), game exit $( $run.exitCode ), validation errors $( $v.errors ) corruption $( $v.corruption ) warnings $( $v.warnings ) live $( $v.live )"
		if ( $failed ) { $detail += ", failed: $( $failed -join '; ' )" }
	}
	Add-Result "run $name" ( $code -eq 0 ) $detail
	$moved = @( Get-WindowDemos $outDir | Where-Object { $_ -ne $Warmup } )
	Add-Result "window of $name" ( $moved.Count -eq 0 ) $( if ( $moved ) { "moved or resized during $( $moved -join ', ' )" } else { 'untouched during the compared demos' } )
	return $outDir
}

function Invoke-Python( [string] $check, [string[]] $arguments ) {
	# Windows PowerShell turns a native stderr line into a terminating error under 'Stop'
	$ErrorActionPreference = 'Continue'
	$output = & python @arguments 2>&1
	$code = $LASTEXITCODE
	$output | ForEach-Object { Write-Host $_ }
	$last = @( $output | Where-Object { "$_".Trim() } ) | Select-Object -Last 1
	Add-Result $check ( $code -eq 0 ) "exit $code, $last"
}

$a = Invoke-Run 'a' @{ DebugLevel = 1; KeepShaderCache = $true }
$b = Invoke-Run 'b' @{ DebugLevel = 0; KeepShaderCache = $true }
Invoke-Python 'frames a = b' ( @( $cmpFrames, $a, $b ) + $Demos )

$analysisArgs = @( $analyze, $a, '--baseline', $Baseline, '--demos', $demoList, '--bars', $Bars, '--all-steps',
	'--heatmaps', '5', '--sheets', ( Join-Path $OutRoot "sheets-$Stage" ), '--sheet-prefix', "$Stage-" )
Invoke-Python 'bars of a against the baseline' $analysisArgs

if ( $Gbv ) {
	$g = Invoke-Run 'gbv' @{ DebugLevel = 2; TimeoutScale = 6; KeepShaderCache = $true }
	# the demos only: analyze.py has written its heat maps into a\analysis
	Invoke-Python 'frames a = gbv' ( @( $cmpFrames, $a, $g ) + $Demos )
}

$passed = -not ( $results | Where-Object { $_.result -ne 'pass' } )
$lines = @( "# compare-apis $Stage (d3d12$( if ( $Hdr ) { ', HDR' } )): $( if ( $passed ) { 'PASS' } else { 'FAIL' } )", '',
	"Demos: $demoList. Warmup: $( if ( $Warmup ) { $Warmup } else { 'none' } ). Baseline: $Baseline. Bars: $Bars. Extra args: $( $ExtraArgs -join ' ' ).", '',
	'| check | result | detail |', '|---|---|---|' )
$lines += $results | ForEach-Object { "| $( $_.check ) | $( $_.result ) | $( $_.detail ) |" }
$summary = "$prefix-summary.md"
[System.IO.File]::WriteAllLines( $summary, [string[]] $lines, ( New-Object System.Text.UTF8Encoding $false ) )
$lines | ForEach-Object { Write-Host $_ }
Write-Host "summary in $summary"
if ( $passed ) { exit 0 } else { exit 1 }
