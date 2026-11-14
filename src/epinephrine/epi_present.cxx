// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - P-2 GPU present: the DXGK side and the mailbox to the GPU worker.
// New file, not derived from KMDOD. Touches no GPU, SMMU or GMU register: the worker (epi_gmu.cxx) does the
// chunking, staging, IB and ring work (G0_GPU_SURVEY section 9.21.68). This file decides which presents go to the
// GPU, hands them over, waits, verifies, and falls back to the CPU.
//
// Lifetime of a request (non-paged, two references):
//   present thread: alloc (ref 1) -> submit (ref 2, mailbox) -> wait SourceReleased -> wait Done -> release
//   worker:         take -> read Src into staging -> EpiPresentSourceReleased -> fences -> EpiPresentComplete
// If the present thread gives up (timeout), it copies the present with the CPU, hands the source MDL to the
// request (the pages stay locked until the worker lets go of it) and releases its reference. The worker's
// EpiPresentComplete then frees the request and unlocks the MDL. The present thread never returns while the
// worker can still read a source that is unlocked.

#include "BDD.hxx"
#include "epi_hw.hxx"
#include "epi_present.hxx"

#define EPI_PRESENT_TAG      'rpEE'
#define EPI_FB_PA            0xE1000000ULL
#define EPI_FB_WIDTH         1080
#define EPI_FB_HEIGHT        2400
#define EPI_FB_PITCH         4320
#define EPI_SOURCE_WAIT_MS   1000   // submit to SourceReleased (the worker's own waits are 100 ms bounded)
#define EPI_DONE_WAIT_MS     200    // SourceReleased to Done

ULONG g_EpiGpuPresentVerify = 1;
EPI_PRESENT_STATS g_EpiPresentStats;

static KSPIN_LOCK g_PmLock;                 // zero == initialized
static KEVENT g_PmRequest;
static BOOLEAN g_PmOpen = FALSE;
static EPI_GPU_PRESENT_REQ* g_PmPending = NULL;
static volatile LONG g_PmDisabled = 0;
static volatile LONG g_PmStopRequested = 0;

ULONGLONG EpiPresentQpc(VOID)
{
    return static_cast<ULONGLONG>(KeQueryPerformanceCounter(NULL).QuadPart);
}

static ULONG EpiQpcToUs(ULONGLONG Start)
{
    LARGE_INTEGER freq;
    const ULONGLONG now = static_cast<ULONGLONG>(KeQueryPerformanceCounter(&freq).QuadPart);
    if (freq.QuadPart <= 0 || now < Start) { return 0; }
    const ULONGLONG us = (now - Start) * 1000000ULL / static_cast<ULONGLONG>(freq.QuadPart);
    return us > MAXULONG ? MAXULONG : static_cast<ULONG>(us);
}

static VOID EpiAdd64(ULONG* Low, ULONG* High, ULONGLONG Value)
{
    const ULONGLONG v = ((static_cast<ULONGLONG>(*High) << 32) | *Low) + Value;
    *Low = static_cast<ULONG>(v);
    *High = static_cast<ULONG>(v >> 32);
}

static EPI_GPU_PRESENT_REQ* EpiPresentAlloc(ULONG NumRects)
{
    const SIZE_T size = FIELD_OFFSET(EPI_GPU_PRESENT_REQ, RectStore) + static_cast<SIZE_T>(NumRects) * sizeof(RECT);
    EPI_GPU_PRESENT_REQ* r = static_cast<EPI_GPU_PRESENT_REQ*>(ExAllocatePool2(POOL_FLAG_NON_PAGED, size, EPI_PRESENT_TAG));
    if (r == NULL) { return NULL; }
    KeInitializeEvent(&r->SourceReleased, NotificationEvent, FALSE);
    KeInitializeEvent(&r->Done, NotificationEvent, FALSE);
    r->Refs = 1;
    r->Rects = r->RectStore;
    r->Status = STATUS_PENDING;
    return r;
}

static VOID EpiPresentRelease(EPI_GPU_PRESENT_REQ* Req)
{
    if (InterlockedDecrement(&Req->Refs) != 0) { return; }
    if (Req->Mdl != NULL)
    {
        MmUnlockPages(Req->Mdl);
        IoFreeMdl(Req->Mdl);
    }
    ExFreePoolWithTag(Req, EPI_PRESENT_TAG);
}

// ---- worker side ----

