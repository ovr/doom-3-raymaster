<#
.SYNOPSIS
Fails when shared code uses an OpenGL name.

.DESCRIPTION
OpenGL belongs to neo/renderer/OpenGL/ and to the WGL part of the platform layer
(neo/sys/win32/win_glimp.cpp, win_qgl.cpp). Every other .cpp, .h and .inl file under neo/ is
shared code: there the script looks, outside comments and string literals, for

  - qgl* and qwgl* calls and pointers, and gl*( / wgl*( calls,
  - GL_* constants and GL* types (GLuint, GLenum, ...),
  - #include lines of qgl.h, glext.h, wglext.h and gl/gl.h.

The precompiled header does not include qgl.h, so a GL type or a qgl call in shared code does not
compile either. The script also catches what compiles anyway: gl* and wgl* functions that
windows.h declares, an #include of a GL header, and code inside #if blocks no config builds.

Exit code: 0 when no shared file has a GL name, 1 otherwise (the names are listed per file).
ctest runs it as the test renderApiLeaks.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-render-api-leaks.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..' ) ).Path
$neoRoot = Join-Path $repoRoot 'neo'

$allowedDirs = @( 'neo/renderer/OpenGL/' )
$allowedFiles = @( 'neo/sys/win32/win_glimp.cpp', 'neo/sys/win32/win_qgl.cpp' )

# case matters: GLS_* and GLimp_* are the engine's own names
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
	$found = @( $includes.Matches( $text ) | ForEach-Object { $_.Value.Trim() } )
	$text = $literalOnly.Replace( $text, '""' )
	foreach ( $name in $names ) {
		$found += @( $name.Matches( $text ) | ForEach-Object { $_.Value.TrimEnd( '(', ' ', "`t" ) } )
	}
	if ( $found.Count -gt 0 ) { $counts[$relative] = $found }
}

if ( $counts.Count -eq 0 ) {
	Write-Host 'no GL names in shared code'
	exit 0
}
$total = 0
foreach ( $relative in $counts.Keys ) {
	$found = $counts[$relative]
	$total += $found.Count
	Write-Host "GL LEAK: $relative has $( $found.Count ) GL names: $( ( $found | Select-Object -Unique ) -join ', ' )"
}
Write-Host "$total GL names in $( $counts.Count ) shared files; GL belongs to neo/renderer/OpenGL/, shared code calls renderBackend"
exit 1
