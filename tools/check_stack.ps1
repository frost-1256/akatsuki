# SPDX-License-Identifier: MIT
# Inspect the linked ARM64 image, without loading it. PDB must remain next to the image.
# -DisassemblyPath accepts dumpbin output for parser regression cases; it never runs dumpbin.
[CmdletBinding()]
param(
    [string]$ImagePath,
    [string]$DumpbinPath,
    [string]$EwdkRoot,
    [string]$LogPath,
    [string]$DisassemblyPath,
    [int]$WarnBytes = 2048,
    [int]$FailBytes = 4096
)
$ErrorActionPreference = 'Stop'
if (-not $ImagePath) { $ImagePath = Join-Path (Split-Path $PSScriptRoot -Parent) 'build\Release\Epinephrine.sys' }

function Write-StackResult([string]$Message) {
    Write-Output $Message
    if ($LogPath) { $Message | Out-File -LiteralPath $LogPath -Append -Encoding utf8 }
}
function Get-Immediate([string]$Value) {
    if ($Value -match '^0x') { return [Convert]::ToInt64($Value.Substring(2), 16) }
    return [long]::Parse($Value, [Globalization.CultureInfo]::InvariantCulture)
}
function Get-StackFrame($Function) {
    $total = 0L
    $x15 = $null
    $pendingProbe = $null
    $hasProbe = $false
    $prologue = $true
    $errorText = $null
    foreach ($instruction in $Function.Instructions) {
        $op = $instruction.ToLowerInvariant()
        $allocation = $false
        $bytes = 0L
        if ($op -match '^(?:stp|str)\s+.+,\[sp,#-(0x[0-9a-f]+|\d+)\]!$') {
            $bytes = Get-Immediate $Matches[1]
            $allocation = $true
        } elseif ($op -match '^sub\s+sp,sp,#(0x[0-9a-f]+|\d+)(?:,lsl\s+#(0x[0-9a-f]+|\d+))?$') {
            $bytes = Get-Immediate $Matches[1]
            if ($Matches[2]) { $bytes = $bytes -shl (Get-Immediate $Matches[2]) }
            $allocation = $true
        } elseif ($op -match '^sub\s+sp,sp,x15,lsl\s+#(?:4|0x4)$') {
            $allocation = $true
            if ($null -eq $pendingProbe) { $errorText = 'unresolved x15/__chkstk allocation' }
            else { $bytes = $pendingProbe * 16; $pendingProbe = $null }
        } elseif ($op -match '^sub\s+sp,sp,') {
            $errorText = 'unsupported variable stack allocation'
        }
        if ($allocation) {
            # Never silently ignore a split/shrink-wrapped allocation outside the entry prologue.
            if (-not $prologue) { $errorText = 'stack allocation outside entry prologue' }
            $total += $bytes
        }
        if ($op -match '^(?:mov|movz)\s+x15,#(0x[0-9a-f]+|\d+)(?:,lsl\s+#(0x[0-9a-f]+|\d+))?$') {
            $x15 = Get-Immediate $Matches[1]
            if ($Matches[2]) { $x15 = $x15 -shl (Get-Immediate $Matches[2]) }
        } elseif ($op -match '^bl\s+.*__chkstk\b') {
            $hasProbe = $true
            $pendingProbe = $x15
            if ($null -eq $x15) { $errorText = '__chkstk without a known x15 constant' }
            $x15 = $null
        } elseif ($op -match '^(?:bl|blr)\s+' -and $op -notmatch '__security_push_cookie\b') {
            $prologue = $false
            $x15 = $null
        } elseif ($op -match '^(?:b(?:\.[a-z]+)?|br|ret|cbz|cbnz|tbz|tbnz)\b') {
            $prologue = $false
        } elseif ($op -match '^\w+\s+x15[,\s]') {
            # A different x15 assignment invalidates a previously seen constant.
            $x15 = $null
        }
    }
    if ($null -ne $pendingProbe) { $errorText = '__chkstk without its stack subtraction' }
    $name = $Function.Name
    if ($name -match '^\?([^@]+)@@') { $name = $Matches[1] }
    [PSCustomObject]@{ Name = $name; Bytes = $total; Chkstk = $hasProbe; Error = $errorText }
}

