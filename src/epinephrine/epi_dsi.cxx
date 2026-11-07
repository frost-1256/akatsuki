// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - DSI0 DCS brightness (WP2b-5). New file, not derived from KMDOD.
// Register layout and the MSM in-memory command format follow the mainline Linux msm DSI 6G host
// (drivers/gpu/drm/msm/dsi: dsi.xml offsets + DSI_6G_REG_SHIFT 4, dsi_cmd_dma_add, msm_dsi_host_cmd_xfer_commit).

#include "BDD.hxx"
#include "epi_hw.hxx"
#include "epi_dsi.hxx"

#pragma code_seg("PAGE")

// DSI0 controller, offsets from MDP_REGS (6G layout, already shifted by 4).
#define EPI_DSI0_OFF            0x94000
#define EPI_DSI0_SIZE           0x400
#define DSI_CTRL                0x004
#define DSI_STATUS0             0x008
#define DSI_DMA_BASE            0x048
#define DSI_DMA_LEN             0x04C
#define DSI_TRIG_CTRL           0x084
#define DSI_TRIG_DMA            0x090
#define DSI_INTR_CTRL           0x110

#define DSI_CTRL_ENABLE         0x00000001u
#define DSI_CTRL_CMD_MODE_EN    0x00000004u
#define DSI_STATUS0_DMA_BUSY    0x00000002u
#define DSI_STATUS0_MDP_BUSY    0x00000004u
#define DSI_IRQ_CMD_DMA_DONE    0x00000001u
#define DSI_IRQ_MASK_BITS       0x02220202u     // MASK_* bits (read-write); the even status bits are write-1-to-clear
#define DSI_TRIG_DMA_MASK       0x7u
#define DSI_TRIGGER_SW          4u

static ULONG Rd(volatile ULONG* b, ULONG off) { return READ_REGISTER_ULONG(b + off / 4); }
static VOID Wr(volatile ULONG* b, ULONG off, ULONG v) { WRITE_REGISTER_ULONG(b + off / 4, v); }

// Busy-wait until (reg & mask) == want. Returns elapsed microseconds, or MAXULONG on timeout.
static ULONG WaitBits(volatile ULONG* b, ULONG off, ULONG mask, ULONG want, ULONG timeoutUs)
{
    for (ULONG us = 0; us <= timeoutUs; us += 10)
    {
        if ((Rd(b, off) & mask) == want) { return us; }
        KeStallExecutionProcessor(10);
    }
    return MAXULONG;
}

ULONG EpiDsiPercentToLevel(UCHAR Percent)
{
    PAGED_CODE();
    if (Percent > 100) { Percent = 100; }
    return EPI_BL_MIN_LEVEL + ((ULONG)Percent * (EPI_BL_MAX_LEVEL - EPI_BL_MIN_LEVEL) + 50) / 100;
}

NTSTATUS EpiDsiStart(_Out_ EPI_DSI* pDsi, _In_ const EPI_HW_RESOURCES* pRes)
{
    PAGED_CODE();
    RtlZeroMemory(pDsi, sizeof(*pDsi));
    ExInitializeFastMutex(&pDsi->Lock);
    pDsi->Level = EPI_BL_DEFAULT;
    pDsi->Percent = 50;
    if (!pRes->MdpFound || pRes->MdpLength < EPI_DSI0_OFF + EPI_DSI0_SIZE) { return STATUS_DEVICE_CONFIGURATION_ERROR; }

    PHYSICAL_ADDRESS pa;
    pa.QuadPart = pRes->MdpBase.QuadPart + EPI_DSI0_OFF;
    pDsi->Regs = static_cast<volatile ULONG*>(MmMapIoSpaceEx(pa, EPI_DSI0_SIZE, PAGE_READWRITE | PAGE_NOCACHE));
    if (pDsi->Regs == NULL) { return STATUS_INSUFFICIENT_RESOURCES; }

    PHYSICAL_ADDRESS lo; lo.QuadPart = 0;
    PHYSICAL_ADDRESS hi; hi.QuadPart = 0xFFFFFFFFULL;
    PHYSICAL_ADDRESS bd; bd.QuadPart = 0;
    pDsi->Buf = static_cast<UCHAR*>(MmAllocateContiguousMemorySpecifyCache(PAGE_SIZE, lo, hi, bd, MmNonCached));
    if (pDsi->Buf == NULL)
    {
        MmUnmapIoSpace((PVOID)pDsi->Regs, EPI_DSI0_SIZE);
        pDsi->Regs = NULL;
        return STATUS_NO_MEMORY;
    }
    pDsi->BufPa = (ULONG)MmGetPhysicalAddress(pDsi->Buf).QuadPart;

    // Only drive the controller in the state UEFI left it in (and used itself).
    const ULONG ctrl = Rd(pDsi->Regs, DSI_CTRL);
    const ULONG trig = Rd(pDsi->Regs, DSI_TRIG_CTRL);
    if ((ctrl & (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN)) != (DSI_CTRL_ENABLE | DSI_CTRL_CMD_MODE_EN) ||
        (trig & DSI_TRIG_DMA_MASK) != DSI_TRIGGER_SW)
    {
        return STATUS_DEVICE_NOT_READY;   // mapped but not Ready; EpiDsiStop releases
    }
    pDsi->Ready = TRUE;
    return STATUS_SUCCESS;
}