VOID EpiPresentMailboxOpen(VOID)
{
    KeInitializeEvent(&g_PmRequest, SynchronizationEvent, FALSE);
    RtlZeroMemory(&g_EpiPresentStats, sizeof(g_EpiPresentStats));
    InterlockedExchange(&g_PmDisabled, 0);
    InterlockedExchange(&g_PmStopRequested, 0);
    KIRQL irql;
    KeAcquireSpinLock(&g_PmLock, &irql);
    g_PmPending = NULL;
    g_PmOpen = TRUE;
    KeReleaseSpinLock(&g_PmLock, irql);
}

VOID EpiPresentMailboxClose(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_PmLock, &irql);
    g_PmOpen = FALSE;
    EPI_GPU_PRESENT_REQ* r = g_PmPending;
    g_PmPending = NULL;
    KeReleaseSpinLock(&g_PmLock, irql);
    if (r != NULL) { EpiPresentComplete(r, STATUS_CANCELLED); }
}

PKEVENT EpiPresentRequestEvent(VOID)
{
    return &g_PmRequest;
}

EPI_GPU_PRESENT_REQ* EpiPresentTake(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_PmLock, &irql);
    EPI_GPU_PRESENT_REQ* r = g_PmPending;
    g_PmPending = NULL;
    KeReleaseSpinLock(&g_PmLock, irql);
    return r;
}

VOID EpiPresentSourceReleased(EPI_GPU_PRESENT_REQ* Req)
{
    if (InterlockedExchange(&Req->SourceSignalled, 1) == 0)
    {
        KeSetEvent(&Req->SourceReleased, IO_NO_INCREMENT, FALSE);
    }
}

VOID EpiPresentComplete(EPI_GPU_PRESENT_REQ* Req, NTSTATUS Status)
{
    if (InterlockedExchange(&Req->DoneSignalled, 1) != 0) { return; } // a second completion would drop a ref twice
    Req->Status = Status;
    KeMemoryBarrier();
    EpiPresentSourceReleased(Req);
    KeSetEvent(&Req->Done, IO_NO_INCREMENT, FALSE);
    EpiPresentRelease(Req);
}

EPI_GPU_PRESENT_REQ* EpiPresentAllocSelfTest(const UCHAR* Src, LONG SrcPitch, const RECT* Rect)
{
    EPI_GPU_PRESENT_REQ* r = EpiPresentAlloc(1);
    if (r == NULL) { return NULL; }
    r->Src = Src;
    r->SrcPitch = SrcPitch;
    r->Flags = EPI_PRESENT_REQ_SELFTEST;
    r->NumRects = 1;
    r->RectStore[0] = *Rect;
    return r;
}

BOOLEAN EpiPresentStopRequested(VOID)
{
    return g_PmStopRequested != 0;
}

VOID EpiGpuPresentDisable(VOID)
{
    InterlockedExchange(&g_PmDisabled, 1);
    g_EpiPresentStats.GpuPresentDisabled = 1;
}

// ---- present thread side ----

static BOOLEAN EpiPresentSubmit(EPI_GPU_PRESENT_REQ* Req)
{
    BOOLEAN ok = FALSE;
    KIRQL irql;
    KeAcquireSpinLock(&g_PmLock, &irql);
    if (g_PmOpen && g_PmDisabled == 0 && g_PmPending == NULL)
    {
        InterlockedIncrement(&Req->Refs);
        g_PmPending = Req;
        ok = TRUE;
    }
    KeReleaseSpinLock(&g_PmLock, irql);
    if (ok) { KeSetEvent(&g_PmRequest, IO_NO_INCREMENT, FALSE); }
    return ok;
}

static BOOLEAN EpiPresentReady(VOID)
{
    KIRQL irql;
    KeAcquireSpinLock(&g_PmLock, &irql);
    const BOOLEAN open = g_PmOpen;
    KeReleaseSpinLock(&g_PmLock, irql);
    return open;
}

static BOOLEAN EpiWait(PKEVENT Event, ULONG Milliseconds)
{
    LARGE_INTEGER t;
    t.QuadPart = -static_cast<LONGLONG>(Milliseconds) * 10000;
    return KeWaitForSingleObject(Event, Executive, KernelMode, FALSE, &t) == STATUS_SUCCESS;
}

// Plain framebuffer, identity: the CPU copy used when the GPU path gave up after a submit.
static VOID EpiCpuCopyRects(BYTE* Dst, const UCHAR* Src, LONG SrcPitch, ULONG NumRects, const RECT* Rects)
{
    for (ULONG i = 0; i < NumRects; ++i)
    {
        const RECT& r = Rects[i];
        const SIZE_T bytes = static_cast<SIZE_T>(r.right - r.left) * 4;
        for (LONG y = r.top; y < r.bottom; ++y)
        {
            RtlCopyMemory(Dst + static_cast<SIZE_T>(y) * EPI_FB_PITCH + static_cast<SIZE_T>(r.left) * 4,
                          Src + static_cast<SSIZE_T>(y) * SrcPitch + static_cast<SSIZE_T>(r.left) * 4, bytes);
        }
    }
}

