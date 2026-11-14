// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - P-2 GPU present: the DXGK side and the mailbox to the GPU worker.
// New file, not derived from KMDOD. Touches no GPU, SMMU or GMU register.
#pragma once
// Include after BDD.hxx.

// One present handed to the GPU worker (G0_GPU_SURVEY section 9.21.68).
// The present thread allocates it (non-paged), the worker takes it from the mailbox. Both hold a reference;
// the last EpiPresentRelease frees it (and unlocks the source MDL if the present thread abandoned it).
typedef struct _EPI_GPU_PRESENT_REQ
{
    // ---- in: written by the present thread before submit, read only afterwards ----
    const UCHAR* Src;            // system VA such that pixel (x, y) is at Src + y * SrcPitch + x * 4 (BGRX).
                                 // Only the rows covered by Rects are mapped; never touch other rows.
    LONG   SrcPitch;             // bytes, > 0
    ULONG  Flags;                // EPI_PRESENT_REQ_SELFTEST: request built by the worker itself (no present thread)
    ULONG  NumRects;             // >= 1
    RECT*  Rects;                // screen coordinates (== source coordinates), clipped to 1080 x 2400, non-empty
    // ---- out: written by the worker before EpiPresentComplete ----
    NTSTATUS Status;             // STATUS_SUCCESS: every chunk's fence came back
    ULONG  Chunks;               // chunks submitted
    ULONG  MaxInFlight;          // max chunks in flight (<= 2)
    ULONG  FailReason;           // worker-defined, 0 on success
    // ---- private to epi_present.cxx ----
    KEVENT SourceReleased;       // worker will not read Src any more
    KEVENT Done;                 // worker finished (success or failure); Status is valid
    volatile LONG Refs;
    volatile LONG SourceSignalled;
    volatile LONG DoneSignalled;
    PMDL   Mdl;                  // set only when the present thread abandoned the request; freed with it
    RECT   RectStore[1];         // NumRects entries follow
} EPI_GPU_PRESENT_REQ;

#define EPI_PRESENT_REQ_SELFTEST 0x1

// ---- worker side (called by the GPU session worker, PASSIVE_LEVEL) ----
// Open at handover once P-2 is ready (after its self-test passed); Close before the session ends.
// Close completes a request still waiting in the mailbox with STATUS_CANCELLED. A request already taken must be
// completed by the worker itself: every EPI_GPU_PRESENT_REQ returned by EpiPresentTake must get exactly one
// EpiPresentComplete before the worker forgets it, on every path (stop, failure, teardown).
_IRQL_requires_(PASSIVE_LEVEL) VOID   EpiPresentMailboxOpen(VOID);
_IRQL_requires_(PASSIVE_LEVEL) VOID   EpiPresentMailboxClose(VOID);
PKEVENT EpiPresentRequestEvent(VOID);                        // synchronization event, wait on it with StopEvent
BOOLEAN EpiPresentStopRequested(VOID);                       // the present side gave up on a request (it has also
                                                             // called EpiGpuSessionRequestStop(4)); informational
EPI_GPU_PRESENT_REQ* EpiPresentTake(VOID);                   // NULL when nothing is waiting
VOID EpiPresentSourceReleased(_Inout_ EPI_GPU_PRESENT_REQ* Req); // after the last CPU read of Req->Src (idempotent)
VOID EpiPresentComplete(_Inout_ EPI_GPU_PRESENT_REQ* Req, NTSTATUS Status); // also signals SourceReleased; drops
                                                             // the worker's reference, so Req is gone afterwards
// Self-test helpers for the worker: build a request for a source the worker owns (kernel memory). The worker runs
// it through its own chunk path, then completes it like any other request.
_IRQL_requires_(PASSIVE_LEVEL)
EPI_GPU_PRESENT_REQ* EpiPresentAllocSelfTest(_In_ const UCHAR* Src, LONG SrcPitch, _In_ const RECT* Rect);

