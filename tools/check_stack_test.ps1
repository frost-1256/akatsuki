# SPDX-License-Identifier: MIT
# Text-only regression cases. No compilation, driver load, signing, or device access.
$ErrorActionPreference = 'Stop'
$cases = @(
    @{ Name = 'immediate-and-preindex'; Code = 0; Lines = @(
        'Small:',
        '  0000000140001000: A9BE7BFD  stp fp,lr,[sp,#-0x20]!',
        '  0000000140001004: D10083FF  sub sp,sp,#2016',
        '  0000000140001008: D65F03C0  ret'
    ); Expect = 'stack_check=PASS functions=1 max_frame=2048 max_function=Small' },
    @{ Name = 'warning-boundary'; Code = 0; Lines = @(
        'Warn:',
        '  0000000140001000: D14007FF  sub sp,sp,#1,lsl #12',
        '  0000000140001004: D65F03C0  ret'
    ); Expect = 'stack_check=WARN Warn 4096' },
    @{ Name = 'oversized-shift'; Code = 1; Lines = @(
        'Large:',
        '  0000000140001000: A9BE7BFD  stp fp,lr,[sp,#-0x20]!',
        '  0000000140001004: D14007FF  sub sp,sp,#0x1,lsl #12',
        '  0000000140001008: D65F03C0  ret'
    ); Expect = 'stack_check=FAIL Large 4128' },
    @{ Name = 'chkstk'; Code = 1; Lines = @(
        '?EpiGmuBootTest@@YAXXZ:',
        '  0000000140001000: A9BA7BFD  stp fp,lr,[sp,#-0x60]!',
        '  0000000140001004: 940017BB  bl __security_push_cookie',
        '  0000000140001008: D2807FCF  mov x15,#0x3FE',
        '  000000014000100C: 940017A0  bl __chkstk',
        '  0000000140001010: CB2F73FF  sub sp,sp,x15,lsl #4',
        '  0000000140001014: D65F03C0  ret'
    ); Expect = 'stack_check=FAIL EpiGmuBootTest 16448' },
    @{ Name = 'unknown-register'; Code = 1; Lines = @(
        'Unknown:',
        '  0000000140001000: CB2F73FF  sub sp,sp,x15,lsl #4',
        '  0000000140001004: D65F03C0  ret'
    ); Expect = 'parse_error=unresolved x15/__chkstk allocation' },
    @{ Name = 'split-prologue'; Code = 1; Lines = @(
        'Split:',
        '  0000000140001000: 94000000  bl SomeFunction',
        '  0000000140001004: D10083FF  sub sp,sp,#32',
        '  0000000140001008: D65F03C0  ret'
    ); Expect = 'parse_error=stack allocation outside entry prologue' },
    @{ Name = 'missing-symbols'; Code = 1; Lines = @(
        '  0000000140001000: D65F03C0  ret'
    ); Expect = 'stack_check=ERROR unnamed ARM64 instructions' }
)
$scratch = Join-Path (Split-Path $PSScriptRoot -Parent) ('build\stack_parser_' + [guid]::NewGuid().ToString('N') + '.txt')
New-Item -ItemType Directory -Force (Split-Path $scratch -Parent) | Out-Null
try {
    foreach ($case in $cases) {
        $case.Lines | Set-Content -LiteralPath $scratch -Encoding ascii
        $output = & powershell -NoProfile -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'check_stack.ps1') -DisassemblyPath $scratch
        $code = $LASTEXITCODE
        if ($code -ne $case.Code -or ($output -join "`n").IndexOf($case.Expect, [StringComparison]::Ordinal) -lt 0) {
            throw "$($case.Name): exit=$code, output=$($output -join '; ')"
        }
    }
    Write-Output "stack parser cases passed: $($cases.Count)"
} finally {
    Remove-Item -LiteralPath $scratch
}
exit 0
