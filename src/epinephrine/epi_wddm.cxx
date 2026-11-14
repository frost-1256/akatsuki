// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - W-1: full WDDM 1.3 driver mode (G0_GPU_SURVEY section 9.21.72).
//
// Portions derived from the render-only sample (roskmd: RosKmdGlobal.cpp, RosKmdAdapter.cpp, RosKmdDevice.cpp,
// RosKmdContext.cpp) in microsoft/graphics-driver-samples (commit de4a216):
//   Copyright (c) Microsoft Corporation. MIT License (full text in LICENSE-ROS-MIT.txt).
// Changes from ROS: VC4 hardware, WPP tracing, ACPI and power components removed; one adapter object (the
// existing BASIC_DISPLAY_DRIVER) keeps the display side; private data checked before use; the CPU engine
// reports preemption at DMA buffer boundaries; a flip is shown by copying the primary to the UEFI framebuffer.
//
// Touches no GPU, SMMU or GMU register.

#include "BDD.hxx"
#include "epi_wddm.hxx"

#define EPI_WDDM_TAG 'dWpE'

// ntifs.h (documented; not declared by the headers this driver includes)
extern "C" NTSYSAPI NTSTATUS NTAPI ZwWaitForSingleObject(_In_ HANDLE Handle, _In_ BOOLEAN Alertable,
                                                          _In_opt_ PLARGE_INTEGER Timeout);

// ---- driver private objects ----

#define EPI_DMABUF_MAGIC 0x42445045u   // 'EPDB'
#define EPI_DMA_RENDER   1
#define EPI_DMA_PAGING   2
#define EPI_DMA_PRESENT  3
// DMA buffer private data (DmaBufferPrivateDataSize of every context and PagingBufferPrivateDataSize).
typedef struct _EPI_DMABUF_INFO
{
    ULONG  Magic;
    ULONG  Kind;                 // EPI_DMA_*
    PUCHAR Buffer;               // CPU address of the start of the DMA buffer
    ULONG  Size;
} EPI_DMABUF_INFO;

typedef struct _EPI_DMA_SUB
{
    LIST_ENTRY Entry;
    PUCHAR     Buffer;
    ULONG      Size;
    ULONG      Start, End;
    UINT       Fence;
    ULONG      Kind;
    BOOLEAN    Paging;
} EPI_DMA_SUB;

#define EPI_DEV_MAGIC   0x56445045u    // 'EPDV'
#define EPI_CTX_MAGIC   0x54435045u    // 'EPCT'
#define EPI_ALLOC_OBJ   0x4F415045u    // 'EPAO'
#define EPI_DALLOC_OBJ  0x41445045u    // 'EPDA'

typedef struct _EPI_WDDM_DEVICE
{
    ULONG     Magic;
    EPI_WDDM* W;
    HANDLE    hRtDevice;
    DXGK_CREATEDEVICEFLAGS Flags;
} EPI_WDDM_DEVICE;

typedef struct _EPI_WDDM_CONTEXT
{
    ULONG            Magic;
    EPI_WDDM_DEVICE* Dev;
    UINT             Node;
    HANDLE           hRtContext;
    DXGK_CREATECONTEXTFLAGS Flags;
} EPI_WDDM_CONTEXT;

typedef struct _EPI_ALLOCATION
{
    ULONG          Magic;
    EPI_ALLOC_DESC Desc;
} EPI_ALLOCATION;

typedef struct _EPI_DEVICE_ALLOCATION
{
    ULONG           Magic;
    D3DKMT_HANDLE   hKm;
    EPI_ALLOCATION* Alloc;
} EPI_DEVICE_ALLOCATION;

typedef struct _EPI_RESOURCE
{
    ULONG Magic;
} EPI_RESOURCE;

struct _EPI_WDDM
{
    BASIC_DISPLAY_DRIVER*    Bdd;
    const DXGKRNL_INTERFACE* Dxgk;
    PDEVICE_OBJECT           Pdo;
    EPI_VSYNC*               Vsync;
    // Video memory segment
    PUCHAR           VidMem;
    PHYSICAL_ADDRESS VidMemPa;
    SIZE_T           VidMemSize;
    PFN_NUMBER       Aperture[EPI_WDDM_APERTURE_PAGES];   // bookkeeping only (DMA buffers are used by CPU address)
    // CPU engine queue (QueueLock, <= DISPATCH_LEVEL)
    KSPIN_LOCK  QueueLock;
    LIST_ENTRY  Queue;
    LIST_ENTRY  Free;
    EPI_DMA_SUB Subs[EPI_WDDM_QUEUE_LENGTH];
    BOOLEAN     PreemptPending;
    UINT        PreemptFence;
    LONG        ResetEpoch;
    volatile LONG LastDoneFence;
    // Worker
    KEVENT        WorkEvent;
    PKTHREAD      Thread;
    volatile LONG Exit;
    DXGKARGCB_NOTIFY_INTERRUPT_DATA Notify;   // used by the worker only
    // Flip -> CPU scanout. SetVidPnSourceAddress (any IRQL up to DIRQL) writes Flip under the FlipSeq seqlock
    // (odd while writing) and queues FlipDpc, which wakes the worker.
    KDPC          FlipDpc;
    volatile LONG FlipSeq;
    struct
    {
        UINT     Source;
        UINT     Segment;
        LONGLONG Address;
        ULONG    Width, Height, Pitch;
    } Flip;
    LONG   ScanDoneSeq;
    KEVENT ScanLock;             // synchronization event used as a PASSIVE_LEVEL mutex: scanout vs CommitVidPn
    // Diagnostics
    EPI_WDDM_TRACE T;
    volatile LONG  Changes;
};

// ---- diagnostics helpers (any IRQL) ----

#define EPI_T(W, f)   (*(volatile LONG*)&(W)->T.f)
static __forceinline VOID EpiCnt(_Inout_ volatile LONG* p, _Inout_ EPI_WDDM* W)
{
    InterlockedIncrement(p);
    InterlockedIncrement(&W->Changes);
}
#define EPI_COUNT(W, f) EpiCnt(&EPI_T(W, f), (W))
static __forceinline VOID EpiDdi(_Inout_ EPI_WDDM* W, ULONG Id)
{
    EpiCnt((volatile LONG*)&W->T.Ddi[Id], W);
}
static VOID EpiFail(_Inout_ EPI_WDDM* W, ULONG Id, NTSTATUS St)
{
    InterlockedIncrement(&EPI_T(W, FailCount));
    InterlockedExchange(&EPI_T(W, LastFailDdi), (LONG)Id);
    InterlockedExchange(&EPI_T(W, LastFailStatus), (LONG)St);
    InterlockedIncrement(&W->Changes);
}

static __forceinline EPI_WDDM* EpiW(_In_opt_ HANDLE hAdapter)
{
    return hAdapter ? reinterpret_cast<BASIC_DISPLAY_DRIVER*>(hAdapter)->m_Wddm : NULL;
}
static __forceinline EPI_WDDM_DEVICE* EpiDev(_In_opt_ HANDLE hDevice)
{
    EPI_WDDM_DEVICE* d = static_cast<EPI_WDDM_DEVICE*>(hDevice);
    return (d != NULL && d->Magic == EPI_DEV_MAGIC) ? d : NULL;
}
static __forceinline EPI_WDDM_CONTEXT* EpiCtx(_In_opt_ HANDLE hContext)
{
    EPI_WDDM_CONTEXT* c = static_cast<EPI_WDDM_CONTEXT*>(hContext);
    return (c != NULL && c->Magic == EPI_CTX_MAGIC) ? c : NULL;
}
static __forceinline EPI_ALLOCATION* EpiAlloc(_In_opt_ HANDLE hAllocation)
{
    EPI_ALLOCATION* a = static_cast<EPI_ALLOCATION*>(hAllocation);
    return (a != NULL && a->Magic == EPI_ALLOC_OBJ) ? a : NULL;
}

static __forceinline BOOLEAN EpiIs32bpp(D3DDDIFORMAT f)
{
    return (f == D3DDDIFMT_A8R8G8B8 || f == D3DDDIFMT_X8R8G8B8 || f == D3DDDIFMT_A8B8G8R8 || f == D3DDDIFMT_X8B8G8R8)
        ? TRUE : FALSE;
}

// ---- DDI declarations (the typedefs check the signatures) ----

