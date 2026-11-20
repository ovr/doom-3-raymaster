# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
<#
.SYNOPSIS
Fails when one half of the editor includes a header of the other half's world.

.DESCRIPTION
The editor is two modules in one process. Doom3Editor.exe is the engine plus editor/bridge/;
Doom3EditorUI.dll is Qt plus editor/ui/. They must not share a translation unit: the engine
replaces the global operator new, redirects the CRT string compares with macros and links the
static CRT, Qt wants the DLL runtime. So

  - editor/ui/ includes no engine header (neo/, idlib/, framework/, renderer/, sys/, d3xp/,
    precompiled.h) and nothing of editor/bridge/,
  - editor/bridge/ includes no Qt header and nothing of editor/ui/,
  - editor/api/ and editor/mtr/, which both halves include, include neither.

Exit code: 0 when every #include is on its side, 1 otherwise. ctest runs it as editorModules.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-editor-modules.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..' ) ).Path
$editorRoot = Join-Path $repoRoot 'editor'

$include = New-Object regex '(?m)^[ \t]*#[ \t]*include[ \t]*[<"]([^>"\n]+)[>"]'
$comment = New-Object regex '//[^\n]*|/\*[\s\S]*?\*/'
$engine = '(^|/)(neo|idlib|framework|renderer|sys|d3xp|cm|aas|sound|swf|ui)/|precompiled\.h$|(^|/)bridge/|^Editor(Engine|EngineLocal|World|View|Tool|History|Preview|Brush)\.h$'
$qt = '^Q[A-Z]\w*$|^Qt\w*/|(^|/)q\w+\.h$|\.\./ui/'
$rules = @(
	@{ Dir = 'ui'; Forbidden = $engine; What = 'an engine or bridge header' },
	@{ Dir = 'bridge'; Forbidden = $qt; What = 'a Qt or UI header' },
	@{ Dir = 'api'; Forbidden = "$engine|$qt"; What = 'an engine or Qt header' },
	@{ Dir = 'mtr'; Forbidden = "$engine|$qt"; What = 'an engine or Qt header' }
)

$bad = 0
$checked = 0
foreach ( $rule in $rules ) {
	$dir = Join-Path $editorRoot $rule.Dir
	if ( -not ( Test-Path $dir ) ) { continue }
	$files = Get-ChildItem $dir -Recurse -File | Where-Object { '.cpp', '.h', '.inl' -contains $_.Extension.ToLowerInvariant() }
	foreach ( $file in ( $files | Sort-Object FullName ) ) {
		$checked++
		$text = $comment.Replace( [System.IO.File]::ReadAllText( $file.FullName ), ' ' )
		foreach ( $match in $include.Matches( $text ) ) {
			$path = $match.Groups[1].Value.Replace( '\', '/' )
			# the two shared folders are everybody's
			if ( $path -match '(^|/)(api|mtr)/' ) { continue }
			if ( $path -cmatch $rule.Forbidden ) {
				Write-Host "editor/$( $rule.Dir )/$( $file.Name ) includes $( $rule.What ): $path"
				$bad++
			}
		}
	}
}

if ( $bad -eq 0 ) {
	Write-Host "$checked editor files: every #include is on its side"
	exit 0
}
Write-Host "$bad #include lines cross the editor's module boundary (editor/api/EditorEngineApi.h is the only shared interface)"
exit 1
