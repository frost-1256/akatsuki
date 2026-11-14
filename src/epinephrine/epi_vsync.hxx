// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - VSync for the display-only driver (WP2b-3b/3e). New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx (ntddk.h, dispmprt.h) and epi_hw.hxx.

// INTF1 registers (offsets from MDP_REGS), observed on spacewar in WP2b-3a/3d:
//   +0xAC frame counter (+1 per frame, ~16.72 ms), +0xB0 scan line (1..VActive, wraps with the frame counter),
//   +0x1C0 interrupt enable, +0x1C4 status (latched), +0x1C8 clear (write 1 to clear).
//   Status bit11 fires once per frame at the frame-counter increment (line 1).
#define EPI_INTF1_OFF            0x36000
#define EPI_INTF1_MAP_SIZE       0x200
#define EPI_INTF_FRAME_COUNT     0xAC
#define EPI_INTF_LINE_COUNT      0xB0
#define EPI_INTF_INTR_EN         0x1C0
#define EPI_INTF_INTR_STATUS     0x1C4
#define EPI_INTF_INTR_CLEAR      0x1C8
#define EPI_INTF_INTR_FRAME      0x00000800u
#define EPI_MDSS_MAP_SIZE        0x20
#define EPI_MDSS_HW_INTR_STATUS  0x10

// Poll period of the high-resolution timer (100 ns units). Software mode: VSync source (jitter < 1 ms).
// Hardware mode: watchdog only.
#define EPI_VSYNC_POLL_100NS     10000
#define EPI_VSYNC_WATCH_100NS    200000     // hardware mode while armed: watchdog every 20 ms

// Hardware-mode guards.
#define EPI_HWVSYNC_STORM_LIMIT  200        // frame interrupts per second; above it the enable is cleared in the ISR
#define EPI_HWVSYNC_WATCH_FRAMES 5          // frames after arming with no interrupt -> disarm (line not delivered)

// HwFailed reasons (recorded in EpiDiag)
#define EPI_HWV_OK          0
#define EPI_HWV_STORM       1
#define EPI_HWV_NO_IRQ      2
#define EPI_HWV_ARM_FAILED  3

typedef struct _EPI_VSYNC
{
    volatile ULONG*          Intf;          // INTF1 (read-only, or read-write in hardware mode); NULL when not started
    volatile ULONG*          Mdss;          // MDSS wrapper (read-only)
    PEX_TIMER                Timer;
    const DXGKRNL_INTERFACE* Dxgk;
    ULONG                    VActive;
    ULONG                    TargetId;
    ULONG                    LastFrame;
    volatile LONG            Enabled;       // DXGK_INTERRUPT_DISPLAYONLY_VSYNC enabled by dxgkrnl
    volatile LONG            Notifies;      // VSyncs reported (either source)
    volatile LONG            Skipped;       // frame counter advanced by more than 1 between polls
    volatile LONG64          CrtcAddress;   // W-1 full mode: primary address reported with DXGK_INTERRUPT_CRTC_VSYNC
                                            // (set by the CPU scanout after it copied a flipped primary)

    // Hardware mode (INTF1 frame interrupt on IRQ 0x73)
    BOOLEAN                  HwMode;        // requested for this start (Device Parameters\EpiHwVsync)
    volatile LONG            HwArmed;       // interrupt enable bit set
    volatile LONG            HwFailed;      // EPI_HWV_*; once set, software mode for the rest of this start
    volatile LONG            IsrCalls;      // every ISR invocation (any line)
    volatile LONG            IsrFrames;     // frame interrupts handled
    ULONG                    ArmFrame;      // frame counter when armed
    LONG                     ArmIsrFrames;  // IsrFrames when armed
    ULONG                    LastTop;       // MDSS HW_INTR_STATUS seen by the last frame interrupt
    ULONGLONG                StormWindow;   // interrupt time (100 ns) of the current 1 s window
    ULONG                    StormCount;
    BOOLEAN                  FastPoll;      // timer currently at EPI_VSYNC_POLL_100NS
} EPI_VSYNC;

// Driver-wide switch, read in DriverEntry from the service key (Services\Epinephrine\EpiSoftVsync, DWORD,
// default 1). 0 = do not register DxgkDdiControlInterrupt / DxgkDdiGetScanLine (plain KMDOD behaviour,
// frequencies NOTSPECIFIED). Changing it needs the driver to be reloaded (restart the device).
extern BOOLEAN g_EpiSoftVsync;
// W-1 (G0_GPU_SURVEY section 9.21.72), read with the same key (Services\Epinephrine\EpiWddmMode, DWORD, default 0):
// 0 = display-only driver, 1 = full WDDM driver, 2 = full WDDM driver for this load only (DriverEntry resets the
// value to 0 and flushes it before registering, so a crash in full mode comes back as the display-only driver).
// Full mode forces g_EpiSoftVsync and reports DXGK_INTERRUPT_CRTC_VSYNC instead of DISPLAYONLY_VSYNC.
extern ULONG g_EpiWddmMode;
_IRQL_requires_(PASSIVE_LEVEL) VOID EpiReadDriverSwitches(_In_ PUNICODE_STRING RegistryPath);

// StartDevice / StopDevice (PASSIVE). HwMode maps INTF1 read-write; otherwise read-only and nothing is written.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS EpiVsyncStart(_Out_ EPI_VSYNC* pV, _In_ const EPI_HW_RESOURCES* pRes, _In_ const DXGKRNL_INTERFACE* pDxgk,
                       ULONG VActive, ULONG TargetId, BOOLEAN HwMode);
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiVsyncStop(_Inout_ EPI_VSYNC* pV);

// DxgkDdiControlInterrupt(DXGK_INTERRUPT_DISPLAYONLY_VSYNC) and DxgkDdiGetScanLine helpers (<= DISPATCH_LEVEL).
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS EpiVsyncControl(_Inout_ EPI_VSYNC* pV, BOOLEAN Enable);
_IRQL_requires_max_(DISPATCH_LEVEL) NTSTATUS EpiVsyncScanLine(_In_ const EPI_VSYNC* pV, _Out_ BOOLEAN* pInVBlank, _Out_ UINT* pScanLine);

// DxgkDdiInterruptRoutine body (DIRQL). Returns TRUE only for the INTF1 frame interrupt.
BOOLEAN EpiVsyncIsr(_Inout_ EPI_VSYNC* pV);