DXGKDDI_QUERYADAPTERINFO               EpiWddmQueryAdapterInfo;
DXGKDDI_CREATEDEVICE                   EpiWddmCreateDevice;
DXGKDDI_DESTROYDEVICE                  EpiWddmDestroyDevice;
DXGKDDI_CREATECONTEXT                  EpiWddmCreateContext;
DXGKDDI_DESTROYCONTEXT                 EpiWddmDestroyContext;
DXGKDDI_CREATEALLOCATION               EpiWddmCreateAllocation;
DXGKDDI_DESTROYALLOCATION              EpiWddmDestroyAllocation;
DXGKDDI_DESCRIBEALLOCATION             EpiWddmDescribeAllocation;
DXGKDDI_GETSTANDARDALLOCATIONDRIVERDATA EpiWddmGetStandardAllocationDriverData;
DXGKDDI_OPENALLOCATIONINFO             EpiWddmOpenAllocation;
DXGKDDI_CLOSEALLOCATION                EpiWddmCloseAllocation;
DXGKDDI_RENDER                         EpiWddmRender;
DXGKDDI_RENDERKM                       EpiWddmRenderKm;
DXGKDDI_PRESENT                        EpiWddmPresent;
DXGKDDI_PATCH                          EpiWddmPatch;
DXGKDDI_SUBMITCOMMAND                  EpiWddmSubmitCommand;
DXGKDDI_BUILDPAGINGBUFFER              EpiWddmBuildPagingBuffer;
DXGKDDI_PREEMPTCOMMAND                 EpiWddmPreemptCommand;
DXGKDDI_QUERYCURRENTFENCE              EpiWddmQueryCurrentFence;
DXGKDDI_RESETFROMTIMEOUT               EpiWddmResetFromTimeout;
DXGKDDI_RESTARTFROMTIMEOUT             EpiWddmRestartFromTimeout;
DXGKDDI_ESCAPE                         EpiWddmEscape;
DXGKDDI_COLLECTDBGINFO                 EpiWddmCollectDbgInfo;
DXGKDDI_SETVIDPNSOURCEADDRESS          EpiWddmSetVidPnSourceAddress;
DXGKDDI_SETPALETTE                     EpiWddmSetPalette;
DXGKDDI_GETNODEMETADATA                EpiWddmGetNodeMetadata;
DXGKDDI_COMMITVIDPN                    EpiWddmCommitVidPn;

// ---- worker ----

static BOOLEAN EpiWddmNotifySync(_In_opt_ PVOID Context)   // DIRQL, synchronized with the ISR
{
    EPI_WDDM* W = static_cast<EPI_WDDM*>(Context);
    W->Dxgk->DxgkCbNotifyInterrupt(W->Dxgk->DeviceHandle, &W->Notify);
    return W->Dxgk->DxgkCbQueueDpc(W->Dxgk->DeviceHandle);
}

static VOID EpiWddmNotify(_Inout_ EPI_WDDM* W)
{
    BOOLEAN ret = FALSE;
    const NTSTATUS st = W->Dxgk->DxgkCbSynchronizeExecution(W->Dxgk->DeviceHandle, EpiWddmNotifySync, W, 0, &ret);
    if (!NT_SUCCESS(st)) { EPI_COUNT(W, NotifyFail); }
}

// One side of a paging TRANSFER: video memory segment or system memory (MDL). Returns NULL when out of range.
static PUCHAR EpiWddmXferSide(_In_ EPI_WDDM* W, UINT Segment, LONGLONG Address, _In_opt_ MDL* Mdl, UINT MdlOffset,
                              SIZE_T Size, _Inout_ PMDL* pUnmapMdl, _Inout_ PVOID* pUnmapVa)
{
    if (Segment == EPI_WDDM_SEG_VIDMEM)
    {
        if (Address < 0 || (ULONGLONG)Address > W->VidMemSize || Size > W->VidMemSize - (SIZE_T)Address) { return NULL; }
        return W->VidMem + Address;
    }
    if (Segment != 0 || Mdl == NULL) { return NULL; }
    const SIZE_T off = (SIZE_T)MdlOffset * PAGE_SIZE;
    const SIZE_T bytes = MmGetMdlByteCount(Mdl);
    if (off > bytes || Size > bytes - off) { return NULL; }
    const BOOLEAN wasMapped = (Mdl->MdlFlags & (MDL_MAPPED_TO_SYSTEM_VA | MDL_SOURCE_IS_NONPAGED_POOL)) ? TRUE : FALSE;
    PUCHAR va = static_cast<PUCHAR>(MmGetSystemAddressForMdlSafe(Mdl, HighPagePriority | MdlMappingNoExecute));
    if (va == NULL) { return NULL; }
    if (!wasMapped) { *pUnmapMdl = Mdl; *pUnmapVa = va; }
    return va + off;
}

// Paging buffer: DXGKARG_BUILDPAGINGBUFFER records written by EpiWddmBuildPagingBuffer (as in ROS).
static VOID EpiWddmRunPaging(_Inout_ EPI_WDDM* W, _In_ const EPI_DMA_SUB* S)
{
    if (S->End <= S->Start) { return; }   // fence only
    if (S->Kind != EPI_DMA_PAGING || S->Buffer == NULL || S->End > S->Size ||
        ((S->End - S->Start) % sizeof(DXGKARG_BUILDPAGINGBUFFER)) != 0)
    {
        EPI_COUNT(W, PagingError);
        return;
    }
    EPI_COUNT(W, PagingBuffers);
    const DXGKARG_BUILDPAGINGBUFFER* op = reinterpret_cast<const DXGKARG_BUILDPAGINGBUFFER*>(S->Buffer + S->Start);
    const DXGKARG_BUILDPAGINGBUFFER* end = reinterpret_cast<const DXGKARG_BUILDPAGINGBUFFER*>(S->Buffer + S->End);
    for (; op < end; ++op)
    {
        switch (op->Operation)
        {
        case DXGK_OPERATION_FILL:
        {
            const LONGLONG addr = op->Fill.Destination.SegmentAddress.QuadPart;
            const SIZE_T size = op->Fill.FillSize;
            if (op->Fill.Destination.SegmentId != EPI_WDDM_SEG_VIDMEM || addr < 0 || (size % sizeof(ULONG)) != 0 ||
                (ULONGLONG)addr > W->VidMemSize || size > W->VidMemSize - (SIZE_T)addr || (addr % sizeof(ULONG)) != 0)
            {
                EPI_COUNT(W, PagingError);
                break;
            }
            ULONG* p = reinterpret_cast<ULONG*>(W->VidMem + addr);
            for (SIZE_T i = 0; i < size / sizeof(ULONG); ++i) { p[i] = op->Fill.FillPattern; }
            break;
        }
        case DXGK_OPERATION_TRANSFER:
        {
            // ROS ignores TransferOffset; W-1a does the same and counts the transfers where it is not 0.
            if (op->Transfer.TransferOffset != 0) { EPI_COUNT(W, PagingXferOffset); }
            const SIZE_T size = op->Transfer.TransferSize;
            PMDL unmapMdl[2] = { NULL, NULL };
            PVOID unmapVa[2] = { NULL, NULL };
            PUCHAR src = EpiWddmXferSide(W, op->Transfer.Source.SegmentId, op->Transfer.Source.SegmentAddress.QuadPart,
                                         op->Transfer.Source.SegmentId == 0 ? op->Transfer.Source.pMdl : NULL,
                                         op->Transfer.MdlOffset, size, &unmapMdl[0], &unmapVa[0]);
            PUCHAR dst = EpiWddmXferSide(W, op->Transfer.Destination.SegmentId,
                                         op->Transfer.Destination.SegmentAddress.QuadPart,
                                         op->Transfer.Destination.SegmentId == 0 ? op->Transfer.Destination.pMdl : NULL,
                                         op->Transfer.MdlOffset, size, &unmapMdl[1], &unmapVa[1]);
            if (src != NULL && dst != NULL) { RtlCopyMemory(dst, src, size); }
            else { EPI_COUNT(W, PagingError); }
            for (ULONG i = 0; i < 2; ++i) { if (unmapMdl[i] != NULL) { MmUnmapLockedPages(unmapVa[i], unmapMdl[i]); } }
            break;
        }
        default:
            EPI_COUNT(W, PagingError);   // only FILL and TRANSFER are written into the buffer
            break;
        }
    }
}

static VOID EpiWddmExecute(_Inout_ EPI_WDDM* W, _In_ const EPI_DMA_SUB* S)
{
    if (S->Paging) { EpiWddmRunPaging(W, S); return; }
    // W-1a: no render commands exist yet (no UMD); render and present DMA buffers complete without work.
    if (S->Kind == EPI_DMA_PRESENT) { EPI_COUNT(W, PresentDone); }
    else { EPI_COUNT(W, RenderDone); }
}