// TRUE when the screen equals the source in every rect (32 bit compare, X byte included).
static BOOLEAN EpiVerifyRects(const BYTE* Dst, const UCHAR* Src, LONG SrcPitch, ULONG NumRects, const RECT* Rects)
{
    for (ULONG i = 0; i < NumRects; ++i)
    {
        const RECT& r = Rects[i];
        for (LONG y = r.top; y < r.bottom; ++y)
        {
            const volatile ULONG* d = reinterpret_cast<const volatile ULONG*>(Dst + static_cast<SIZE_T>(y) * EPI_FB_PITCH);
            const ULONG* s = reinterpret_cast<const ULONG*>(Src + static_cast<SSIZE_T>(y) * SrcPitch);
            for (LONG x = r.left; x < r.right; ++x)
            {
                const ULONG got = d[x];
                if (got != s[x])
                {
                    if (g_EpiPresentStats.VerifyBad == 0)
                    {
                        g_EpiPresentStats.VerifyFirstX = static_cast<ULONG>(x);
                        g_EpiPresentStats.VerifyFirstY = static_cast<ULONG>(y);
                        g_EpiPresentStats.VerifyValue = got;
                        g_EpiPresentStats.VerifyExpected = s[x];
                    }
                    return FALSE;
                }
            }
        }
    }
    return TRUE;
}

static VOID EpiGpuPresentFailed(ULONG Reason, NTSTATUS Status)
{
    ++g_EpiPresentStats.GpuFail;
    g_EpiPresentStats.GpuFailReason = Reason;
    g_EpiPresentStats.GpuFailStatus = static_cast<ULONG>(Status);
    EpiGpuPresentDisable();
    InterlockedExchange(&g_PmStopRequested, 1);
    EpiGpuSessionRequestStop(4);    // the worker tears the GPU down (never waits); later presents take the CPU path
}

VOID EpiCpuPresentDone(ULONG Reason, ULONGLONG StartQpc)
{
    const ULONG us = EpiQpcToUs(StartQpc);
    ++g_EpiPresentStats.CpuPresents;
    if (Reason < EPI_CPU_REASONS) { ++g_EpiPresentStats.CpuReason[Reason]; }
    if (us > g_EpiPresentStats.CpuPresentMaxUs) { g_EpiPresentStats.CpuPresentMaxUs = us; }
    EpiAdd64(&g_EpiPresentStats.CpuPresentTotalUsLow, &g_EpiPresentStats.CpuPresentTotalUsHigh, us);
}