VOID EpiDsiStop(_Inout_ EPI_DSI* pDsi)
{
    PAGED_CODE();
    if (pDsi->Regs == NULL && pDsi->Buf == NULL) { return; }
    ExAcquireFastMutex(&pDsi->Lock);
    pDsi->Ready = FALSE;
    pDsi->Enabled = FALSE;
    // After a DMA timeout the engine may still own the page: leak it rather than free memory under DMA.
    if (pDsi->Buf != NULL && pDsi->LastStatus != STATUS_IO_TIMEOUT)
    {
        MmFreeContiguousMemorySpecifyCache(pDsi->Buf, PAGE_SIZE, MmNonCached);
    }
    pDsi->Buf = NULL;
    if (pDsi->Regs != NULL) { MmUnmapIoSpace((PVOID)pDsi->Regs, EPI_DSI0_SIZE); }
    pDsi->Regs = NULL;
    ExReleaseFastMutex(&pDsi->Lock);
}

NTSTATUS EpiDsiSendBrightness(_Inout_ EPI_DSI* pDsi, ULONG Level)
{
    PAGED_CODE();
    if (Level > EPI_BL_MAX_LEVEL) { Level = EPI_BL_MAX_LEVEL; }
    NTSTATUS st;
    volatile ULONG* r = NULL;
    ExAcquireFastMutex(&pDsi->Lock);
    if (!pDsi->Ready || pDsi->Regs == NULL || pDsi->Buf == NULL)
    {
        st = STATUS_DEVICE_NOT_READY;
        goto out;
    }
    r = pDsi->Regs;
    if ((Rd(r, DSI_STATUS0) & DSI_STATUS0_DMA_BUSY) != 0)
    {
        st = STATUS_DEVICE_BUSY;
        goto out;
    }

    // DCS long write (0x39, VC0), payload {0x51, level[11:8], level[7:0]} (set_display_brightness_large).
    // MSM format: {wc lo, wc hi, data id, 0x80 last | 0x40 long}, payload, 0xFF padding to 4 bytes.
    {
        UCHAR* b = pDsi->Buf;
        b[0] = 3; b[1] = 0; b[2] = 0x39; b[3] = 0xC0;
        b[4] = 0x51; b[5] = (UCHAR)(Level >> 8); b[6] = (UCHAR)(Level & 0xFF); b[7] = 0xFF;
        KeMemoryBarrier();
    }

    // Clear a stale DMA_DONE (keep the mask bits), let the MDP frame transfer finish, then trigger.
    Wr(r, DSI_INTR_CTRL, (Rd(r, DSI_INTR_CTRL) & DSI_IRQ_MASK_BITS) | DSI_IRQ_CMD_DMA_DONE);
    pDsi->LastIdleUs = WaitBits(r, DSI_STATUS0, DSI_STATUS0_MDP_BUSY, 0, 40000);
    Wr(r, DSI_DMA_BASE, pDsi->BufPa);
    Wr(r, DSI_DMA_LEN, 8);
    KeMemoryBarrier();
    Wr(r, DSI_TRIG_DMA, 1);
    KeMemoryBarrier();
    pDsi->LastDoneUs = WaitBits(r, DSI_INTR_CTRL, DSI_IRQ_CMD_DMA_DONE, DSI_IRQ_CMD_DMA_DONE, 50000);
    if (pDsi->LastDoneUs != MAXULONG)
    {
        Wr(r, DSI_INTR_CTRL, (Rd(r, DSI_INTR_CTRL) & DSI_IRQ_MASK_BITS) | DSI_IRQ_CMD_DMA_DONE);
        pDsi->Level = Level;
        pDsi->Sends++;
        st = STATUS_SUCCESS;
    }
    else
    {
        pDsi->Ready = FALSE;
        st = STATUS_IO_TIMEOUT;
    }

out:
    if (!NT_SUCCESS(st)) { pDsi->Fails++; }
    pDsi->LastStatus = st;
    ExReleaseFastMutex(&pDsi->Lock);
    return st;
}

// ---- Brightness interface (monitor.sys -> dxgkrnl -> DxgkDdiQueryInterface) ----
static VOID EpiBlRef(PVOID) {}
static VOID EpiBlDeref(PVOID) {}

