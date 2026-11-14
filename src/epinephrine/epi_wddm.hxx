// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - W-1: full WDDM 1.3 driver mode (G0_GPU_SURVEY section 9.21.72).
//
// Portions of epi_wddm.cxx are derived from the render-only sample (roskmd) in microsoft/graphics-driver-samples
// (commit de4a216): Copyright (c) Microsoft Corporation, MIT License (full text in LICENSE-ROS-MIT.txt).
// The VC4 hardware parts, WPP tracing and power components were removed; the rest was adapted to Epinephrine.
//
// Touches no GPU, SMMU or GMU register. The render engine is emulated on the CPU (paging buffers only in W-1a)
// and a flip copies the primary from the video memory segment to the UEFI framebuffer (CPU scanout).
#pragma once
// Include after BDD.hxx.

// Segments (same layout as ROS): 1 = aperture (DMA and paging buffers), 2 = video memory (physically contiguous
// system memory allocated at StartDevice, CPU visible, cache coherent).
#define EPI_WDDM_SEG_APERTURE        1
#define EPI_WDDM_SEG_VIDMEM          2
#define EPI_WDDM_APERTURE_BASE       0xC0000000ULL
#define EPI_WDDM_APERTURE_PAGES      1024
#define EPI_WDDM_VIDMEM_MAX          (128u * 1024 * 1024)
#define EPI_WDDM_VIDMEM_MIN          (32u * 1024 * 1024)
#define EPI_WDDM_DMA_BUFFER_SIZE     PAGE_SIZE
#define EPI_WDDM_PAGING_BUFFER_SIZE  PAGE_SIZE
#define EPI_WDDM_ALLOCATION_LIST     64
#define EPI_WDDM_PATCH_LIST          128
#define EPI_WDDM_QUEUE_LENGTH        64

// Allocation private data (DXGK_ALLOCATIONINFO.pPrivateDriverData). Written by GetStandardAllocationDriverData;
// W-1a has no UMD, so every other caller of CreateAllocation is refused.
#define EPI_ALLOC_MAGIC     0x43415045u   // 'EPAC'
#define EPI_ALLOC_PRIMARY   0x1           // shared primary (scanned out by a flip)
#define EPI_ALLOC_SHADOW    0x2
#define EPI_ALLOC_STAGING   0x4
#define EPI_ALLOC_SHARED    0x8
typedef struct _EPI_ALLOC_DESC
{
    ULONG          Magic;
    ULONG          Flags;              // EPI_ALLOC_*
    ULONG          Width, Height;      // pixels
    ULONG          Pitch;              // bytes
    D3DDDIFORMAT   Format;
    D3DDDI_VIDEO_PRESENT_SOURCE_ID SourceId;
    D3DDDI_RATIONAL RefreshRate;
    ULONG          SizeBytes;
} EPI_ALLOC_DESC;

#define EPI_RES_MAGIC 0x53455045u        // 'EPES'
typedef struct _EPI_RES_DESC
{
    ULONG Magic;
} EPI_RES_DESC;

// DXGKQAITYPE_UMDRIVERPRIVATE (for the future UMD).
typedef struct _EPI_UMD_ADAPTER_INFO
{
    ULONG Magic;                         // 'EPUM'
    ULONG Version;                       // 1
    ULONG WddmVersion;
    ULONG VidMemBytes;
} EPI_UMD_ADAPTER_INFO;

// DDI call counters in the WddmTrace diagnostic (index into EPI_WDDM_TRACE.Ddi).
enum EPI_WDDM_DDI
{
    EpiDdiQueryAdapterInfo, EpiDdiCreateDevice, EpiDdiDestroyDevice, EpiDdiCreateContext, EpiDdiDestroyContext,
    EpiDdiCreateAllocation, EpiDdiDestroyAllocation, EpiDdiDescribeAllocation, EpiDdiGetStandardAllocationDriverData,
    EpiDdiOpenAllocation, EpiDdiCloseAllocation, EpiDdiRender, EpiDdiPresent, EpiDdiPatch, EpiDdiSubmitCommand,
    EpiDdiBuildPagingBuffer, EpiDdiPreemptCommand, EpiDdiQueryCurrentFence, EpiDdiResetFromTimeout,
    EpiDdiRestartFromTimeout, EpiDdiEscape, EpiDdiCollectDbgInfo, EpiDdiSetVidPnSourceAddress, EpiDdiSetPalette,
    EpiDdiGetNodeMetadata, EpiDdiRenderKm, EpiDdiCommitVidPn,
    EpiDdiCount
};

// EpiDiag WddmTrace (REG_BINARY): all ULONG, written by the worker every second when something changed and at
// StopDevice. The decoder (tools/decw1.py) must follow this order.
typedef struct _EPI_WDDM_TRACE
{
    ULONG Version;            // 1
    ULONG Mode;               // EpiWddmMode used by DriverEntry (1 or 2)
    ULONG StartStatus;        // EpiWddmStart NTSTATUS
    ULONG VidMemBytes;
    ULONG VidMemPaLow, VidMemPaHigh;
    ULONG Writes;
    ULONG Ddi[EpiDdiCount];
    ULONG FailCount, LastFailDdi, LastFailStatus;
    ULONG QaiMask;            // bit n: DXGKQAITYPE n (< 32) was asked
    ULONG QaiUnknownCount, QaiUnknownLast;
    ULONG PresentBlt, PresentFlip, PresentColorFill, PresentOther, PresentDone;
    ULONG RenderDone, RenderNonEmpty;
    ULONG PagingBuffers, PagingFill, PagingTransfer, PagingXferOffset, PagingDiscard, PagingMap, PagingUnmap,
          PagingOther, PagingError;
    ULONG AllocLive, AllocMax, AllocBytesLive, AllocPrimary, AllocShadow, AllocStaging, AllocBadData;
    ULONG LastSubmitFence, LastDoneFence, DmaCompleted, Preempted, QueueFull, NotifyFail, PrepError, Resets;
    ULONG Flips, FlipNoAlloc, ScanCopies, ScanSkipped, ScanMaxUs, LastFlipSeg, LastFlipAddr;
    ULONG VsyncNotifies;
} EPI_WDDM_TRACE;

typedef struct _EPI_WDDM EPI_WDDM;

// DriverEntry for EpiWddmMode 1/2: registers the full driver (DxgkInitialize).
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS EpiWddmDriverEntry(_In_ DRIVER_OBJECT* pDriverObject, _In_ UNICODE_STRING* pRegistryPath);

// StartDevice (full mode only, after the VSync start): video memory segment, DMA queue, worker thread.
_IRQL_requires_(PASSIVE_LEVEL)
NTSTATUS EpiWddmStart(_In_ BASIC_DISPLAY_DRIVER* Bdd, _Outptr_ EPI_WDDM** ppW);
// StopDevice: stops the worker, writes the last WddmTrace, frees everything. Idempotent.
_IRQL_requires_(PASSIVE_LEVEL)
VOID EpiWddmStop(_Inout_ EPI_WDDM** ppW);
