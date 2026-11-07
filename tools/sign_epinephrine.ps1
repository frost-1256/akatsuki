# Test-sign the Epinephrine package (Epinephrine.sys, then epinephrine.cat) with the spacewar test certificate.
# SPDX-License-Identifier: MIT
# The certificate (CurrentUser\My) and the phone's testsigning trust are set up outside this repository.
# Verification with /pa fails on the PC (test root not trusted there); that is expected and only logged.
param(
  [ValidateSet('Release', 'Debug')][string]$Configuration = 'Release',
  [string]$CertificateThumbprint = '270E894625FF92924385BB9C7774E455D464F890',
  [string]$SignTool = 'C:\Program Files (x86)\Windows Kits\10\bin\10.0.28000.0\x64\signtool.exe'
)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$pkg = Join-Path $root "build\$Configuration\SampleDisplay"
$log = Join-Path $root 'build\sign.log'
"sign $(Get-Date -Format o) thumb=$CertificateThumbprint" | Out-File $log -Encoding utf8
foreach ($f in 'Epinephrine.sys', 'epinephrine.cat') {
  $p = Join-Path $pkg $f
  if (-not (Test-Path $p)) { throw "missing $p (build first)" }
  $ErrorActionPreference = 'Continue'
  & $SignTool sign /fd sha256 /s My /sha1 $CertificateThumbprint $p 2>&1 | Out-File $log -Append -Encoding utf8
  $code = $LASTEXITCODE
  $ErrorActionPreference = 'Stop'
  "sign_exit[$f]=$code" | Out-File $log -Append -Encoding utf8
  if ($code -ne 0) { throw "signing $f failed; see $log" }
}
$ErrorActionPreference = 'Continue'
& $SignTool verify /pa /c (Join-Path $pkg 'epinephrine.cat') (Join-Path $pkg 'Epinephrine.sys') 2>&1 | Out-File $log -Append -Encoding utf8
"verify_catalog_exit=$LASTEXITCODE (nonzero expected on the PC: test root untrusted)" | Out-File $log -Append -Encoding utf8
$ErrorActionPreference = 'Stop'
# .NET SHA256 instead of Get-FileHash (the Utility module is not always loadable in a bare -NoProfile host).
$sha = [System.Security.Cryptography.SHA256]::Create()
Get-ChildItem $pkg -File | ForEach-Object {
  $fs = [System.IO.File]::OpenRead($_.FullName)
  try { $h = -join ($sha.ComputeHash($fs) | ForEach-Object { $_.ToString('X2') }) } finally { $fs.Dispose() }
  "$($_.Name)  $h"
} | Tee-Object -FilePath $log -Append