static VOID EpiWddmRunQueue(_Inout_ EPI_WDDM* W)
{
    for (;;)
    {
        KIRQL irql;
        KeAcquireSpinLock(&W->QueueLock, &irql);
        if (W->PreemptPending)
        {
            // Everything still queued was dropped by EpiWddmPreemptCommand; the buffer that was running (if any)
            // has completed and been reported, so LastDoneFence is the last completed fence.
            const UINT fence = W->PreemptFence;
            W->PreemptPending = FALSE;
            KeReleaseSpinLock(&W->QueueLock, irql);
            RtlZeroMemory(&W->Notify, sizeof(W->Notify));
            W->Notify.InterruptType = DXGK_INTERRUPT_DMA_PREEMPTED;
            W->Notify.DmaPreempted.PreemptionFenceId = fence;
            W->Notify.DmaPreempted.LastCompletedFenceId = (UINT)W->LastDoneFence;
            EpiWddmNotify(W);
            EPI_COUNT(W, Preempted);
            continue;
        }
        if (IsListEmpty(&W->Queue))
        {
            KeReleaseSpinLock(&W->QueueLock, irql);
            return;
        }
        EPI_DMA_SUB* s = CONTAINING_RECORD(RemoveHeadList(&W->Queue), EPI_DMA_SUB, Entry);
        const LONG epoch = W->ResetEpoch;
        KeReleaseSpinLock(&W->QueueLock, irql);

        EpiWddmExecute(W, s);

        KeAcquireSpinLock(&W->QueueLock, &irql);
        const BOOLEAN report = (epoch == W->ResetEpoch) ? TRUE : FALSE;   // not after ResetFromTimeout
        const UINT fence = s->Fence;
        InsertTailList(&W->Free, &s->Entry);
        if (report) { InterlockedExchange(&W->LastDoneFence, (LONG)fence); }
        KeReleaseSpinLock(&W->QueueLock, irql);
        if (report)
        {
            RtlZeroMemory(&W->Notify, sizeof(W->Notify));
            W->Notify.InterruptType = DXGK_INTERRUPT_DMA_COMPLETED;
            W->Notify.DmaCompleted.SubmissionFenceId = fence;
            W->Notify.DmaCompleted.NodeOrdinal = 0;
            W->Notify.DmaCompleted.EngineOrdinal = 0;
            EpiWddmNotify(W);
            InterlockedExchange(&EPI_T(W, LastDoneFence), (LONG)fence);
            EPI_COUNT(W, DmaCompleted);
        }
    }
}

// CPU scanout: copy the flipped primary from the video memory segment to the UEFI framebuffer, then let the
// VSync report the new address (the flip completes at the next VSync after the copy).
static VOID EpiWddmScanout(_Inout_ EPI_WDDM* W)
{
    const LONG s1 = ReadAcquire(&W->FlipSeq);
    if ((s1 & 1) != 0 || s1 == W->ScanDoneSeq) { return; }   // writer active: its DPC wakes us again
    const UINT source = W->Flip.Source;
    const UINT segment = W->Flip.Segment;
    const LONGLONG address = W->Flip.Address;
    const ULONG width = W->Flip.Width, height = W->Flip.Height, pitch = W->Flip.Pitch;
    KeMemoryBarrier();
    if (ReadAcquire(&W->FlipSeq) != s1) { return; }
    W->ScanDoneSeq = s1;

    BOOLEAN copied = FALSE;
    const ULONGLONG bytes = (ULONGLONG)pitch * height;
    if (segment == EPI_WDDM_SEG_VIDMEM && width != 0 && height != 0 && pitch >= width * 4ULL && address >= 0 &&
        (ULONGLONG)address <= W->VidMemSize && bytes <= W->VidMemSize - (SIZE_T)address)
    {
        const LONGLONG t0 = KeQueryPerformanceCounter(NULL).QuadPart;
        KeWaitForSingleObject(&W->ScanLock, Executive, KernelMode, FALSE, NULL);
        CURRENT_BDD_MODE* m = W->Bdd->EpiModeMut(source);
        if (m != NULL && m->FrameBuffer.Ptr != NULL && m->Flags.FrameBufferIsActive && m->DispInfo.Pitch != 0)
        {
            const ULONG rows = min(height, m->DispInfo.Height);
            ULONG rowBytes = min(width, m->DispInfo.Width) * 4;
            rowBytes = min(rowBytes, m->DispInfo.Pitch);
            const PUCHAR src = W->VidMem + address;
            PUCHAR dst = static_cast<PUCHAR>(m->FrameBuffer.Ptr);
            for (ULONG y = 0; y < rows; ++y)
            {
                RtlCopyMemory(dst + (SIZE_T)y * m->DispInfo.Pitch, src + (SIZE_T)y * pitch, rowBytes);
            }
            copied = TRUE;
        }
        KeSetEvent(&W->ScanLock, 0, FALSE);
        if (copied)
        {
            LARGE_INTEGER f;
            const LONGLONG t1 = KeQueryPerformanceCounter(&f).QuadPart;
            const ULONG us = f.QuadPart ? (ULONG)((t1 - t0) * 1000000 / f.QuadPart) : 0;
            if (us > W->T.ScanMaxUs) { InterlockedExchange(&EPI_T(W, ScanMaxUs), (LONG)us); }
        }
    }
    if (copied) { EPI_COUNT(W, ScanCopies); } else { EPI_COUNT(W, ScanSkipped); }
    InterlockedExchange64(&W->Vsync->CrtcAddress, address);
}

static VOID EpiWddmWriteTrace(_Inout_ EPI_WDDM* W)
{
    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(W->Pdo, &hParams, &hDiag))) { return; }
    W->T.Writes++;
    W->T.VsyncNotifies = (ULONG)W->Vsync->Notifies;
    const EPI_WDDM_TRACE copy = W->T;   // counters may move while copying; one write may be slightly stale
    EpiSetValue(hDiag, L"WddmTrace", REG_BINARY, &copy, sizeof(copy));
    ZwClose(hDiag);
    ZwClose(hParams);
}

static KSTART_ROUTINE EpiWddmWorker;
static VOID EpiWddmWorker(_In_ PVOID Context)
{
    EPI_WDDM* W = static_cast<EPI_WDDM*>(Context);
    LONG written = -1;
    ULONGLONG lastTrace = 0;
    for (;;)
    {
        LARGE_INTEGER timeout;
        timeout.QuadPart = -10000000LL;   // 1 s
        KeWaitForSingleObject(&W->WorkEvent, Executive, KernelMode, FALSE, &timeout);
        if (W->Exit) { break; }
        EpiWddmRunQueue(W);
        EpiWddmScanout(W);
        const ULONGLONG now = KeQueryInterruptTime();
        if (now - lastTrace >= 10000000ULL)
        {
            lastTrace = now;
            const LONG changes = W->Changes;
            if (changes != written)
            {
                written = changes;
                EpiWddmWriteTrace(W);
            }
        }
    }
    PsTerminateSystemThread(STATUS_SUCCESS);
}

static KDEFERRED_ROUTINE EpiWddmFlipDpc;
static VOID EpiWddmFlipDpc(_In_ PKDPC Dpc, _In_opt_ PVOID Context, _In_opt_ PVOID A1, _In_opt_ PVOID A2)
{
    UNREFERENCED_PARAMETER(Dpc);
    UNREFERENCED_PARAMETER(A1);
    UNREFERENCED_PARAMETER(A2);
    EPI_WDDM* W = static_cast<EPI_WDDM*>(Context);
    KeSetEvent(&W->WorkEvent, 0, FALSE);
}

// ---- start / stop ----

NTSTATUS EpiWddmStart(_In_ BASIC_DISPLAY_DRIVER* Bdd, _Outptr_ EPI_WDDM** ppW)
{
    *ppW = NULL;
    EPI_WDDM* W = static_cast<EPI_WDDM*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(EPI_WDDM), EPI_WDDM_TAG));
    if (W == NULL) { return STATUS_NO_MEMORY; }
    W->Bdd = Bdd;
    W->Dxgk = Bdd->GetDxgkInterface();
    W->Pdo = Bdd->EpiPdo();
    W->Vsync = Bdd->EpiVsyncMut();
    W->T.Version = 1;
    W->T.Mode = g_EpiWddmMode;

    PHYSICAL_ADDRESS low, high, skip;
    low.QuadPart = 0;
    high.QuadPart = -1;
    skip.QuadPart = 0;
    for (SIZE_T size = EPI_WDDM_VIDMEM_MAX; size >= EPI_WDDM_VIDMEM_MIN; size /= 2)
    {
        W->VidMem = static_cast<PUCHAR>(MmAllocateContiguousMemorySpecifyCache(size, low, high, skip, MmCached));
        if (W->VidMem != NULL) { W->VidMemSize = size; break; }
    }
    if (W->VidMem == NULL)
    {
        ExFreePoolWithTag(W, EPI_WDDM_TAG);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(W->VidMem, W->VidMemSize);
    W->VidMemPa = MmGetPhysicalAddress(W->VidMem);
    W->T.VidMemBytes = (ULONG)W->VidMemSize;
    W->T.VidMemPaLow = W->VidMemPa.LowPart;
    W->T.VidMemPaHigh = (ULONG)W->VidMemPa.HighPart;

    KeInitializeSpinLock(&W->QueueLock);
    InitializeListHead(&W->Queue);
    InitializeListHead(&W->Free);
    for (ULONG i = 0; i < EPI_WDDM_QUEUE_LENGTH; ++i) { InsertTailList(&W->Free, &W->Subs[i].Entry); }
    KeInitializeEvent(&W->WorkEvent, SynchronizationEvent, FALSE);
    KeInitializeEvent(&W->ScanLock, SynchronizationEvent, TRUE);
    KeInitializeDpc(&W->FlipDpc, EpiWddmFlipDpc, W);

    OBJECT_ATTRIBUTES oa;
    InitializeObjectAttributes(&oa, NULL, OBJ_KERNEL_HANDLE, NULL, NULL);
    HANDLE hThread = NULL;
    NTSTATUS st = PsCreateSystemThread(&hThread, THREAD_ALL_ACCESS, &oa, NULL, NULL, EpiWddmWorker, W);
    if (NT_SUCCESS(st))
    {
        st = ObReferenceObjectByHandle(hThread, THREAD_ALL_ACCESS, *PsThreadType, KernelMode,
                                       reinterpret_cast<PVOID*>(&W->Thread), NULL);
        if (!NT_SUCCESS(st))
        {
            // The thread runs but cannot be waited for: stop it before freeing anything.
            InterlockedExchange(&W->Exit, 1);
            KeSetEvent(&W->WorkEvent, 0, FALSE);
            ZwWaitForSingleObject(hThread, FALSE, NULL);
        }
        ZwClose(hThread);
    }
    if (!NT_SUCCESS(st))
    {
        MmFreeContiguousMemorySpecifyCache(W->VidMem, W->VidMemSize, MmCached);
        ExFreePoolWithTag(W, EPI_WDDM_TAG);
        return st;
    }
    *ppW = W;
    InterlockedIncrement(&W->Changes);
    return STATUS_SUCCESS;
}

