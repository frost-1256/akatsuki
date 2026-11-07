// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - software VSync for the display-only driver (WP2b-3b). New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx (ntddk.h, dispmprt.h) and epi_hw.hxx.

// INTF1 registers (offsets from MDP_REGS), observed read-only in WP2b-3a on spacewar:
//   +0xAC frame counter (+1 per frame, ~16.72 ms), +0xB0 scan line (1..VActive, wraps with the frame counter).
#define EPI_INTF1_OFF            0x36000
#define EPI_INTF1_MAP_SIZE       0x100
#define EPI_INTF_FRAME_COUNT     0xAC
#define EPI_INTF_LINE_COUNT      0xB0

// Poll period of the high-resolution timer (100 ns units). 1 ms keeps the VSync jitter below 1 ms.
#define EPI_VSYNC_POLL_100NS     10000

typedef struct _EPI_VSYNC
{
    volatile ULONG*          Intf;          // INTF1 mapped read-only; NULL when not started
    PEX_TIMER                Timer;
    const DXGKRNL_INTERFACE* Dxgk;
    ULONG                    VActive;
    ULONG                    TargetId;
    ULONG                    LastFrame;
    volatile LONG            Enabled;       // DXGK_INTERRUPT_DISPLAYONLY_VSYNC enabled by dxgkrnl
    volatile LONG            Notifies;      // VSyncs reported
    volatile LONG            Skipped;       // frame counter advanced by more than 1 between polls
} EPI_VSYNC;

// Driver-wide switch, read in DriverEntry from the service key (Services\Epinephrine\EpiSoftVsync, DWORD,
// default 1). 0 = do not register DxgkDdiControlInterrupt / DxgkDdiGetScanLine (plain KMDOD behaviour,
// frequencies NOTSPECIFIED). Changing it needs the driver to be reloaded (restart the device).
extern BOOLEAN g_EpiSoftVsync;
_IRQL_requires_(PASSIVE_LEVEL) VOID EpiReadDriverSwitches(_In_ PUNICODE_STRING RegistryPath);

// StartDevice / StopDevice (PASSIVE). Start maps INTF1 read-only and allocates the timer; it never writes hardware.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS EpiVsyncStart(_Out_ EPI_VSYNC* pV, _In_ const EPI_HW_RESOURCES* pRes, _In_ const DXGKRNL_INTERFACE* pDxgk,
                       ULONG VActive, ULONG TargetId);
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiVsyncStop(_Inout_ EPI_VSYNC* pV);

// DxgkDdiControlInterrupt(DXGK_INTERRUPT_DISPLAYONLY_VSYNC) and DxgkDdiGetScanLine helpers (<= DISPATCH_LEVEL).
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS EpiVsyncControl(_Inout_ EPI_VSYNC* pV, BOOLEAN Enable);
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS EpiVsyncScanLine(_In_ const EPI_VSYNC* pV, _Out_ BOOLEAN* pInVBlank, _Out_ UINT* pScanLine);
