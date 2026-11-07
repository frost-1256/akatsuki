// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - DSI0 DCS brightness (WP2b-5). New file, not derived from KMDOD.
#pragma once
// Include after BDD.hxx and epi_hw.hxx.

// Brightness is DCS 0x51 (set_display_brightness, 12-bit big-endian; PCFG BacklightType 3 / BacklightBitWidth 12)
// sent on DSI0 with the command DMA, in the state UEFI left the controller (command mode, SW DMA trigger,
// low-power DMA). The MDP stream is allowed to go idle before each trigger; CMD_DMA_DONE is polled (no interrupt).
#define EPI_BL_MAX_LEVEL   4095
#define EPI_BL_MIN_LEVEL   16          // 0 % on the slider; level 0 would blank an AMOLED panel completely
#define EPI_BL_DEFAULT     2048        // PCFG BacklightDefault

typedef struct _EPI_DSI
{
    volatile ULONG* Regs;              // DSI0 (read-write), NULL when not started
    UCHAR*          Buf;               // one page, non-cached, below 4 GB (DMA_BASE is 32-bit)
    ULONG           BufPa;
    FAST_MUTEX      Lock;
    BOOLEAN         Ready;             // controller in the expected state and no DMA timeout so far
    BOOLEAN         Enabled;           // brightness interface offered this start (Device Parameters\EpiBrightness)
    UCHAR           Percent;           // last value set through the interface
    ULONG           Level;             // last level sent
    ULONG           Sends;
    ULONG           Fails;
    ULONG           LastIdleUs;
    ULONG           LastDoneUs;
    NTSTATUS        LastStatus;
} EPI_DSI;

// StartDevice: maps DSI0 and allocates the DMA page; checks the controller state (Ready). StopDevice: EpiDsiStop.
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiDsiStart(_Out_ EPI_DSI* pDsi, _In_ const EPI_HW_RESOURCES* pRes);
_IRQL_requires_(PASSIVE_LEVEL) VOID EpiDsiStop(_Inout_ EPI_DSI* pDsi);
// Sends one brightness command (serialized). After a DMA timeout Ready drops and nothing more is sent this start.
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiDsiSendBrightness(_Inout_ EPI_DSI* pDsi, ULONG Level);
// 0..100 % -> EPI_BL_MIN_LEVEL..EPI_BL_MAX_LEVEL
ULONG EpiDsiPercentToLevel(UCHAR Percent);

// DxgkDdiQueryInterface for GUID_DEVINTERFACE_BRIGHTNESS (v1) and GUID_DEVINTERFACE_BRIGHTNESS_2 (v2).
// Context is the BASIC_DISPLAY_DRIVER; callbacks go through BASIC_DISPLAY_DRIVER::EpiBrightnessSet/Get.
_IRQL_requires_(PASSIVE_LEVEL) NTSTATUS EpiBrightnessQueryInterface(_In_ PVOID Context, _Inout_ PQUERY_INTERFACE pQi);