VOID EpiWddmStop(_Inout_ EPI_WDDM** ppW)
{
    EPI_WDDM* W = *ppW;
    if (W == NULL) { return; }
    InterlockedExchange(&W->Exit, 1);
    KeSetEvent(&W->WorkEvent, 0, FALSE);
    KeWaitForSingleObject(W->Thread, Executive, KernelMode, FALSE, NULL);
    ObDereferenceObject(W->Thread);
    W->Thread = NULL;
    KeRemoveQueueDpc(&W->FlipDpc);
    KeFlushQueuedDpcs();
    EpiWddmWriteTrace(W);
    *ppW = NULL;
    MmFreeContiguousMemorySpecifyCache(W->VidMem, W->VidMemSize, MmCached);
    ExFreePoolWithTag(W, EPI_WDDM_TAG);
}

// ---- adapter DDIs ----

NTSTATUS APIENTRY EpiWddmQueryAdapterInfo(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_QUERYADAPTERINFO p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }   // never the display-only caps in full mode
    EpiDdi(W, EpiDdiQueryAdapterInfo);
    if ((ULONG)p->Type < 32) { InterlockedOr(&EPI_T(W, QaiMask), (LONG)(1u << (ULONG)p->Type)); }

    switch (p->Type)
    {
    case DXGKQAITYPE_DRIVERCAPS:
    {
        if (p->OutputDataSize < sizeof(DXGK_DRIVERCAPS)) { EpiFail(W, EpiDdiQueryAdapterInfo, STATUS_BUFFER_TOO_SMALL); return STATUS_BUFFER_TOO_SMALL; }
        DXGK_DRIVERCAPS* c = static_cast<DXGK_DRIVERCAPS*>(p->pOutputData);
        RtlZeroMemory(c, sizeof(*c));
        c->HighestAcceptableAddress.QuadPart = -1;
        c->PresentationCaps.SupportSoftwareDeviceBitmaps = TRUE;
        c->PresentationCaps.NoScreenToScreenBlt = TRUE;      // ROS: screen to screen blt is slow
        c->PresentationCaps.NoOverlapScreenBlt = TRUE;
        c->PresentationCaps.MaxTextureWidthShift = 3;        // 16K x 16K redirection bitmaps
        c->PresentationCaps.MaxTextureHeightShift = 3;
        // Flip by MMIO at VSync, at most one pending (ROS). Here the "MMIO" is the CPU scanout copy, and the flip
        // completes when the VSync after the copy reports the new address.
        c->FlipCaps.FlipOnVSyncMmIo = TRUE;
        c->MaxQueuedFlipOnVSync = 1;
        c->FlipCaps.FlipOnVSyncWithNoWait = TRUE;
        c->FlipCaps.FlipIndependent = TRUE;                  // required for WDDM 1.3
        c->SchedulingCaps.MultiEngineAware = 1;
        c->SchedulingCaps.PreemptionAware = 1;
        c->GpuEngineTopology.NbAsymetricProcessingNodes = 1;
        c->WDDMVersion = DXGKDDI_WDDMv1_3;
        c->PreemptionCaps.GraphicsPreemptionGranularity = D3DKMDT_GRAPHICS_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->PreemptionCaps.ComputePreemptionGranularity = D3DKMDT_COMPUTE_PREEMPTION_DMA_BUFFER_BOUNDARY;
        c->SupportNonVGA = TRUE;                             // DxgkDdiStopDeviceAndReleasePostDisplayOwnership
        c->SupportSmoothRotation = TRUE;
        c->SupportDirectFlip = 1;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_QUERYSEGMENT3:
    {
        if (p->OutputDataSize < sizeof(DXGK_QUERYSEGMENTOUT3)) { EpiFail(W, EpiDdiQueryAdapterInfo, STATUS_BUFFER_TOO_SMALL); return STATUS_BUFFER_TOO_SMALL; }
        DXGK_QUERYSEGMENTOUT3* q = static_cast<DXGK_QUERYSEGMENTOUT3*>(p->pOutputData);
        if (q->pSegmentDescriptor == NULL)
        {
            q->NbSegment = 2;
            return STATUS_SUCCESS;
        }
        q->PagingBufferPrivateDataSize = sizeof(EPI_DMABUF_INFO);   // same as DmaBufferPrivateDataSize
        q->PagingBufferSegmentId = EPI_WDDM_SEG_APERTURE;
        q->PagingBufferSize = EPI_WDDM_PAGING_BUFFER_SIZE;
        DXGK_SEGMENTDESCRIPTOR3* d = q->pSegmentDescriptor;
        RtlZeroMemory(&d[0], sizeof(d[0]));
        d[0].Flags.Aperture = TRUE;
        d[0].Flags.CacheCoherent = TRUE;
        d[0].Flags.CpuVisible = TRUE;                      // legacy requirement (ROS); never mapped
        d[0].BaseAddress.QuadPart = EPI_WDDM_APERTURE_BASE;
        d[0].CpuTranslatedAddress.QuadPart = (LONGLONG)0xFFFFFFFE00000000ULL;
        d[0].Size = (SIZE_T)EPI_WDDM_APERTURE_PAGES * PAGE_SIZE;
        d[0].CommitLimit = d[0].Size;
        RtlZeroMemory(&d[1], sizeof(d[1]));
        d[1].BaseAddress.QuadPart = 0;
        d[1].Flags.CpuVisible = TRUE;
        d[1].Flags.CacheCoherent = TRUE;
        d[1].Flags.DirectFlip = TRUE;
        d[1].CpuTranslatedAddress = W->VidMemPa;
        d[1].Size = W->VidMemSize;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_UMDRIVERPRIVATE:
    {
        if (p->OutputDataSize < sizeof(EPI_UMD_ADAPTER_INFO)) { EpiFail(W, EpiDdiQueryAdapterInfo, STATUS_BUFFER_TOO_SMALL); return STATUS_BUFFER_TOO_SMALL; }
        EPI_UMD_ADAPTER_INFO* u = static_cast<EPI_UMD_ADAPTER_INFO*>(p->pOutputData);
        u->Magic = 0x4D555045u;   // 'EPUM'
        u->Version = 1;
        u->WddmVersion = DXGKDDI_WDDMv1_3;
        u->VidMemBytes = (ULONG)W->VidMemSize;
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_HISTORYBUFFERPRECISION:
    {
        const UINT n = p->OutputDataSize / sizeof(DXGKARG_HISTORYBUFFERPRECISION);
        for (UINT i = 0; i < n; ++i)
        {
            static_cast<DXGKARG_HISTORYBUFFERPRECISION*>(p->pOutputData)[i].PrecisionBits = 64;
        }
        return STATUS_SUCCESS;
    }
    case DXGKQAITYPE_DISPLAY_DRIVERCAPS_EXTENSION:
        return BddDdiQueryAdapterInfo(hAdapter, p);
    default:
        InterlockedIncrement(&EPI_T(W, QaiUnknownCount));
        InterlockedExchange(&EPI_T(W, QaiUnknownLast), (LONG)p->Type);
        return STATUS_NOT_SUPPORTED;
    }
}

// ---- device / context (RosKmdDevice.cpp, RosKmdContext.cpp) ----

NTSTATUS APIENTRY EpiWddmCreateDevice(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_CREATEDEVICE p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiCreateDevice);
    EPI_WDDM_DEVICE* d = static_cast<EPI_WDDM_DEVICE*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*d), EPI_WDDM_TAG));
    if (d == NULL) { EpiFail(W, EpiDdiCreateDevice, STATUS_NO_MEMORY); return STATUS_NO_MEMORY; }
    d->Magic = EPI_DEV_MAGIC;
    d->W = W;
    d->hRtDevice = p->hDevice;
    d->Flags = p->Flags;
    p->hDevice = d;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmDestroyDevice(IN_CONST_HANDLE hDevice)
{
    EPI_WDDM_DEVICE* d = EpiDev(hDevice);
    if (d == NULL) { return STATUS_INVALID_PARAMETER; }
    EpiDdi(d->W, EpiDdiDestroyDevice);
    d->Magic = 0;
    ExFreePoolWithTag(d, EPI_WDDM_TAG);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmCreateContext(IN_CONST_HANDLE hDevice, INOUT_PDXGKARG_CREATECONTEXT p)
{
    EPI_WDDM_DEVICE* d = EpiDev(hDevice);
    if (d == NULL) { return STATUS_INVALID_PARAMETER; }
    EPI_WDDM* W = d->W;
    EpiDdi(W, EpiDdiCreateContext);
    if (p->NodeOrdinal != 0) { EpiFail(W, EpiDdiCreateContext, STATUS_INVALID_PARAMETER); return STATUS_INVALID_PARAMETER; }
    EPI_WDDM_CONTEXT* c = static_cast<EPI_WDDM_CONTEXT*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*c), EPI_WDDM_TAG));
    if (c == NULL) { EpiFail(W, EpiDdiCreateContext, STATUS_NO_MEMORY); return STATUS_NO_MEMORY; }
    c->Magic = EPI_CTX_MAGIC;
    c->Dev = d;
    c->Node = p->NodeOrdinal;
    c->hRtContext = p->hContext;
    c->Flags = p->Flags;

    DXGK_CONTEXTINFO* ci = &p->ContextInfo;
    ci->DmaBufferSegmentSet = 1 << (EPI_WDDM_SEG_APERTURE - 1);
    ci->DmaBufferSize = EPI_WDDM_DMA_BUFFER_SIZE;
    ci->DmaBufferPrivateDataSize = sizeof(EPI_DMABUF_INFO);
    if (p->Flags.GdiContext)
    {
        ci->AllocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
        ci->PatchLocationListSize = DXGK_ALLOCATION_LIST_SIZE_GDICONTEXT;
    }
    else
    {
        ci->AllocationListSize = EPI_WDDM_ALLOCATION_LIST;
        ci->PatchLocationListSize = EPI_WDDM_PATCH_LIST;
    }
    p->hContext = c;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmDestroyContext(IN_CONST_HANDLE hContext)
{
    EPI_WDDM_CONTEXT* c = EpiCtx(hContext);
    if (c == NULL) { return STATUS_INVALID_PARAMETER; }
    EpiDdi(c->Dev->W, EpiDdiDestroyContext);
    c->Magic = 0;
    ExFreePoolWithTag(c, EPI_WDDM_TAG);
    return STATUS_SUCCESS;
}

