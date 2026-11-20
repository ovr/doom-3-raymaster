<#
.SYNOPSIS
Fails when a file uses the names of a graphics API outside the folder of that API.

.DESCRIPTION
OpenGL belongs to neo/renderer/OpenGL/ and to the WGL part of the platform layer
(neo/sys/win32/win_glimp.cpp, win_qgl.cpp). Every other .cpp, .h and .inl file under neo/ and editor/ is
shared code: there the script looks, outside comments and string literals, for

  - qgl* and qwgl* calls and pointers, and gl*( / wgl*( calls,
  - GL_* constants and GL* types (GLuint, GLenum, ...),
  - #include lines of qgl.h, glext.h, wglext.h and gl/gl.h.

Direct3D 12, DXGI and the HLSL compilers (FXC, DXC) belong to neo/renderer/D3D12/ and to the tests named
neo/tests/d3d12_*. Every other file is checked for

  - ID3D12*, IDXGI* and IDxc* interfaces, D3D12_*, DXGI_*, D3D_* and DXC_* names, D3DCompile*, Dxc*,
  - #include lines of d3d12*.h, dxgi*.h, d3dcompiler.h and dxcapi.h.

The D3D12 folder itself is shared code for the OpenGL rule: it must not name OpenGL either.

The precompiled header includes neither qgl.h nor the D3D headers, so most of these names do not
compile in the wrong place. The script also catches what compiles anyway: gl* and wgl* functions
that windows.h declares, an #include of an API header, and code inside #if blocks no config builds.

Exit code: 0 when no file names an API outside its folder, 1 otherwise (the names are listed per
file). ctest runs it as the test renderApiLeaks.

.EXAMPLE
powershell -NoProfile -ExecutionPolicy Bypass -File tools\check-render-api-leaks.ps1
#>
[CmdletBinding()]
param()

$ErrorActionPreference = 'Stop'
$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..' ) ).Path
$neoRoot = Join-Path $repoRoot 'neo'

# case matters: GLS_* and GLimp_* are the engine's own names
$none = [System.Text.RegularExpressions.RegexOptions]::None
$comment = '//[^\n]*|/\*[\s\S]*?\*/'
$literal = '"(?:\\.|[^"\\\n])*"|''(?:\\.|[^''\\\n])*'''
$commentOrLiteral = New-Object regex ( "$comment|$literal" ), $none
$literalOnly = New-Object regex $literal, $none

$apis = @(
	@{
		Name = 'GL'
		Home = 'neo/renderer/OpenGL/'
		AllowedDirs = @( 'neo/renderer/OpenGL/' )
		AllowedFiles = @( 'neo/sys/win32/win_glimp.cpp', 'neo/sys/win32/win_qgl.cpp' )
		AllowedPrefixes = @()
		Hint = 'gl'
		Includes = ( New-Object regex '(?im)^[ \t]*#[ \t]*include[ \t]*[<"][^>"\n]*\b(qgl\.h|glext\.h|wglext\.h|gl/gl\.h)[>"]', $none )
		Names = @(
			( New-Object regex '\bqw?gl[A-Z]\w*', $none ),
			( New-Object regex '\bw?gl[A-Z]\w*\s*\(', $none ),
			( New-Object regex '\bGL_[A-Z0-9_]+\b', $none ),
			( New-Object regex '\bGL(u?int|enum|sizei\w*|intptr\w*|float|double|boolean|bitfield|u?byte|u?short|char\w*|void|sync|handleARB|u?int64)\b', $none )
		)
	},
	@{
		Name = 'D3D12'
		Home = 'neo/renderer/D3D12/'
		AllowedDirs = @( 'neo/renderer/D3D12/' )
		AllowedFiles = @()
		AllowedPrefixes = @( 'neo/tests/d3d12_' )
		Hint = 'd3d|dxgi|dxc'
		Includes = ( New-Object regex '(?im)^[ \t]*#[ \t]*include[ \t]*[<"][^>"\n]*\b(d3d12\w*\.h|dxgi\w*\.h|d3dcompiler\.h|dxcapi\.h)[>"]', $none )
		Names = @(
			( New-Object regex '\b(ID3D12|IDXGI|IDxc)\w+', $none ),
			( New-Object regex '\b(D3D12|DXGI|D3D|DXC)_[A-Z0-9_]+\b', $none ),
			( New-Object regex '\bD3DCompile\w*', $none ),
			( New-Object regex '\bDxc[A-Z]\w*', $none )
		)
	}
)
$dropComments = [System.Text.RegularExpressions.MatchEvaluator] {
	param( $match )
	if ( $match.Value[0] -eq '/' ) { ' ' } else { $match.Value }
}

function Test-Allowed( $api, [string] $relative ) {
	if ( $api.AllowedFiles -contains $relative ) { return $true }
	foreach ( $prefix in ( $api.AllowedDirs + $api.AllowedPrefixes ) ) {
		if ( $relative.StartsWith( $prefix, [System.StringComparison]::OrdinalIgnoreCase ) ) { return $true }
	}
	return $false
}

$latin1 = [System.Text.Encoding]::GetEncoding( 28591 )
$counts = [ordered] @{}
# the editor is shared code too: its bridge calls renderSystem and renderBackend only
$files = Get-ChildItem $neoRoot, ( Join-Path $repoRoot 'editor' ) -Recurse -File | Where-Object { '.cpp', '.h', '.inl' -contains $_.Extension.ToLowerInvariant() }
foreach ( $file in ( $files | Sort-Object FullName ) ) {
	$relative = $file.FullName.Substring( $repoRoot.Length + 1 ).Replace( '\', '/' )
	$checked = @( $apis | Where-Object { -not ( Test-Allowed $_ $relative ) } )
	if ( $checked.Count -eq 0 ) { continue }

	$raw = [System.IO.File]::ReadAllText( $file.FullName, $latin1 )
	$checked = @( $checked | Where-Object { $raw -match "(?i)$( $_.Hint )" } )
	if ( $checked.Count -eq 0 ) { continue }
	# the includes are counted before the string literals go, the names after
	$text = $commentOrLiteral.Replace( $raw, $dropComments )
	$names = $literalOnly.Replace( $text, '""' )
	foreach ( $api in $checked ) {
		$found = @( $api.Includes.Matches( $text ) | ForEach-Object { $_.Value.Trim() } )
		foreach ( $name in $api.Names ) {
			$found += @( $name.Matches( $names ) | ForEach-Object { $_.Value.TrimEnd( '(', ' ', "`t" ) } )
		}
		if ( $found.Count -gt 0 ) { $counts["$relative|$( $api.Name )"] = @{ File = $relative; Api = $api; Found = $found } }
	}
}

if ( $counts.Count -eq 0 ) {
	Write-Host 'no GL or D3D12 names outside their folders'
	exit 0
}
$total = 0
foreach ( $entry in $counts.Values ) {
	$total += $entry.Found.Count
	Write-Host "$( $entry.Api.Name ) LEAK: $( $entry.File ) has $( $entry.Found.Count ) $( $entry.Api.Name ) names: $( ( $entry.Found | Select-Object -Unique ) -join ', ' )"
}
Write-Host "$total API names in $( $counts.Count ) files outside their folders; GL belongs to neo/renderer/OpenGL/, D3D12 to neo/renderer/D3D12/, shared code calls renderBackend"
exit 1
