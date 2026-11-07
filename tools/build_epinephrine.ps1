# Build Epinephrine (ARM64 Release, unsigned) with a mounted EWDK.
# SPDX-License-Identifier: MIT
# Usage: powershell -NoProfile -ExecutionPolicy Bypass -File tools\build_epinephrine.ps1 [-Configuration Release|Debug]
# Output: build\<Configuration>\ (Epinephrine.sys, Epinephrine.inf, Epinephrine.cat if inf2cat ran) and build\build.log
param([ValidateSet('Release', 'Debug')][string]$Configuration = 'Release')
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$proj = Join-Path $root 'src\epinephrine\Sample\SampleDisplay.vcxproj'
$out = Join-Path $root "build\$Configuration"
New-Item -ItemType Directory -Force $out | Out-Null
$log = Join-Path $root 'build\build.log'

# Find a mounted EWDK (never touch the phone volumes R:/Y:).
$ewdk = $null
foreach ($d in [IO.DriveInfo]::GetDrives()) {
  if ($d.Name -match '^[RY]:' -or -not $d.IsReady) { continue }
  if ($d.DriveType -notin @([IO.DriveType]::CDRom, [IO.DriveType]::Fixed)) { continue }
  if (Test-Path (Join-Path $d.Name 'BuildEnv\SetupBuildEnv.cmd')) { $ewdk = $d.Name; break }
}
if (-not $ewdk) { throw 'EWDK not mounted (mount EWDK_*.iso first)' }

$intDir = Join-Path $root "build\obj\$Configuration\"
# IntDir/OutDir are passed unquoted (a trailing \ before a quote would escape it), so no spaces allowed
if ("$intDir$out" -match '\s') { throw 'build paths must not contain spaces' }
$cmd = "call `"${ewdk}BuildEnv\SetupBuildEnv.cmd`" amd64 >nul && msbuild `"$proj`" /nologo /m /t:Rebuild " +
       "/p:Configuration=$Configuration /p:Platform=ARM64 /p:SignMode=Off /p:IntDir=$intDir /p:OutDir=$out\ /v:minimal"
"ewdk=$ewdk`r`ncmd=$cmd" | Out-File $log -Encoding utf8
# stderr lines (e.g. vswhere noise) must not abort the script
$ErrorActionPreference = 'Continue'
cmd /c "$cmd 2>&1" | Tee-Object -FilePath $log -Append
$code = $LASTEXITCODE
$ErrorActionPreference = 'Stop'
"msbuild_exit=$code" | Out-File $log -Append -Encoding utf8
if ($code -ne 0) { throw "build failed ($code), see $log" }
Get-ChildItem $out -Recurse -File | Where-Object Extension -in '.sys', '.inf', '.cat', '.pdb' |
  ForEach-Object { "$($_.FullName.Substring($out.Length + 1))  $((Get-FileHash $_.FullName).Hash)" } | Tee-Object -FilePath $log -Append