// ---- allocations (RosKmdAdapter.cpp) ----

NTSTATUS APIENTRY EpiWddmCreateAllocation(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_CREATEALLOCATION p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiCreateAllocation);

    EPI_RESOURCE* newRes = NULL;
    if (p->Flags.Resource && p->hResource == NULL)
    {
        newRes = static_cast<EPI_RESOURCE*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*newRes), EPI_WDDM_TAG));
        if (newRes == NULL) { EpiFail(W, EpiDdiCreateAllocation, STATUS_NO_MEMORY); return STATUS_NO_MEMORY; }
        newRes->Magic = EPI_RES_MAGIC;
    }

    NTSTATUS st = STATUS_SUCCESS;
    UINT done = 0;
    for (; done < p->NumAllocations; ++done)
    {
        DXGK_ALLOCATIONINFO* info = &p->pAllocationInfo[done];
        info->hAllocation = NULL;
        if (info->pPrivateDriverData == NULL || info->PrivateDriverDataSize != sizeof(EPI_ALLOC_DESC))
        {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        const EPI_ALLOC_DESC desc = *static_cast<const EPI_ALLOC_DESC*>(info->pPrivateDriverData);
        if (desc.Magic != EPI_ALLOC_MAGIC || desc.SizeBytes == 0 || desc.SizeBytes > W->VidMemSize)
        {
            st = STATUS_INVALID_PARAMETER;
            break;
        }
        EPI_ALLOCATION* a = static_cast<EPI_ALLOCATION*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*a), EPI_WDDM_TAG));
        if (a == NULL) { st = STATUS_NO_MEMORY; break; }
        a->Magic = EPI_ALLOC_OBJ;
        a->Desc = desc;

        info->hAllocation = a;
        info->Alignment = PAGE_SIZE;
        info->AllocationPriority = D3DDDI_ALLOCATIONPRIORITY_NORMAL;
        info->EvictionSegmentSet = 0;                       // do not evict into the aperture
        info->Flags.Value = 0;
        // ROS: shared allocations (including the primary) are not CPU visible unless they live only in an
        // aperture segment; allocations that are flipped are not cached.
        info->Flags.CpuVisible = (desc.Flags & (EPI_ALLOC_SHARED | EPI_ALLOC_PRIMARY)) ? 0 : 1;
        info->Flags.Cached = info->Flags.CpuVisible;
        info->HintedBank.Value = 0;
        info->MaximumRenamingListLength = 0;
        info->pAllocationUsageHint = NULL;
        info->PhysicalAdapterIndex = 0;
        info->PitchAlignedSize = 0;
        info->PreferredSegment.Value = 0;
        info->PreferredSegment.SegmentId0 = EPI_WDDM_SEG_VIDMEM;
        info->Size = desc.SizeBytes;
        info->SupportedReadSegmentSet = 1 << (EPI_WDDM_SEG_VIDMEM - 1);
        info->SupportedWriteSegmentSet = 1 << (EPI_WDDM_SEG_VIDMEM - 1);

        const LONG live = InterlockedIncrement(&EPI_T(W, AllocLive));
        if (live > (LONG)W->T.AllocMax) { InterlockedExchange(&EPI_T(W, AllocMax), live); }
        InterlockedExchangeAdd(&EPI_T(W, AllocBytesLive), (LONG)desc.SizeBytes);
        if (desc.Flags & EPI_ALLOC_PRIMARY) { EPI_COUNT(W, AllocPrimary); }
        if (desc.Flags & EPI_ALLOC_SHADOW) { EPI_COUNT(W, AllocShadow); }
        if (desc.Flags & EPI_ALLOC_STAGING) { EPI_COUNT(W, AllocStaging); }
    }
    if (!NT_SUCCESS(st))
    {
        if (st == STATUS_INVALID_PARAMETER) { EPI_COUNT(W, AllocBadData); }
        for (UINT i = 0; i < done; ++i)
        {
            EPI_ALLOCATION* a = EpiAlloc(p->pAllocationInfo[i].hAllocation);
            if (a == NULL) { continue; }
            InterlockedDecrement(&EPI_T(W, AllocLive));
            InterlockedExchangeAdd(&EPI_T(W, AllocBytesLive), -(LONG)a->Desc.SizeBytes);
            a->Magic = 0;
            ExFreePoolWithTag(a, EPI_WDDM_TAG);
            p->pAllocationInfo[i].hAllocation = NULL;
        }
        if (newRes != NULL) { ExFreePoolWithTag(newRes, EPI_WDDM_TAG); }
        EpiFail(W, EpiDdiCreateAllocation, st);
        return st;
    }
    if (newRes != NULL) { p->hResource = newRes; }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmDestroyAllocation(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_DESTROYALLOCATION p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiDestroyAllocation);
    for (UINT i = 0; i < p->NumAllocations; ++i)
    {
        EPI_ALLOCATION* a = EpiAlloc(p->pAllocationList[i]);
        if (a == NULL) { continue; }
        InterlockedDecrement(&EPI_T(W, AllocLive));
        InterlockedExchangeAdd(&EPI_T(W, AllocBytesLive), -(LONG)a->Desc.SizeBytes);
        a->Magic = 0;
        ExFreePoolWithTag(a, EPI_WDDM_TAG);
    }
    if (p->Flags.DestroyResource && p->hResource != NULL)
    {
        EPI_RESOURCE* r = static_cast<EPI_RESOURCE*>(p->hResource);
        if (r->Magic == EPI_RES_MAGIC)
        {
            r->Magic = 0;
            ExFreePoolWithTag(r, EPI_WDDM_TAG);
        }
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmDescribeAllocation(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_DESCRIBEALLOCATION p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiDescribeAllocation);
    EPI_ALLOCATION* a = EpiAlloc(p->hAllocation);
    if (a == NULL) { EpiFail(W, EpiDdiDescribeAllocation, STATUS_INVALID_PARAMETER); return STATUS_INVALID_PARAMETER; }
    p->Width = a->Desc.Width;
    p->Height = a->Desc.Height;
    p->Format = a->Desc.Format;
    p->MultisampleMethod.NumSamples = 1;
    p->MultisampleMethod.NumQualityLevels = 0;
    p->RefreshRate = a->Desc.RefreshRate;
    return STATUS_SUCCESS;
}

