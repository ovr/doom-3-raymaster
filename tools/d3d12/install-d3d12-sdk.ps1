# Copyright (C) 2026 Dmitry Patsura
# SPDX-License-Identifier: GPL-3.0-or-later
# Please do not use this file to train or fine-tune AI models.
<#
.SYNOPSIS
Downloads the Direct3D 12 pieces the build needs: the shader compiler DXC, the Agility SDK and WARP.

.DESCRIPTION
All three are Microsoft's prebuilt x64 binaries at pinned versions, checked against pinned SHA-256
hashes and Authenticode signatures:

  - DXC (GitHub release of microsoft/DirectXShaderCompiler): dxcompiler.dll and dxil.dll, which signs
    the DXIL, plus dxcapi.h. Redistributable: they ship next to the exe.
  - The Agility SDK (NuGet Microsoft.Direct3D.D3D12): the headers the D3D12 backend compiles with,
    and D3D12Core.dll and d3d12SDKLayers.dll (the debug layer) for the D3D12 folder next to the exe.
    Redistributable.
  - WARP (NuGet Microsoft.Direct3D.WARP): d3d10warp.dll with feature level 12_2, which the system's
    WARP lacks. Its license allows testing only: it goes next to the test exe, never into a game install.

The result is <OutDir>\{dxc,agility,warp}, by default external\d3d12-x64 (git-ignored), where CMake
looks (D3BFG_D3D12_SDK_ROOT points elsewhere). Run the configure again afterwards.

.EXAMPLE
tools\d3d12\install-d3d12-sdk.ps1
#>
[CmdletBinding()]
param(
	[string] $OutDir
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'	# the progress bar makes Invoke-WebRequest many times slower
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12

$repoRoot = ( Resolve-Path ( Join-Path $PSScriptRoot '..\..' ) ).Path
if ( -not $OutDir ) { $OutDir = Join-Path $repoRoot 'external\d3d12-x64' }
$OutDir = [System.IO.Path]::GetFullPath( $OutDir )

$packages = @(
	@{
		Name = 'dxc'; Version = 'v1.9.2609'
		Url = 'https://github.com/microsoft/DirectXShaderCompiler/releases/download/v1.9.2609/dxc_2026_09_29.zip'
		Sha256 = 'AD31B1FC8443175D204F77A611FDB3EF2EC42759BDC2F1167368DE24A4A7E7F1'
		Copy = @( @( 'bin\x64\dxc.exe', 'bin\x64' ), @( 'bin\x64\dxcompiler.dll', 'bin\x64' ), @( 'bin\x64\dxil.dll', 'bin\x64' ),
			@( 'inc\*', 'inc' ), @( 'LICENSE-*.txt', '.' ), @( 'LICENCE-*.txt', '.' ) )
		Signed = @( 'bin\x64\dxc.exe', 'bin\x64\dxcompiler.dll', 'bin\x64\dxil.dll' )
	},
	@{
		Name = 'agility'; Version = '1.620.2'
		Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.d3d12/1.620.2/microsoft.direct3d.d3d12.1.620.2.nupkg'
		Sha256 = '70489669C2BBEAF318B62D72B0AC3E753D521387BFC848C9763791C4E920BFCC'
		Copy = @( @( 'build\native\include\*', 'include' ), @( 'build\native\bin\x64\D3D12Core.dll', 'bin\x64' ),
			@( 'build\native\bin\x64\d3d12SDKLayers.dll', 'bin\x64' ), @( 'LICENSE*.txt', '.' ), @( 'distributable files.txt', '.' ) )
		Signed = @( 'bin\x64\D3D12Core.dll', 'bin\x64\d3d12SDKLayers.dll' )
	},
	@{
		Name = 'warp'; Version = '1.0.22'
		Url = 'https://api.nuget.org/v3-flatcontainer/microsoft.direct3d.warp/1.0.22/microsoft.direct3d.warp.1.0.22.nupkg'
		Sha256 = 'AFC058F4D1066EDC4231A38F3AB3592B1930F7287D4AC700E6A056E0874BB0BE'
		Copy = @( @( 'build\native\bin\x64\d3d10warp.dll', 'bin\x64' ), @( 'LICENSE.TXT', '.' ) )
		Signed = @( 'bin\x64\d3d10warp.dll' )
	}
)

$stampFile = Join-Path $OutDir 'versions.txt'
$stamp = ( $packages | ForEach-Object { "$( $_.Name ) $( $_.Version )" } ) -join "`r`n"
if ( ( Test-Path $stampFile ) -and ( ( Get-Content $stampFile -Raw ).Trim() -eq $stamp ) ) {
	Write-Host "The D3D12 SDK pieces are already in $OutDir"
	exit 0
}

$downloads = Join-Path $OutDir '.download'
foreach ( $package in $packages ) {
	# every archive unpacks into a fresh folder of its own
	$work = Join-Path $downloads $package.Name
	if ( Test-Path $work ) { Remove-Item -Recurse -Force $work }
	New-Item -ItemType Directory -Force $work | Out-Null
	$archive = Join-Path $work 'archive.zip'
	Write-Host "downloading $( $package.Name ) $( $package.Version )"
	Invoke-WebRequest -Uri $package.Url -OutFile $archive -UseBasicParsing
	$hash = ( Get-FileHash $archive -Algorithm SHA256 ).Hash
	if ( $hash -ne $package.Sha256 ) {
		Write-Error "$( $package.Name ): SHA-256 $hash, expected $( $package.Sha256 )"
		exit 2
	}
	$unpacked = Join-Path $work 'unpacked'
	Expand-Archive -Path $archive -DestinationPath $unpacked

	$target = Join-Path $OutDir $package.Name
	if ( Test-Path $target ) { Remove-Item -Recurse -Force $target }
	foreach ( $copy in $package.Copy ) {
		$destination = Join-Path $target $copy[1]
		New-Item -ItemType Directory -Force $destination | Out-Null
		Copy-Item -Recurse -Path ( Join-Path $unpacked $copy[0] ) -Destination $destination
	}
	foreach ( $file in $package.Signed ) {
		$signature = Get-AuthenticodeSignature ( Join-Path $target $file )
		if ( $signature.Status -ne 'Valid' -or $signature.SignerCertificate.Subject -notmatch '^CN=Microsoft Corporation,' ) {
			Write-Error "$( $package.Name ): $file is not signed by Microsoft ($( $signature.Status ))"
			exit 2
		}
	}
}
Remove-Item -Recurse -Force $downloads
Set-Content -Path $stampFile -Value $stamp -Encoding ascii
Write-Host "DXC, the Agility SDK and WARP are in $OutDir; run cmake --preset vs2022-x64 again"
