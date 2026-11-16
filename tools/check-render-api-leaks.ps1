<#
.SYNOPSIS
Counts the OpenGL names that shared code still uses, per file, and holds them to a budget.

.DESCRIPTION
OpenGL belongs to neo/renderer/OpenGL/ and to the WGL part of the platform layer
(neo/sys/win32/win_glimp.cpp, win_qgl.cpp). Every other .cpp, .h and .inl file under neo/ is
shared code: there the script counts, outside comments and string literals,

  - qgl* and qwgl* calls and pointers, and gl*( / wgl*( calls,
  - GL_* constants and GL* types (GLuint, GLenum, ...),
  - #include lines of qgl.h, glext.h, wglext.h and gl/gl.h.

The budget file (tools/render-api-leaks.budget.json) holds the count allowed per file. While the
renderer moves behind idRenderBackend the counts may only fall: the script fails when a file
has more names than its budget or when a file without a budget has any.

Exit code: 0 when every file is within its budget (or no budget file exists), 1 otherwise.

.PARAMETER Update
Writes the counts that fell into the budget file (and creates the file when it is missing).
A count never rises this way: a file over its budget keeps the old number and still fails.

.EXAMPLE
powershell -NoProfile -File tools\check-render-api-leaks.ps1
powershell -NoProfile -File tools\check-render-api-leaks.ps1 -Update
#>
[CmdletBinding()]
param(
	[switch] $Update,
	[string] $Budget
)

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..' ) ).Path
$neoRoot = Join-Path $repoRoot 'neo'
if ( -not $Budget ) { $Budget = Join-Path $PSScriptRoot 'render-api-leaks.budget.json' }

$allowedDirs = @( 'neo/renderer/OpenGL/' )
$allowedFiles = @( 'neo/sys/win32/win_glimp.cpp', 'neo/sys/win32/win_qgl.cpp' )

# case matters: GL_State and glConfig are the engine's own names
$none = [System.Text.RegularExpressions.RegexOptions]::None
$comment = '//[^\n]*|/\*[\s\S]*?\*/'
$literal = '"(?:\\.|[^"\\\n])*"|''(?:\\.|[^''\\\n])*'''
$commentOrLiteral = New-Object regex ( "$comment|$literal" ), $none
$literalOnly = New-Object regex $literal, $none
$includes = New-Object regex '(?im)^[ \t]*#[ \t]*include[ \t]*[<"][^>"\n]*\b(qgl\.h|glext\.h|wglext\.h|gl/gl\.h)[>"]', $none
$names = @(
	( New-Object regex '\bqw?gl[A-Z]\w*', $none ),
	( New-Object regex '\bw?gl[A-Z]\w*\s*\(', $none ),
	( New-Object regex '\bGL_[A-Z0-9_]+\b', $none ),
	( New-Object regex '\bGL(u?int|enum|sizei\w*|intptr\w*|float|double|boolean|bitfield|u?byte|u?short|char\w*|void|sync|handleARB|u?int64)\b', $none )
)
$dropComments = [System.Text.RegularExpressions.MatchEvaluator] {
	param( $match )
	if ( $match.Value[0] -eq '/' ) { ' ' } else { $match.Value }
}

$latin1 = [System.Text.Encoding]::GetEncoding( 28591 )
$counts = [ordered] @{}
$files = Get-ChildItem $neoRoot -Recurse -File | Where-Object { '.cpp', '.h', '.inl' -contains $_.Extension.ToLowerInvariant() }
foreach ( $file in ( $files | Sort-Object FullName ) ) {
	$relative = 'neo/' + $file.FullName.Substring( $neoRoot.Length + 1 ).Replace( '\', '/' )
	if ( $allowedFiles -contains $relative ) { continue }
	if ( $allowedDirs | Where-Object { $relative.StartsWith( $_, [System.StringComparison]::OrdinalIgnoreCase ) } ) { continue }

	$text = [System.IO.File]::ReadAllText( $file.FullName, $latin1 )
	if ( $text.IndexOf( 'gl', [System.StringComparison]::OrdinalIgnoreCase ) -lt 0 ) { continue }
	# the includes are counted before the string literals go, the names after
	$text = $commentOrLiteral.Replace( $text, $dropComments )
	$count = $includes.Matches( $text ).Count
	$text = $literalOnly.Replace( $text, '""' )
	foreach ( $name in $names ) {
		$count += $name.Matches( $text ).Count
	}
	if ( $count -gt 0 ) { $counts[$relative] = $count }
}

$allowed = $null
if ( Test-Path $Budget ) {
	$allowed = @{}
	$json = Get-Content $Budget -Raw | ConvertFrom-Json
	foreach ( $property in $json.files.PSObject.Properties ) {
		$allowed[$property.Name] = [int] $property.Value
	}
}

$rows = @()
$over = @()
$total = 0
$totalAllowed = 0
foreach ( $relative in $counts.Keys ) {
	$count = $counts[$relative]
	$total += $count
	$row = [ordered] @{ names = $count }
	if ( $null -ne $allowed ) {
		if ( $allowed.ContainsKey( $relative ) ) {
			$row.budget = $allowed[$relative]
			if ( $count -gt $allowed[$relative] ) { $over += "$relative has $count GL names, its budget is $($allowed[$relative])" }
		} else {
			$row.budget = 'none'
			$over += "$relative has $count GL names and no budget (a new file with GL in it)"
		}
	}
	$row.file = $relative
	$rows += New-Object psobject -Property $row
}
if ( $null -ne $allowed ) {
	foreach ( $value in $allowed.Values ) { $totalAllowed += $value }
}

$rows | Format-Table -AutoSize | Out-String -Width 200 | Write-Host
if ( $null -ne $allowed ) {
	Write-Host "$total GL names in $( $counts.Count ) shared files, the budget is $totalAllowed in $( $allowed.Count ) files"
} else {
	Write-Host "$total GL names in $( $counts.Count ) shared files, no budget file"
}

if ( $Update ) {
	# a fallen count replaces the budget, a risen one does not
	$lines = @()
	foreach ( $relative in $counts.Keys ) {
		$value = $counts[$relative]
		if ( $null -ne $allowed ) {
			if ( -not $allowed.ContainsKey( $relative ) ) { continue }
			$value = [Math]::Min( $value, $allowed[$relative] )
		}
		$lines += "    `"$relative`": $value"
	}
	$text = "{`r`n  `"files`": {`r`n" + ( $lines -join ",`r`n" ) + "`r`n  }`r`n}`r`n"
	[System.IO.File]::WriteAllText( $Budget, $text, ( New-Object System.Text.UTF8Encoding $false ) )
	Write-Host "budget written to $Budget"
}

if ( $over.Count -gt 0 ) {
	$over | ForEach-Object { Write-Host "OVER BUDGET: $_" }
	exit 1
}
exit 0