static NTSTATUS EpiStdSurface(_Out_ EPI_ALLOC_DESC* d, ULONG Flags, UINT Width, UINT Height, D3DDDIFORMAT Format)
{
    if (Width == 0 || Height == 0 || Width > 16384 || Height > 16384 || !EpiIs32bpp(Format))
    {
        return STATUS_INVALID_PARAMETER;
    }
    d->Magic = EPI_ALLOC_MAGIC;
    d->Flags = Flags;
    d->Width = Width;
    d->Height = Height;
    d->Pitch = Width * 4;
    d->Format = Format;
    d->SizeBytes = d->Pitch * Height;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmGetStandardAllocationDriverData(IN_CONST_HANDLE hAdapter,
                                                         INOUT_PDXGKARG_GETSTANDARDALLOCATIONDRIVERDATA p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiGetStandardAllocationDriverData);
    if (p->pAllocationPrivateDriverData == NULL || p->pResourcePrivateDriverData == NULL)
    {
        // First call: sizes only. ResourcePrivateDriverDataSize goes to CreateAllocation as PrivateDriverDataSize,
        // AllocationPrivateDriverDataSize as pAllocationInfo->PrivateDriverDataSize.
        p->ResourcePrivateDriverDataSize = sizeof(EPI_RES_DESC);
        p->AllocationPrivateDriverDataSize = sizeof(EPI_ALLOC_DESC);
        return STATUS_SUCCESS;
    }
    if (p->AllocationPrivateDriverDataSize < sizeof(EPI_ALLOC_DESC) || p->ResourcePrivateDriverDataSize < sizeof(EPI_RES_DESC))
    {
        EpiFail(W, EpiDdiGetStandardAllocationDriverData, STATUS_INVALID_PARAMETER);
        return STATUS_INVALID_PARAMETER;
    }
    EPI_ALLOC_DESC* d = static_cast<EPI_ALLOC_DESC*>(p->pAllocationPrivateDriverData);
    RtlZeroMemory(d, sizeof(*d));
    static_cast<EPI_RES_DESC*>(p->pResourcePrivateDriverData)->Magic = EPI_RES_MAGIC;

    NTSTATUS st;
    switch (p->StandardAllocationType)
    {
    case D3DKMDT_STANDARDALLOCATION_SHAREDPRIMARYSURFACE:
    {
        const D3DKMDT_SHAREDPRIMARYSURFACEDATA* s = p->pCreateSharedPrimarySurfaceData;
        st = EpiStdSurface(d, EPI_ALLOC_PRIMARY | EPI_ALLOC_SHARED, s->Width, s->Height, s->Format);
        if (NT_SUCCESS(st))
        {
            d->SourceId = s->VidPnSourceId;
            d->RefreshRate = s->RefreshRate;
        }
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_SHADOWSURFACE:
    {
        D3DKMDT_SHADOWSURFACEDATA* s = p->pCreateShadowSurfaceData;
        st = EpiStdSurface(d, EPI_ALLOC_SHADOW | EPI_ALLOC_SHARED, s->Width, s->Height, s->Format);
        if (NT_SUCCESS(st)) { s->Pitch = d->Pitch; }
        break;
    }
    case D3DKMDT_STANDARDALLOCATION_STAGINGSURFACE:
    {
        D3DKMDT_STAGINGSURFACEDATA* s = p->pCreateStagingSurfaceData;
        st = EpiStdSurface(d, EPI_ALLOC_STAGING, s->Width, s->Height, D3DDDIFMT_A8R8G8B8);
        if (NT_SUCCESS(st)) { s->Pitch = d->Pitch; }
        break;
    }
    default:
        // GDI surfaces are asked for only with GDI hardware acceleration (not offered); ROS does not support them.
        st = STATUS_NOT_SUPPORTED;
        break;
    }
    if (!NT_SUCCESS(st)) { EpiFail(W, EpiDdiGetStandardAllocationDriverData, st); }
    return st;
}

NTSTATUS APIENTRY EpiWddmOpenAllocation(IN_CONST_HANDLE hDevice, IN_CONST_PDXGKARG_OPENALLOCATION p)
{
    EPI_WDDM_DEVICE* d = EpiDev(hDevice);
    if (d == NULL) { return STATUS_INVALID_PARAMETER; }
    EPI_WDDM* W = d->W;
    EpiDdi(W, EpiDdiOpenAllocation);
    const DXGKRNL_INTERFACE* dx = W->Dxgk;
    UINT i = 0;
    for (; i < p->NumAllocations; ++i)
    {
        DXGK_OPENALLOCATIONINFO* oi = p->pOpenAllocation + i;
        DXGKARGCB_GETHANDLEDATA hd = {};
        hd.hObject = oi->hAllocation;
        hd.Type = DXGK_HANDLE_ALLOCATION;
        hd.Flags.DeviceSpecific = 0;
        EPI_ALLOCATION* a = EpiAlloc(dx->DxgkCbGetHandleData(&hd));
        EPI_DEVICE_ALLOCATION* da = (a != NULL)
            ? static_cast<EPI_DEVICE_ALLOCATION*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(*da), EPI_WDDM_TAG)) : NULL;
        if (da == NULL) { break; }
        da->Magic = EPI_DALLOC_OBJ;
        da->hKm = oi->hAllocation;
        da->Alloc = a;
        oi->hDeviceSpecificAllocation = da;
    }
    if (i == p->NumAllocations) { return STATUS_SUCCESS; }
    for (UINT j = 0; j < i; ++j)
    {
        EPI_DEVICE_ALLOCATION* da = static_cast<EPI_DEVICE_ALLOCATION*>(p->pOpenAllocation[j].hDeviceSpecificAllocation);
        da->Magic = 0;
        ExFreePoolWithTag(da, EPI_WDDM_TAG);
        p->pOpenAllocation[j].hDeviceSpecificAllocation = NULL;
    }
    EpiFail(W, EpiDdiOpenAllocation, STATUS_NO_MEMORY);
    return STATUS_NO_MEMORY;
}

NTSTATUS APIENTRY EpiWddmCloseAllocation(IN_CONST_HANDLE hDevice, IN_CONST_PDXGKARG_CLOSEALLOCATION p)
{
    EPI_WDDM_DEVICE* d = EpiDev(hDevice);
    if (d == NULL) { return STATUS_INVALID_PARAMETER; }
    EpiDdi(d->W, EpiDdiCloseAllocation);
    for (UINT i = 0; i < p->NumAllocations; ++i)
    {
        EPI_DEVICE_ALLOCATION* da = static_cast<EPI_DEVICE_ALLOCATION*>(p->pOpenHandleList[i]);
        if (da == NULL || da->Magic != EPI_DALLOC_OBJ) { continue; }
        da->Magic = 0;
        ExFreePoolWithTag(da, EPI_WDDM_TAG);
    }
    return STATUS_SUCCESS;
}

// ---- render, present, patch, submit (RosKmdContext.cpp, RosKmdAdapter.cpp) ----

static VOID EpiDmaInfo(_Out_writes_bytes_(Size) PVOID Priv, UINT Size, ULONG Kind, _In_ PVOID Buffer, UINT DmaSize)
{
    if (Priv == NULL || Size < sizeof(EPI_DMABUF_INFO)) { return; }
    EPI_DMABUF_INFO* info = static_cast<EPI_DMABUF_INFO*>(Priv);
    info->Magic = EPI_DMABUF_MAGIC;
    info->Kind = Kind;
    info->Buffer = static_cast<PUCHAR>(Buffer);
    info->Size = DmaSize;
}

