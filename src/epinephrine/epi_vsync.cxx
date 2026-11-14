// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - VSync for the display-only driver (WP2b-3b/3e). New file, not derived from KMDOD.
//
// A display-only driver that registers DxgkDdiControlInterrupt / DxgkDdiGetScanLine must report VSync through
// DxgkCbNotifyInterrupt(DXGK_INTERRUPT_DISPLAYONLY_VSYNC) and must report real target-mode frequencies.
//
// Software mode (default): a 1 ms high-resolution timer polls the INTF1 frame counter and reports each new frame
// from inside DxgkCbSynchronizeExecution. Nothing is written to the hardware.
//
// Hardware mode (Device Parameters\EpiHwVsync, WP2b-3e): the INTF1 frame interrupt (status bit11) is enabled and
// reported from the ISR (IRQ 0x73, assigned to this device only). Guards:
//  - the ISR clears the enable itself when more than EPI_HWVSYNC_STORM_LIMIT frame interrupts arrive in 1 s;
//  - the timer keeps running as a watchdog (every 20 ms while armed) and disarms when the frame counter moves EPI_HWVSYNC_WATCH_FRAMES
//    frames without a frame interrupt (line not delivered to us);
//  - after any failure the rest of this start uses software mode.

#include "BDD.hxx"
#include "epi_hw.hxx"
#include "epi_vsync.hxx"

BOOLEAN g_EpiSoftVsync = TRUE;
ULONG g_EpiWddmMode = 0;

#pragma code_seg(push)
#pragma code_seg("PAGE")

VOID EpiReadDriverSwitches(_In_ PUNICODE_STRING RegistryPath)
{
    PAGED_CODE();
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, RegistryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    HANDLE hKey = NULL;
    BOOLEAN canWrite = TRUE;
    NTSTATUS st = ZwOpenKey(&hKey, KEY_READ | KEY_SET_VALUE, &oa);
    if (!NT_SUCCESS(st))
    {
        canWrite = FALSE;
        st = ZwOpenKey(&hKey, KEY_READ, &oa);
    }
    if (NT_SUCCESS(st))
    {
        g_EpiSoftVsync = EpiQueryDword(hKey, L"EpiSoftVsync", 1) != 0 ? TRUE : FALSE;
        const ULONG mode = EpiQueryDword(hKey, L"EpiWddmMode", 0);
        if (mode == 2)
        {
            // One shot: only when the reset is on disk (otherwise a crash could repeat at every boot).
            if (canWrite && NT_SUCCESS(EpiSetDword(hKey, L"EpiWddmMode", 0)) && NT_SUCCESS(ZwFlushKey(hKey)))
            {
                g_EpiWddmMode = 2;
            }
        }
        else if (mode == 1)
        {
            g_EpiWddmMode = 1;
        }
        ZwClose(hKey);
    }
    if (g_EpiWddmMode != 0)
    {
        g_EpiSoftVsync = TRUE;   // a full driver must report VSync (and the modes then carry real frequencies)
    }
}

#pragma code_seg(pop)

// ---- Non-paged ----

static __forceinline ULONG EpiRd(volatile ULONG* Base, ULONG Off) { return READ_REGISTER_ULONG(Base + Off / 4); }
static __forceinline VOID EpiWr(volatile ULONG* Base, ULONG Off, ULONG V) { WRITE_REGISTER_ULONG(Base + Off / 4, V); }

static VOID EpiReportVsync(_In_ EPI_VSYNC* v)   // at DIRQL (ISR or synchronize routine)
{
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {};
    if (g_EpiWddmMode != 0)
    {
        // Full driver: the flip whose primary is at CrtcAddress is on the screen (epi_wddm.cxx CPU scanout).
        n.InterruptType = DXGK_INTERRUPT_CRTC_VSYNC;
        n.CrtcVsync.VidPnTargetId = v->TargetId;
        n.CrtcVsync.PhysicalAddress.QuadPart = InterlockedCompareExchange64(&v->CrtcAddress, 0, 0);
    }
    else
    {
        n.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_VSYNC;
        n.DisplayOnlyVsync.VidPnTargetId = v->TargetId;
    }
    v->Dxgk->DxgkCbNotifyInterrupt(v->Dxgk->DeviceHandle, &n);
    v->Dxgk->DxgkCbQueueDpc(v->Dxgk->DeviceHandle);
    InterlockedIncrement(&v->Notifies);
}

static BOOLEAN EpiVsyncNotifySync(_In_opt_ PVOID Context)
{
    EpiReportVsync(static_cast<EPI_VSYNC*>(Context));
    return TRUE;
}

// Arm / disarm run synchronized with the ISR (or at DIRQL inside it).
static VOID EpiHwDisarmLocked(_Inout_ EPI_VSYNC* v)
{
    EpiWr(v->Intf, EPI_INTF_INTR_EN, EpiRd(v->Intf, EPI_INTF_INTR_EN) & ~EPI_INTF_INTR_FRAME);
    EpiWr(v->Intf, EPI_INTF_INTR_CLEAR, EPI_INTF_INTR_FRAME);
    InterlockedExchange(&v->HwArmed, 0);
}

