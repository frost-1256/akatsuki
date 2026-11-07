// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - software VSync for the display-only driver (WP2b-3b). New file, not derived from KMDOD.
//
// A display-only driver that registers DxgkDdiControlInterrupt / DxgkDdiGetScanLine must report VSync through
// DxgkCbNotifyInterrupt(DXGK_INTERRUPT_DISPLAYONLY_VSYNC) and must report real target-mode frequencies.
// The MDSS VSync interrupt is not used yet (enabling it needs register writes on a shared line). Instead a 1 ms
// high-resolution timer polls the INTF1 frame counter and reports each new frame from inside
// DxgkCbSynchronizeExecution, the same path KMDOD already uses for DXGK_INTERRUPT_DISPLAYONLY_PRESENT_PROGRESS.

#include "BDD.hxx"
#include "epi_hw.hxx"
#include "epi_vsync.hxx"

BOOLEAN g_EpiSoftVsync = TRUE;

#pragma code_seg(push)
#pragma code_seg("PAGE")

VOID EpiReadDriverSwitches(_In_ PUNICODE_STRING RegistryPath)
{
    PAGED_CODE();
    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, RegistryPath, OBJ_KERNEL_HANDLE | OBJ_CASE_INSENSITIVE, NULL, NULL);
    HANDLE hKey = NULL;
    if (NT_SUCCESS(ZwOpenKey(&hKey, KEY_READ, &oa)))
    {
        g_EpiSoftVsync = EpiQueryDword(hKey, L"EpiSoftVsync", 1) != 0 ? TRUE : FALSE;
        ZwClose(hKey);
    }
}

#pragma code_seg(pop)

// ---- Non-paged: timer callback, synchronize routine, DDI helpers ----

typedef struct _EPI_VSYNC_SYNC
{
    const DXGKRNL_INTERFACE* Dxgk;
    ULONG                    TargetId;
} EPI_VSYNC_SYNC;

static BOOLEAN EpiVsyncNotifySync(_In_opt_ PVOID Context)
{
    const EPI_VSYNC_SYNC* s = static_cast<const EPI_VSYNC_SYNC*>(Context);
    DXGKARGCB_NOTIFY_INTERRUPT_DATA n = {};
    n.InterruptType = DXGK_INTERRUPT_DISPLAYONLY_VSYNC;
    n.DisplayOnlyVsync.VidPnTargetId = s->TargetId;
    s->Dxgk->DxgkCbNotifyInterrupt(s->Dxgk->DeviceHandle, &n);
    s->Dxgk->DxgkCbQueueDpc(s->Dxgk->DeviceHandle);
    return TRUE;
}

static VOID EpiVsyncTimer(_In_ PEX_TIMER Timer, _In_opt_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Timer);
    EPI_VSYNC* v = static_cast<EPI_VSYNC*>(Context);
    if (v == NULL || v->Intf == NULL || !v->Enabled) { return; }
    const ULONG frame = READ_REGISTER_ULONG(v->Intf + EPI_INTF_FRAME_COUNT / 4);
    if (frame == v->LastFrame) { return; }
    if (frame - v->LastFrame > 1) { InterlockedIncrement(&v->Skipped); }
    v->LastFrame = frame;

    EPI_VSYNC_SYNC s = { v->Dxgk, v->TargetId };
    BOOLEAN ret = FALSE;
    if (NT_SUCCESS(v->Dxgk->DxgkCbSynchronizeExecution(v->Dxgk->DeviceHandle, EpiVsyncNotifySync, &s, 0, &ret)))
    {
        InterlockedIncrement(&v->Notifies);
    }
}

NTSTATUS EpiVsyncControl(_Inout_ EPI_VSYNC* pV, BOOLEAN Enable)
{
    if (pV->Intf == NULL || pV->Timer == NULL) { return STATUS_NOT_SUPPORTED; }
    if (Enable)
    {
        if (InterlockedExchange(&pV->Enabled, 1) == 0)
        {
            pV->LastFrame = READ_REGISTER_ULONG(pV->Intf + EPI_INTF_FRAME_COUNT / 4);
            ExSetTimer(pV->Timer, -EPI_VSYNC_POLL_100NS, EPI_VSYNC_POLL_100NS, NULL);
        }
    }
    else
    {
        if (InterlockedExchange(&pV->Enabled, 0) != 0)
        {
            ExCancelTimer(pV->Timer, NULL);
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
    const ULONG line = READ_REGISTER_ULONG(pV->Intf + EPI_INTF_LINE_COUNT / 4);
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
                       ULONG VActive, ULONG TargetId)
{
    PAGED_CODE();
    RtlZeroMemory(pV, sizeof(*pV));
    pV->Dxgk = pDxgk;
    pV->VActive = VActive;
    pV->TargetId = TargetId;
    if (!pRes->MdpFound || pRes->MdpLength < EPI_INTF1_OFF + EPI_INTF1_MAP_SIZE) { return STATUS_DEVICE_CONFIGURATION_ERROR; }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = pRes->MdpBase.QuadPart + EPI_INTF1_OFF;
    PVOID va = MmMapIoSpaceEx(pa, EPI_INTF1_MAP_SIZE, PAGE_READONLY | PAGE_NOCACHE);
    if (va == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }
    pV->Timer = ExAllocateTimer(EpiVsyncTimer, pV, EX_TIMER_HIGH_RESOLUTION);
    if (pV->Timer == NULL)
    {
        MmUnmapIoSpace(va, EPI_INTF1_MAP_SIZE);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    pV->Intf = static_cast<volatile ULONG*>(va);
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
        MmUnmapIoSpace(const_cast<ULONG*>(pV->Intf), EPI_INTF1_MAP_SIZE);
        pV->Intf = NULL;
    }
}

#pragma code_seg(pop)