static NTSTATUS EpiBlGetPossible(PVOID Context, ULONG BufferSize, PUCHAR LevelCount, PUCHAR BrightnessLevels)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    // 0..100 in steps of 1, or of 5 when the buffer is small.
    const ULONG step = (BufferSize >= 101) ? 1 : (BufferSize >= 21 ? 5 : 0);
    if (step == 0) { return STATUS_BUFFER_TOO_SMALL; }
    UCHAR n = 0;
    for (ULONG v = 0; v <= 100; v += step) { BrightnessLevels[n++] = (UCHAR)v; }
    *LevelCount = n;
    return STATUS_SUCCESS;
}

static NTSTATUS EpiBlSet(PVOID Context, UCHAR Brightness)
{
    PAGED_CODE();
    return static_cast<BASIC_DISPLAY_DRIVER*>(Context)->EpiBrightnessSet(Brightness);
}

static NTSTATUS EpiBlGet(PVOID Context, PUCHAR Brightness)
{
    PAGED_CODE();
    *Brightness = static_cast<BASIC_DISPLAY_DRIVER*>(Context)->EpiBrightnessGet();
    return STATUS_SUCCESS;
}

static NTSTATUS EpiBlGetCaps(PVOID Context, DXGK_BRIGHTNESS_CAPS* Caps)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    Caps->Value = 0;        // no smooth brightness, no adaptive brightness, no nits
    return STATUS_SUCCESS;
}

static NTSTATUS EpiBlSetState(PVOID Context, DXGK_BRIGHTNESS_STATE* State)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(State);
    return STATUS_SUCCESS;
}

static NTSTATUS EpiBlSetOptimization(PVOID Context, DXGK_BACKLIGHT_OPTIMIZATION_LEVEL Level)
{
    PAGED_CODE();
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(Level);
    return STATUS_SUCCESS;  // no content-adaptive backlight reduction
}

static NTSTATUS EpiBlGetReduction(PVOID Context, DXGK_BACKLIGHT_INFO* Info)
{
    PAGED_CODE();
    const USHORT v = (USHORT)((ULONG)static_cast<BASIC_DISPLAY_DRIVER*>(Context)->EpiBrightnessGet() * 0xFFFF / 100);
    Info->BacklightUsersetting = v;
    Info->BacklightEffective = v;
    for (ULONG i = 0; i < 256; i++)
    {
        const WORD g = (WORD)(i * 0x101);
        Info->GammaRamp.Red[i] = g;
        Info->GammaRamp.Green[i] = g;
        Info->GammaRamp.Blue[i] = g;
    }
    return STATUS_SUCCESS;
}

NTSTATUS EpiBrightnessQueryInterface(_In_ PVOID Context, _Inout_ PQUERY_INTERFACE pQi)
{
    PAGED_CODE();
    if (pQi->InterfaceType == NULL || pQi->Interface == NULL) { return STATUS_NOT_SUPPORTED; }
    if ((RtlCompareMemory(pQi->InterfaceType, &GUID_DEVINTERFACE_BRIGHTNESS_2, sizeof(GUID)) == sizeof(GUID)) &&
        pQi->Version == DXGK_BRIGHTNESS_INTERFACE_VERSION_2 && pQi->Size >= sizeof(DXGK_BRIGHTNESS_INTERFACE_2))
    {
        DXGK_BRIGHTNESS_INTERFACE_2* p = reinterpret_cast<DXGK_BRIGHTNESS_INTERFACE_2*>(pQi->Interface);
        p->Size = sizeof(*p);
        p->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_2;
        p->Context = Context;
        p->InterfaceReference = EpiBlRef;
        p->InterfaceDereference = EpiBlDeref;
        p->GetPossibleBrightness = EpiBlGetPossible;
        p->SetBrightness = EpiBlSet;
        p->GetBrightness = EpiBlGet;
        p->GetBrightnessCaps = EpiBlGetCaps;
        p->SetBrightnessState = EpiBlSetState;
        p->SetBacklightOptimization = EpiBlSetOptimization;
        p->GetBacklightReduction = EpiBlGetReduction;
        return STATUS_SUCCESS;
    }
    if ((RtlCompareMemory(pQi->InterfaceType, &GUID_DEVINTERFACE_BRIGHTNESS, sizeof(GUID)) == sizeof(GUID)) &&
        pQi->Version == DXGK_BRIGHTNESS_INTERFACE_VERSION_1 && pQi->Size >= sizeof(DXGK_BRIGHTNESS_INTERFACE))
    {
        DXGK_BRIGHTNESS_INTERFACE* p = reinterpret_cast<DXGK_BRIGHTNESS_INTERFACE*>(pQi->Interface);
        p->Size = sizeof(*p);
        p->Version = DXGK_BRIGHTNESS_INTERFACE_VERSION_1;
        p->Context = Context;
        p->InterfaceReference = EpiBlRef;
        p->InterfaceDereference = EpiBlDeref;
        p->GetPossibleBrightness = EpiBlGetPossible;
        p->SetBrightness = EpiBlSet;
        p->GetBrightness = EpiBlGet;
        return STATUS_SUCCESS;
    }
    return STATUS_NOT_SUPPORTED;
}