static BOOLEAN EpiHwArmSync(_In_opt_ PVOID Context)
{
    EPI_VSYNC* v = static_cast<EPI_VSYNC*>(Context);
    v->ArmFrame = EpiRd(v->Intf, EPI_INTF_FRAME_COUNT);
    v->ArmIsrFrames = v->IsrFrames;
    v->StormWindow = KeQueryInterruptTime();
    v->StormCount = 0;
    EpiWr(v->Intf, EPI_INTF_INTR_CLEAR, EPI_INTF_INTR_FRAME);
    InterlockedExchange(&v->HwArmed, 1);
    EpiWr(v->Intf, EPI_INTF_INTR_EN, EpiRd(v->Intf, EPI_INTF_INTR_EN) | EPI_INTF_INTR_FRAME);
    return TRUE;
}

static BOOLEAN EpiFlushSync(_In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    return TRUE;
}

static BOOLEAN EpiHwDisarmSync(_In_opt_ PVOID Context)
{
    EpiHwDisarmLocked(static_cast<EPI_VSYNC*>(Context));
    return TRUE;
}

static VOID EpiHwDisarm(_Inout_ EPI_VSYNC* v, LONG Reason)
{
    if (Reason != EPI_HWV_OK) { InterlockedCompareExchange(&v->HwFailed, Reason, EPI_HWV_OK); }
    if (!v->HwArmed) { return; }
    BOOLEAN ret = FALSE;
    if (!NT_SUCCESS(v->Dxgk->DxgkCbSynchronizeExecution(v->Dxgk->DeviceHandle, EpiHwDisarmSync, v, 0, &ret)))
    {
        EpiHwDisarmLocked(v);   // interrupt object gone: nothing to race with
    }
}

BOOLEAN EpiVsyncIsr(_Inout_ EPI_VSYNC* v)
{
    InterlockedIncrement(&v->IsrCalls);
    if (v->Intf == NULL || !v->HwArmed) { return FALSE; }
    const ULONG st = EpiRd(v->Intf, EPI_INTF_INTR_STATUS);
    if ((st & EPI_INTF_INTR_FRAME) == 0) { return FALSE; }
    v->LastTop = EpiRd(v->Mdss, EPI_MDSS_HW_INTR_STATUS);
    EpiWr(v->Intf, EPI_INTF_INTR_CLEAR, EPI_INTF_INTR_FRAME);
    InterlockedIncrement(&v->IsrFrames);

    const ULONGLONG now = KeQueryInterruptTime();
    if (now - v->StormWindow >= 10000000ULL)
    {
        v->StormWindow = now;
        v->StormCount = 0;
    }
    if (++v->StormCount > EPI_HWVSYNC_STORM_LIMIT)
    {
        InterlockedCompareExchange(&v->HwFailed, EPI_HWV_STORM, EPI_HWV_OK);
        EpiHwDisarmLocked(v);
        return TRUE;
    }
    if (v->Enabled) { EpiReportVsync(v); }
    return TRUE;
}

static VOID EpiVsyncTimer(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);
    EPI_VSYNC* v = static_cast<EPI_VSYNC*>(Context);
    if (v == NULL || v->Intf == NULL || !v->Enabled) { return; }
    const ULONG frame = EpiRd(v->Intf, EPI_INTF_FRAME_COUNT);

    if (v->HwArmed)
    {
        // Watchdog: frames are running but no frame interrupt reached the ISR.
        if (frame - v->ArmFrame >= EPI_HWVSYNC_WATCH_FRAMES && v->IsrFrames == v->ArmIsrFrames)
        {
            EpiHwDisarm(v, EPI_HWV_NO_IRQ);
        }
        v->LastFrame = frame;
        if (v->HwArmed) { return; }   // the ISR reports VSync
    }
    if (!v->FastPoll)
    {
        // Disarmed (watchdog or storm guard in the ISR): software VSync needs the 1 ms poll again.
        v->FastPoll = TRUE;
        ExSetTimer(v->Timer, -EPI_VSYNC_POLL_100NS, EPI_VSYNC_POLL_100NS, NULL);
    }

    if (frame == v->LastFrame) { return; }
    if (frame - v->LastFrame > 1) { InterlockedIncrement(&v->Skipped); }
    v->LastFrame = frame;
    BOOLEAN ret = FALSE;
    v->Dxgk->DxgkCbSynchronizeExecution(v->Dxgk->DeviceHandle, EpiVsyncNotifySync, v, 0, &ret);
}