NTSTATUS APIENTRY EpiWddmRender(IN_CONST_HANDLE hContext, INOUT_PDXGKARG_RENDER p)
{
    EPI_WDDM_CONTEXT* c = EpiCtx(hContext);
    if (c == NULL) { return STATUS_INVALID_PARAMETER; }
    EPI_WDDM* W = c->Dev->W;
    EpiDdi(W, EpiDdiRender);
    // W-1a has no UMD and no render command set: nothing is written into the DMA buffer, which then completes
    // on the CPU engine without work.
    if (p->CommandLength != 0) { EPI_COUNT(W, RenderNonEmpty); }
    EpiDmaInfo(p->pDmaBufferPrivateData, p->DmaBufferPrivateDataSize, EPI_DMA_RENDER, p->pDmaBuffer, p->DmaSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmRenderKm(IN_CONST_HANDLE hContext, INOUT_PDXGKARG_RENDER p)
{
    UNREFERENCED_PARAMETER(p);
    EPI_WDDM_CONTEXT* c = EpiCtx(hContext);
    if (c != NULL) { EpiDdi(c->Dev->W, EpiDdiRenderKm); }
    return STATUS_NOT_SUPPORTED;   // GDI contexts only (kernel-mode command buffers are not offered)
}

NTSTATUS APIENTRY EpiWddmPresent(IN_CONST_HANDLE hContext, INOUT_PDXGKARG_PRESENT p)
{
    EPI_WDDM_CONTEXT* c = EpiCtx(hContext);
    if (c == NULL) { return STATUS_INVALID_PARAMETER; }
    EPI_WDDM* W = c->Dev->W;
    EpiDdi(W, EpiDdiPresent);
    if (p->Flags.Flip) { EPI_COUNT(W, PresentFlip); }
    else if (p->Flags.Blt) { EPI_COUNT(W, PresentBlt); }
    else if (p->Flags.ColorFill) { EPI_COUNT(W, PresentColorFill); }
    else { EPI_COUNT(W, PresentOther); }
    // As in ROS, nothing is written for a present: flips are shown by SetVidPnSourceAddress; blt presents need
    // the UMD (W-5) and do not occur in W-1a.
    EpiDmaInfo(p->pDmaBufferPrivateData, p->DmaBufferPrivateDataSize, EPI_DMA_PRESENT, p->pDmaBuffer, p->DmaSize);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmPatch(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_PATCH p)
{
    UNREFERENCED_PARAMETER(p);
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiPatch);
    return STATUS_SUCCESS;   // no command refers to an allocation address yet
}

NTSTATUS APIENTRY EpiWddmSubmitCommand(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SUBMITCOMMAND p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiSubmitCommand);
    const EPI_DMABUF_INFO* info = static_cast<const EPI_DMABUF_INFO*>(p->pDmaBufferPrivateData);
    const BOOLEAN infoOk = (info != NULL && p->DmaBufferPrivateDataSize >= sizeof(*info) && info->Magic == EPI_DMABUF_MAGIC)
        ? TRUE : FALSE;

    KIRQL irql;
    KeAcquireSpinLock(&W->QueueLock, &irql);
    if (p->DmaBufferPhysicalAddress.QuadPart == 0 && p->DmaBufferSubmissionStartOffset == 0 &&
        p->DmaBufferSubmissionEndOffset == 0)
    {
        InterlockedIncrement(&EPI_T(W, PrepError));   // preparation error: the buffer is to be discarded (ROS)
    }
    if (IsListEmpty(&W->Free))
    {
        KeReleaseSpinLock(&W->QueueLock, irql);
        EPI_COUNT(W, QueueFull);   // the fence never comes back: dxgkrnl recovers with a timeout
        EpiFail(W, EpiDdiSubmitCommand, STATUS_INSUFFICIENT_RESOURCES);
        return STATUS_SUCCESS;
    }
    EPI_DMA_SUB* s = CONTAINING_RECORD(RemoveHeadList(&W->Free), EPI_DMA_SUB, Entry);
    s->Buffer = infoOk ? info->Buffer : NULL;
    s->Size = infoOk ? info->Size : 0;
    s->Kind = infoOk ? info->Kind : 0;
    s->Paging = p->Flags.Paging ? TRUE : FALSE;
    s->Start = p->DmaBufferSubmissionStartOffset;
    s->End = p->DmaBufferSubmissionEndOffset;
    s->Fence = p->SubmissionFenceId;
    InsertTailList(&W->Queue, &s->Entry);
    InterlockedExchange(&EPI_T(W, LastSubmitFence), (LONG)p->SubmissionFenceId);
    KeReleaseSpinLock(&W->QueueLock, irql);
    KeSetEvent(&W->WorkEvent, 0, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmBuildPagingBuffer(IN_CONST_HANDLE hAdapter, IN_PDXGKARG_BUILDPAGINGBUFFER p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiBuildPagingBuffer);
    PUCHAR start = static_cast<PUCHAR>(p->pDmaBuffer);
    PUCHAR pos = start;

    switch (p->Operation)
    {
    case DXGK_OPERATION_MAP_APERTURE_SEGMENT:
    {
        EPI_COUNT(W, PagingMap);
        if (p->MapApertureSegment.SegmentId != EPI_WDDM_SEG_APERTURE) { break; }
        const SIZE_T first = p->MapApertureSegment.OffsetInPages;
        const SIZE_T count = p->MapApertureSegment.NumberOfPages;
        PMDL mdl = p->MapApertureSegment.pMdl;
        const SIZE_T mdlPages = (mdl != NULL)
            ? ADDRESS_AND_SIZE_TO_SPAN_PAGES(MmGetMdlVirtualAddress(mdl), MmGetMdlByteCount(mdl)) : 0;
        if (first > EPI_WDDM_APERTURE_PAGES || count > EPI_WDDM_APERTURE_PAGES - first ||
            p->MapApertureSegment.MdlOffset > mdlPages || count > mdlPages - p->MapApertureSegment.MdlOffset)
        {
            EPI_COUNT(W, PagingError);
            break;
        }
        const PPFN_NUMBER pfn = MmGetMdlPfnArray(mdl);
        for (SIZE_T i = 0; i < count; ++i) { W->Aperture[first + i] = pfn[p->MapApertureSegment.MdlOffset + i]; }
        break;
    }
    case DXGK_OPERATION_UNMAP_APERTURE_SEGMENT:
    {
        EPI_COUNT(W, PagingUnmap);
        if (p->UnmapApertureSegment.SegmentId != EPI_WDDM_SEG_APERTURE) { break; }
        const SIZE_T first = p->UnmapApertureSegment.OffsetInPages;
        const SIZE_T count = p->UnmapApertureSegment.NumberOfPages;
        if (first > EPI_WDDM_APERTURE_PAGES || count > EPI_WDDM_APERTURE_PAGES - first)
        {
            EPI_COUNT(W, PagingError);
            break;
        }
        for (SIZE_T i = 0; i < count; ++i) { W->Aperture[first + i] = 0; }
        break;
    }
    case DXGK_OPERATION_FILL:
    case DXGK_OPERATION_TRANSFER:
        if (p->Operation == DXGK_OPERATION_FILL) { EPI_COUNT(W, PagingFill); } else { EPI_COUNT(W, PagingTransfer); }
        if (p->DmaSize < sizeof(DXGKARG_BUILDPAGINGBUFFER)) { return STATUS_GRAPHICS_INSUFFICIENT_DMA_BUFFER; }
        *reinterpret_cast<DXGKARG_BUILDPAGINGBUFFER*>(pos) = *p;   // executed later by the worker (ROS)
        pos += sizeof(DXGKARG_BUILDPAGINGBUFFER);
        break;
    case DXGK_OPERATION_DISCARD_CONTENT:
        EPI_COUNT(W, PagingDiscard);
        break;
    default:
        EPI_COUNT(W, PagingOther);
        break;
    }
    p->pDmaBuffer = pos;
    // Record the buffer only when it is new (nothing written yet), as ROS does.
    if (p->DmaSize == EPI_WDDM_PAGING_BUFFER_SIZE)
    {
        EpiDmaInfo(p->pDmaBufferPrivateData, p->DmaBufferPrivateDataSize, EPI_DMA_PAGING, start, p->DmaSize);
    }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmPreemptCommand(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_PREEMPTCOMMAND p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiPreemptCommand);
    // DMA buffer boundary: drop what has not started; the worker reports DMA_PREEMPTED after the running buffer.
    KIRQL irql;
    KeAcquireSpinLock(&W->QueueLock, &irql);
    while (!IsListEmpty(&W->Queue)) { InsertTailList(&W->Free, RemoveHeadList(&W->Queue)); }
    W->PreemptPending = TRUE;
    W->PreemptFence = p->PreemptionFenceId;
    KeReleaseSpinLock(&W->QueueLock, irql);
    KeSetEvent(&W->WorkEvent, 0, FALSE);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmQueryCurrentFence(IN_CONST_HANDLE hAdapter, INOUT_PDXGKARG_QUERYCURRENTFENCE p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiQueryCurrentFence);
    p->CurrentFence = (UINT)W->LastDoneFence;
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmResetFromTimeout(IN_CONST_HANDLE hAdapter)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiResetFromTimeout);
    KIRQL irql;
    KeAcquireSpinLock(&W->QueueLock, &irql);
    while (!IsListEmpty(&W->Queue)) { InsertTailList(&W->Free, RemoveHeadList(&W->Queue)); }
    W->PreemptPending = FALSE;
    W->ResetEpoch++;
    KeReleaseSpinLock(&W->QueueLock, irql);
    EPI_COUNT(W, Resets);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmRestartFromTimeout(IN_CONST_HANDLE hAdapter)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W != NULL) { EpiDdi(W, EpiDdiRestartFromTimeout); }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmEscape(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_ESCAPE p)
{
    UNREFERENCED_PARAMETER(p);
    EPI_WDDM* W = EpiW(hAdapter);
    if (W != NULL) { EpiDdi(W, EpiDdiEscape); }
    return STATUS_NOT_SUPPORTED;
}

NTSTATUS APIENTRY EpiWddmCollectDbgInfo(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COLLECTDBGINFO p)
{
    UNREFERENCED_PARAMETER(p);
    EPI_WDDM* W = EpiW(hAdapter);
    if (W != NULL) { EpiDdi(W, EpiDdiCollectDbgInfo); }
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmGetNodeMetadata(IN_CONST_HANDLE hAdapter, UINT NodeOrdinal, OUT_PDXGKARG_GETNODEMETADATA p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W != NULL) { EpiDdi(W, EpiDdiGetNodeMetadata); }
    RtlZeroMemory(p, sizeof(*p));
    p->EngineType = DXGK_ENGINE_TYPE_3D;
    RtlStringCbPrintfW(p->FriendlyName, sizeof(p->FriendlyName), L"3DNode%02X", NodeOrdinal);
    return STATUS_SUCCESS;
}

// ---- display DDIs that are new in full mode ----

NTSTATUS APIENTRY EpiWddmSetVidPnSourceAddress(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETVIDPNSOURCEADDRESS p)
{
    // Up to DIRQL: only interlocked writes and KeInsertQueueDpc.
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return STATUS_DEVICE_NOT_READY; }
    EpiDdi(W, EpiDdiSetVidPnSourceAddress);
    if (p->VidPnSourceId >= MAX_VIEWS) { return STATUS_INVALID_PARAMETER; }
    const EPI_ALLOCATION* a = EpiAlloc(p->hAllocation);
    if (a == NULL) { EPI_COUNT(W, FlipNoAlloc); }
    InterlockedIncrement(&W->FlipSeq);   // odd: writing
    W->Flip.Source = p->VidPnSourceId;
    W->Flip.Segment = p->PrimarySegment;
    W->Flip.Address = p->PrimaryAddress.QuadPart;
    W->Flip.Width = a ? a->Desc.Width : 0;
    W->Flip.Height = a ? a->Desc.Height : 0;
    W->Flip.Pitch = a ? a->Desc.Pitch : 0;
    InterlockedIncrement(&W->FlipSeq);   // even: done
    InterlockedExchange(&EPI_T(W, LastFlipSeg), (LONG)p->PrimarySegment);
    InterlockedExchange(&EPI_T(W, LastFlipAddr), (LONG)p->PrimaryAddress.LowPart);
    EPI_COUNT(W, Flips);
    KeInsertQueueDpc(&W->FlipDpc, NULL, NULL);
    return STATUS_SUCCESS;
}