BOOLEAN EpiGpuPresentTry(const CURRENT_BDD_MODE* Mode, BYTE* DstAddr, UINT DstBitPerPixel, const UCHAR* Src, LONG SrcPitch,
                         ULONG NumMoves, ULONG NumDirtyRects, const RECT* DirtyRect,
                         D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation, PMDL* pMdl, ULONG* CpuReason)
{
    PAGED_CODE();
    const ULONGLONG start = EpiPresentQpc();
    ++g_EpiPresentStats.Presents;

    if (!EpiPresentReady()) { *CpuReason = EPI_CPU_NO_SESSION; return FALSE; }
    if (g_PmDisabled != 0) { *CpuReason = EPI_CPU_DISABLED; return FALSE; }
    if (Rotation != D3DKMDT_VPPR_IDENTITY) { *CpuReason = EPI_CPU_ROTATE; return FALSE; }
    if (DstBitPerPixel != 32) { *CpuReason = EPI_CPU_BPP; return FALSE; }
    if (NumMoves != 0) { *CpuReason = EPI_CPU_MOVE; return FALSE; }
    // The worker is a system thread: it can read only a system mapping of the source (EpiFastPresent 1, MDL).
    if (*pMdl == NULL) { *CpuReason = EPI_CPU_TARGET; return FALSE; }
    if (Mode->DispInfo.PhysicAddress.QuadPart != static_cast<LONGLONG>(EPI_FB_PA) ||
        Mode->DispInfo.Width != EPI_FB_WIDTH || Mode->DispInfo.Height != EPI_FB_HEIGHT ||
        Mode->DispInfo.Pitch != EPI_FB_PITCH || Mode->SrcModeWidth != EPI_FB_WIDTH ||
        Mode->SrcModeHeight != EPI_FB_HEIGHT || DstAddr != Mode->FrameBuffer.Ptr || SrcPitch < EPI_FB_WIDTH * 4)
    { *CpuReason = EPI_CPU_TARGET; return FALSE; }

    ULONG n = 0;
    for (ULONG i = 0; i < NumDirtyRects; ++i)
    {
        const RECT& r = DirtyRect[i];
        if (r.left < 0 || r.top < 0 || r.right > EPI_FB_WIDTH || r.bottom > EPI_FB_HEIGHT) { *CpuReason = EPI_CPU_TARGET; return FALSE; }
        if (r.left < r.right && r.top < r.bottom) { ++n; }
    }
    if (n == 0) { *CpuReason = EPI_CPU_TARGET; return FALSE; } // nothing to draw; the CPU path is a no-op

    EPI_GPU_PRESENT_REQ* req = EpiPresentAlloc(n);
    if (req == NULL) { ++g_EpiPresentStats.GpuFail; g_EpiPresentStats.GpuFailReason = 5; *CpuReason = EPI_CPU_NO_SESSION; return FALSE; }
    req->Src = Src;
    req->SrcPitch = SrcPitch;
    req->NumRects = n;
    ULONGLONG pixels = 0;
    for (ULONG i = 0, k = 0; i < NumDirtyRects; ++i)
    {
        const RECT& r = DirtyRect[i];
        if (r.left < r.right && r.top < r.bottom)
        {
            req->RectStore[k++] = r;
            pixels += static_cast<ULONGLONG>(r.right - r.left) * static_cast<ULONGLONG>(r.bottom - r.top);
        }
    }

    // Earlier CPU presents went through a write-combined mapping: let them reach memory before the GPU draws
    // over the same pixels.
    KeMemoryBarrier();
#if defined(_M_ARM64)
    __dsb(_ARM64_BARRIER_SY);
#endif
    if (!EpiPresentSubmit(req))
    {
        EpiPresentRelease(req);
        ++g_EpiPresentStats.GpuFail;
        g_EpiPresentStats.GpuFailReason = 1;
        *CpuReason = EPI_CPU_NO_SESSION;
        return FALSE;
    }

    ULONG fail = 0;
    if (!EpiWait(&req->SourceReleased, EPI_SOURCE_WAIT_MS)) { fail = 2; }
    else
    {
        const ULONG hold = EpiQpcToUs(start);
        if (hold > g_EpiPresentStats.SourceHoldMaxUs) { g_EpiPresentStats.SourceHoldMaxUs = hold; }
        if (!EpiWait(&req->Done, EPI_DONE_WAIT_MS)) { fail = 3; }
        else if (!NT_SUCCESS(req->Status)) { fail = 4; }
    }

    if (fail == 0)
    {
        if (req->MaxInFlight > g_EpiPresentStats.MaxChunksInFlight) { g_EpiPresentStats.MaxChunksInFlight = req->MaxInFlight; }
        g_EpiPresentStats.GpuChunks += req->Chunks;
        if (g_EpiGpuPresentVerify != 0 && !EpiVerifyRects(DstAddr, Src, SrcPitch, n, req->RectStore))
        {
            ++g_EpiPresentStats.VerifyBad;
            EpiGpuPresentDisable();           // the session and its heartbeats go on; presents use the CPU from now
            EpiCpuCopyRects(DstAddr, Src, SrcPitch, n, req->RectStore);
        }
        else
        {
            ++g_EpiPresentStats.GpuPresents;
            g_EpiPresentStats.GpuRects += n;
            EpiAdd64(&g_EpiPresentStats.GpuPixelsLow, &g_EpiPresentStats.GpuPixelsHigh, pixels);
            const ULONG us = EpiQpcToUs(start);
            if (us > g_EpiPresentStats.GpuPresentMaxUs) { g_EpiPresentStats.GpuPresentMaxUs = us; }
            EpiAdd64(&g_EpiPresentStats.GpuPresentTotalUsLow, &g_EpiPresentStats.GpuPresentTotalUsHigh, us);
        }
        EpiPresentRelease(req);
        return TRUE;
    }

    // Gave up on the GPU: copy everything with the CPU (the source is still locked and mapped).
    EpiGpuPresentFailed(fail, fail == 4 ? req->Status : STATUS_TIMEOUT);
    EpiCpuCopyRects(DstAddr, Src, SrcPitch, n, req->RectStore);
    if (fail == 2 && req->SourceSignalled == 0)
    {
        // The worker may still read the source: the request keeps the pages locked until it is completed.
        ++g_EpiPresentStats.Abandoned;
        req->Mdl = *pMdl;
        *pMdl = NULL;
        KeMemoryBarrier();
    }
    EpiPresentRelease(req);
    return TRUE;
}