NTSTATUS EpiVsyncControl(_Inout_ EPI_VSYNC* pV, BOOLEAN Enable)
{
    if (pV->Intf == NULL || pV->Timer == NULL) { return STATUS_NOT_SUPPORTED; }
    if (Enable)
    {
        if (InterlockedExchange(&pV->Enabled, 1) == 0)
        {
            pV->LastFrame = EpiRd(pV->Intf, EPI_INTF_FRAME_COUNT);
            if (pV->HwMode && pV->HwFailed == EPI_HWV_OK && !pV->HwArmed)
            {
                BOOLEAN ret = FALSE;
                if (!NT_SUCCESS(pV->Dxgk->DxgkCbSynchronizeExecution(pV->Dxgk->DeviceHandle, EpiHwArmSync, pV, 0, &ret)))
                {
                    InterlockedCompareExchange(&pV->HwFailed, EPI_HWV_ARM_FAILED, EPI_HWV_OK);
                }
            }
            pV->FastPoll = pV->HwArmed ? FALSE : TRUE;
            const LONGLONG period = pV->FastPoll ? EPI_VSYNC_POLL_100NS : EPI_VSYNC_WATCH_100NS;
            ExSetTimer(pV->Timer, -period, period, NULL);
        }
    }
    else
    {
        if (InterlockedExchange(&pV->Enabled, 0) != 0)
        {
            ExCancelTimer(pV->Timer, NULL);
            EpiHwDisarm(pV, EPI_HWV_OK);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS EpiVsyncScanLine(_In_ const EPI_VSYNC* pV, _Out_ BOOLEAN* pInVBlank, _Out_ UINT* pScanLine)
{
    *pInVBlank = FALSE;
    *pScanLine = 0;
    if (pV->Intf == NULL) { return STATUS_NOT_SUPPORTED; }
    // The counter runs 1..VActive in the active area; 0 or > VActive is the vertical blank.
    const ULONG line = EpiRd(pV->Intf, EPI_INTF_LINE_COUNT);
    if (line == 0 || line > pV->VActive)
    {
        *pInVBlank = TRUE;
        *pScanLine = line == 0 ? pV->VActive : line;
    }
    else
    {
        *pScanLine = line - 1;
    }
    return STATUS_SUCCESS;
}

#pragma code_seg(push)
#pragma code_seg("PAGE")

NTSTATUS EpiVsyncStart(_Out_ EPI_VSYNC* pV, _In_ const EPI_HW_RESOURCES* pRes, _In_ const DXGKRNL_INTERFACE* pDxgk,
                       ULONG VActive, ULONG TargetId, BOOLEAN HwMode)
{
    PAGED_CODE();
    RtlZeroMemory(pV, sizeof(*pV));
    pV->Dxgk = pDxgk;
    pV->VActive = VActive;
    pV->TargetId = TargetId;
    if (!pRes->MdpFound || pRes->MdpLength < EPI_INTF1_OFF + EPI_INTF1_MAP_SIZE) { return STATUS_DEVICE_CONFIGURATION_ERROR; }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = pRes->MdpBase.QuadPart + EPI_INTF1_OFF;
    PVOID intf = MmMapIoSpaceEx(pa, EPI_INTF1_MAP_SIZE, (HwMode ? PAGE_READWRITE : PAGE_READONLY) | PAGE_NOCACHE);
    pa.QuadPart = pRes->MdpBase.QuadPart;
    PVOID mdss = MmMapIoSpaceEx(pa, EPI_MDSS_MAP_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    PEX_TIMER timer = ExAllocateTimer(EpiVsyncTimer, pV, EX_TIMER_HIGH_RESOLUTION);
    if (intf == NULL || mdss == NULL || timer == NULL)
    {
        if (timer != NULL) { ExDeleteTimer(timer, TRUE, FALSE, NULL); }
        if (mdss != NULL) { MmUnmapIoSpace(mdss, EPI_MDSS_MAP_SIZE); }
        if (intf != NULL) { MmUnmapIoSpace(intf, EPI_INTF1_MAP_SIZE); }
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pV->Timer = timer;
    pV->Mdss = static_cast<volatile ULONG*>(mdss);
    pV->HwMode = HwMode;
    pV->Intf = static_cast<volatile ULONG*>(intf);   // last: the ISR and timer check Intf
    return STATUS_SUCCESS;
}

VOID EpiVsyncStop(_Inout_ EPI_VSYNC* pV)
{
    PAGED_CODE();
    InterlockedExchange(&pV->Enabled, 0);
    if (pV->Timer != NULL)
    {
        ExDeleteTimer(pV->Timer, TRUE, TRUE, NULL);   // cancel and wait for a running callback
        pV->Timer = NULL;
    }
    if (pV->Intf != NULL)
    {
        EpiHwDisarm(pV, EPI_HWV_OK);
        volatile ULONG* intf = pV->Intf;
        pV->Intf = NULL;
        BOOLEAN ret = FALSE;
        // Flush a concurrent ISR that already read Intf before unmapping.
        pV->Dxgk->DxgkCbSynchronizeExecution(pV->Dxgk->DeviceHandle, EpiFlushSync, NULL, 0, &ret);
        MmUnmapIoSpace(const_cast<ULONG*>(intf), EPI_INTF1_MAP_SIZE);
    }
    if (pV->Mdss != NULL)
    {
        MmUnmapIoSpace(const_cast<ULONG*>(pV->Mdss), EPI_MDSS_MAP_SIZE);
        pV->Mdss = NULL;
    }
}

#pragma code_seg(pop)