NTSTATUS APIENTRY EpiWddmSetPalette(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_SETPALETTE p)
{
    UNREFERENCED_PARAMETER(p);
    EPI_WDDM* W = EpiW(hAdapter);
    if (W != NULL) { EpiDdi(W, EpiDdiSetPalette); }
    return STATUS_SUCCESS;   // 32 bpp only
}

NTSTATUS APIENTRY EpiWddmCommitVidPn(IN_CONST_HANDLE hAdapter, IN_CONST_PDXGKARG_COMMITVIDPN_CONST p)
{
    EPI_WDDM* W = EpiW(hAdapter);
    if (W == NULL) { return BddDdiCommitVidPn(hAdapter, p); }
    EpiDdi(W, EpiDdiCommitVidPn);
    // The commit maps and unmaps the framebuffer: keep the CPU scanout out while it runs.
    KeWaitForSingleObject(&W->ScanLock, Executive, KernelMode, FALSE, NULL);
    const NTSTATUS st = BddDdiCommitVidPn(hAdapter, p);
    KeSetEvent(&W->ScanLock, 0, FALSE);
    if (!NT_SUCCESS(st)) { EpiFail(W, EpiDdiCommitVidPn, st); }
    return st;
}

// ---- DriverEntry (RosKmdGlobal.cpp) ----

#pragma code_seg(push)
#pragma code_seg("PAGE")

NTSTATUS EpiWddmDriverEntry(_In_ DRIVER_OBJECT* pDriverObject, _In_ UNICODE_STRING* pRegistryPath)
{
    PAGED_CODE();
    DRIVER_INITIALIZATION_DATA d;
    RtlZeroMemory(&d, sizeof(d));
    d.Version = DXGKDDI_INTERFACE_VERSION;

    // Shared with the display-only mode (bdd_ddi.cxx).
    d.DxgkDdiAddDevice = BddDdiAddDevice;
    d.DxgkDdiStartDevice = BddDdiStartDevice;
    d.DxgkDdiStopDevice = BddDdiStopDevice;
    d.DxgkDdiRemoveDevice = BddDdiRemoveDevice;
    d.DxgkDdiDispatchIoRequest = BddDdiDispatchIoRequest;
    d.DxgkDdiInterruptRoutine = BddDdiInterruptRoutine;
    d.DxgkDdiDpcRoutine = BddDdiDpcRoutine;
    d.DxgkDdiQueryChildRelations = BddDdiQueryChildRelations;
    d.DxgkDdiQueryChildStatus = BddDdiQueryChildStatus;
    d.DxgkDdiQueryDeviceDescriptor = BddDdiQueryDeviceDescriptor;
    d.DxgkDdiSetPowerState = BddDdiSetPowerState;
    d.DxgkDdiResetDevice = BddDdiResetDevice;
    d.DxgkDdiUnload = BddDdiUnload;
    d.DxgkDdiQueryInterface = BddDdiQueryInterface;
    d.DxgkDdiControlInterrupt = BddDdiControlInterrupt;   // CRTC_VSYNC in full mode
    d.DxgkDdiSetPointerPosition = BddDdiSetPointerPosition;
    d.DxgkDdiSetPointerShape = BddDdiSetPointerShape;
    d.DxgkDdiIsSupportedVidPn = BddDdiIsSupportedVidPn;
    d.DxgkDdiRecommendFunctionalVidPn = BddDdiRecommendFunctionalVidPn;
    d.DxgkDdiEnumVidPnCofuncModality = BddDdiEnumVidPnCofuncModality;
    d.DxgkDdiSetVidPnSourceVisibility = BddDdiSetVidPnSourceVisibility;
    d.DxgkDdiUpdateActiveVidPnPresentPath = BddDdiUpdateActiveVidPnPresentPath;
    d.DxgkDdiRecommendMonitorModes = BddDdiRecommendMonitorModes;
    d.DxgkDdiGetScanLine = BddDdiGetScanLine;
    d.DxgkDdiQueryVidPnHWCapability = BddDdiQueryVidPnHWCapability;
    d.DxgkDdiStopDeviceAndReleasePostDisplayOwnership = BddDdiStopDeviceAndReleasePostDisplayOwnership;
    d.DxgkDdiSystemDisplayEnable = BddDdiSystemDisplayEnable;
    d.DxgkDdiSystemDisplayWrite = BddDdiSystemDisplayWrite;

    // Full driver (this file).
    d.DxgkDdiQueryAdapterInfo = EpiWddmQueryAdapterInfo;
    d.DxgkDdiCreateDevice = EpiWddmCreateDevice;
    d.DxgkDdiDestroyDevice = EpiWddmDestroyDevice;
    d.DxgkDdiCreateContext = EpiWddmCreateContext;
    d.DxgkDdiDestroyContext = EpiWddmDestroyContext;
    d.DxgkDdiCreateAllocation = EpiWddmCreateAllocation;
    d.DxgkDdiDestroyAllocation = EpiWddmDestroyAllocation;
    d.DxgkDdiDescribeAllocation = EpiWddmDescribeAllocation;
    d.DxgkDdiGetStandardAllocationDriverData = EpiWddmGetStandardAllocationDriverData;
    d.DxgkDdiOpenAllocation = EpiWddmOpenAllocation;
    d.DxgkDdiCloseAllocation = EpiWddmCloseAllocation;
    d.DxgkDdiRender = EpiWddmRender;
    d.DxgkDdiRenderKm = EpiWddmRenderKm;
    d.DxgkDdiPresent = EpiWddmPresent;
    d.DxgkDdiPatch = EpiWddmPatch;
    d.DxgkDdiSubmitCommand = EpiWddmSubmitCommand;
    d.DxgkDdiBuildPagingBuffer = EpiWddmBuildPagingBuffer;
    d.DxgkDdiPreemptCommand = EpiWddmPreemptCommand;
    d.DxgkDdiQueryCurrentFence = EpiWddmQueryCurrentFence;
    d.DxgkDdiResetFromTimeout = EpiWddmResetFromTimeout;
    d.DxgkDdiRestartFromTimeout = EpiWddmRestartFromTimeout;
    d.DxgkDdiEscape = EpiWddmEscape;
    d.DxgkDdiCollectDbgInfo = EpiWddmCollectDbgInfo;
    d.DxgkDdiGetNodeMetadata = EpiWddmGetNodeMetadata;
    d.DxgkDdiSetPalette = EpiWddmSetPalette;
    d.DxgkDdiSetVidPnSourceAddress = EpiWddmSetVidPnSourceAddress;
    d.DxgkDdiCommitVidPn = EpiWddmCommitVidPn;

    return DxgkInitialize(pDriverObject, pRegistryPath, &d);
}

#pragma code_seg(pop)