try {
    if ($WarnBytes -lt 0 -or $FailBytes -lt $WarnBytes) { throw 'invalid stack thresholds' }
    if ($DisassemblyPath) {
        $lines = Get-Content -LiteralPath $DisassemblyPath
    } else {
        # Fail closed for the wrong image architecture or a missing matching-symbol source.
        $image = [IO.File]::ReadAllBytes((Resolve-Path -LiteralPath $ImagePath).Path)
        $pe = [BitConverter]::ToInt32($image, 0x3c)
        if ([BitConverter]::ToUInt32($image, $pe) -ne 0x4550 -or
            [BitConverter]::ToUInt16($image, $pe + 4) -ne 0xAA64) { throw 'expected an ARM64 PE image' }
        if (-not (Test-Path -LiteralPath ([IO.Path]::ChangeExtension($ImagePath, '.pdb')))) {
            throw 'PDB missing beside image; named function checks are required'
        }
        if (-not $DumpbinPath) {
            if (-not $EwdkRoot) {
                foreach ($drive in [IO.DriveInfo]::GetDrives()) {
                    if ($drive.Name -match '^[RY]:' -or -not $drive.IsReady) { continue }
                    if (Test-Path (Join-Path $drive.Name 'BuildEnv\SetupBuildEnv.cmd')) { $EwdkRoot = $drive.Name; break }
                }
            }
            if (-not $EwdkRoot) { throw 'EWDK not mounted; specify -DumpbinPath or -EwdkRoot' }
            $DumpbinPath = Get-ChildItem -LiteralPath (Join-Path $EwdkRoot 'Program Files\Microsoft Visual Studio') `
                -Recurse -Filter dumpbin.exe -File |
                Where-Object FullName -Match '\\Hostx64\\arm64\\dumpbin\.exe$' |
                Sort-Object FullName -Descending | Select-Object -First 1 -ExpandProperty FullName
        }
        if (-not $DumpbinPath) { throw 'Hostx64/arm64 dumpbin.exe not found in EWDK' }
        $lines = & $DumpbinPath -nologo -disasm $ImagePath 2>&1
        if ($LASTEXITCODE -ne 0) { throw "dumpbin failed ($LASTEXITCODE)" }
    }
    $functions = [Collections.Generic.List[object]]::new()
    $current = $null
    foreach ($line in $lines) {
        $text = [string]$line
        if ($text -match '^([^\s].*):\s*$') {
            $current = [PSCustomObject]@{ Name = $Matches[1]; Instructions = [Collections.Generic.List[string]]::new() }
            $functions.Add($current)
        } elseif ($text -match '^\s+[0-9a-fA-F]{8,16}:\s+[0-9a-fA-F]{8}\s+([a-z][a-z0-9.]*\s*.*?)\s*$') {
            if ($null -eq $current) { throw 'unnamed ARM64 instructions; PDB symbols were not resolved' }
            $current.Instructions.Add($Matches[1])
        }
    }
    $frames = @($functions | Where-Object { $_.Instructions.Count } | ForEach-Object { Get-StackFrame $_ } |
        Sort-Object Bytes -Descending)
    if (-not $frames.Count) { throw 'no named ARM64 functions found' }
    $failures = 0
    foreach ($frame in $frames) {
        if ($frame.Chkstk) { Write-StackResult "stack_check=CHKSTK $($frame.Name) $($frame.Bytes)" }
        if ($frame.Error) {
            Write-StackResult "stack_check=FAIL $($frame.Name) $($frame.Bytes) parse_error=$($frame.Error)"
            ++$failures
        } elseif ($frame.Bytes -gt $FailBytes) {
            Write-StackResult "stack_check=FAIL $($frame.Name) $($frame.Bytes)"
            ++$failures
        } elseif ($frame.Bytes -gt $WarnBytes) {
            Write-StackResult "stack_check=WARN $($frame.Name) $($frame.Bytes)"
        }
    }
    if ($failures) { Write-StackResult "stack_check=FAILED functions=$($frames.Count) failures=$failures"; exit 1 }
    Write-StackResult "stack_check=PASS functions=$($frames.Count) max_frame=$($frames[0].Bytes) max_function=$($frames[0].Name)"
    exit 0
} catch {
    Write-StackResult "stack_check=ERROR $($_.Exception.Message)"
    exit 1
}