// ---- present thread side (blthw.cxx) ----
// Tries the GPU for a present whose source rows are already locked and mapped at Src (see Src above).
// Returns TRUE when the screen has been written completely by the GPU (and verified, if enabled).
// Returns FALSE (nothing submitted, *CpuReason = EPI_CPU_*) when the caller must do the whole present with the
// CPU and then call EpiCpuPresentDone. After a submit it always returns TRUE: on a GPU failure or a verify
// mismatch it has already rewritten every rect with the CPU. *pMdl may be set to NULL: the request then owns it.
_IRQL_requires_(PASSIVE_LEVEL)
BOOLEAN EpiGpuPresentTry(_In_ const CURRENT_BDD_MODE* Mode, _In_ BYTE* DstAddr, UINT DstBitPerPixel,
                         _In_ const UCHAR* Src, LONG SrcPitch, ULONG NumMoves, ULONG NumDirtyRects,
                         _In_reads_(NumDirtyRects) const RECT* DirtyRect, D3DKMDT_VIDPN_PRESENT_PATH_ROTATION Rotation,
                         _Inout_ PMDL* pMdl, _Out_ ULONG* CpuReason);
// The CPU path ran for a present (reason: EPI_CPU_*); records the time.
VOID EpiCpuPresentDone(ULONG Reason, ULONGLONG StartQpc);
ULONGLONG EpiPresentQpc(VOID);

#define EPI_CPU_NO_SESSION  0   // mailbox closed (no session, P-2 off, before the self-test)
#define EPI_CPU_DISABLED    1   // GpuPresentDisabled (verify mismatch or a GPU failure)
#define EPI_CPU_ROTATE      2
#define EPI_CPU_BPP         3   // source or target not 32 bpp
#define EPI_CPU_MOVE        4   // NumMoves > 0
#define EPI_CPU_TARGET      5   // not the plain 1080 x 2400 framebuffer at PA 0xE1000000, no source MDL, no rect
#define EPI_CPU_REASONS     6

// Device Parameters\EpiGpuPresentVerify (read in StartDevice): 1 (default) compares the screen with the source
// after every GPU present, 0 does not.
extern ULONG g_EpiGpuPresentVerify;

// Present-side counters for the worker's GpuPresent diagnostic. Written by the present thread only (one present
// at a time per adapter), read by the worker without a lock (values may be one present stale).
typedef struct _EPI_PRESENT_STATS
{
    ULONG Presents;              // EpiGpuPresentTry + CPU presents seen (every PresentDisplayOnly with a rect or move)
    ULONG GpuPresents;           // completed and (if enabled) verified on the GPU
    ULONG CpuPresents;
    ULONG CpuReason[EPI_CPU_REASONS];
    ULONG GpuRects;
    ULONG GpuChunks;
    ULONG GpuPixelsLow, GpuPixelsHigh;
    ULONG GpuFail;               // submitted but not completed with success (timeout, worker error, cancelled)
    ULONG GpuFailReason;         // last: 1 submit refused, 2 source timeout, 3 done timeout, 4 worker status, 5 no memory
    ULONG GpuFailStatus;         // last worker NTSTATUS
    ULONG VerifyBad;             // presents with a mismatch
    ULONG VerifyFirstX, VerifyFirstY, VerifyValue, VerifyExpected; // first mismatch ever
    ULONG GpuPresentDisabled;
    ULONG GpuPresentMaxUs, GpuPresentTotalUsLow, GpuPresentTotalUsHigh;
    ULONG CpuPresentMaxUs, CpuPresentTotalUsLow, CpuPresentTotalUsHigh;
    ULONG SourceHoldMaxUs;       // submit to SourceReleased
    ULONG Abandoned;             // requests left to the worker after a timeout
    ULONG MaxChunksInFlight;
} EPI_PRESENT_STATS;
extern EPI_PRESENT_STATS g_EpiPresentStats;
// Stops GPU presents for the rest of the session (the worker calls it when its self-test fails).
VOID EpiGpuPresentDisable(VOID);
