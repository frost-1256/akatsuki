// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - panel XML (PCFG) loader (WP2b-2). New file, not derived from KMDOD.

#include "BDD.hxx"
#include "epi_hw.hxx"

#pragma code_seg("PAGE")

#define EPI_ROM_CHUNK   0x1000      // GPU0._ROM returns at most 0x1000 bytes per call
#define EPI_XML_MAX     0x10000     // PCFG is 0x167C bytes today; cap the loop anyway
#define EPI_POOL_TAG    'ipEE'

// _ROM(Offset, Length, Arg2) -> Buffer. Arg2 selects a panel variant in some firmwares; spacewar ignores it.
static NTSTATUS EpiReadRomChunk(const DXGKRNL_INTERFACE* pDxgk, ULONG Offset, ACPI_EVAL_OUTPUT_BUFFER* pOut, ULONG OutSize)
{
    PAGED_CODE();
    // Header + three integer arguments (each ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG)) bytes).
    const ULONG argLen = ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG));
    const ULONG inSize = FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + 3 * argLen;
    DECLSPEC_ALIGN(8) UCHAR inBuf[FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX, Argument) + 3 * ACPI_METHOD_ARGUMENT_LENGTH(sizeof(ULONG))];
    RtlZeroMemory(inBuf, sizeof(inBuf));
    ACPI_EVAL_INPUT_BUFFER_COMPLEX* pIn = reinterpret_cast<ACPI_EVAL_INPUT_BUFFER_COMPLEX*>(inBuf);
    pIn->Signature = ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE;
    pIn->MethodNameAsUlong = (ULONG)'MOR_';   // "_ROM"
    pIn->Size = 3 * argLen;
    pIn->ArgumentCount = 3;
    PACPI_METHOD_ARGUMENT a = pIn->Argument;
    ACPI_METHOD_SET_ARGUMENT_INTEGER(a, Offset);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(a, EPI_ROM_CHUNK);
    a = ACPI_METHOD_NEXT_ARGUMENT(a);
    ACPI_METHOD_SET_ARGUMENT_INTEGER(a, 0);

    RtlZeroMemory(pOut, OutSize);
    return pDxgk->DxgkCbEvalAcpiMethod(pDxgk->DeviceHandle, DISPLAY_ADAPTER_HW_ID, pIn, inSize, pOut, OutSize);
}

VOID EpiPanelLoad(_In_ const DXGKRNL_INTERFACE* pDxgk, _In_ PDEVICE_OBJECT Pdo, _Out_ EPI_PANEL* pPanel)
{
    PAGED_CODE();
    RtlZeroMemory(pPanel, sizeof(*pPanel));

    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(Pdo, &hParams, &hDiag))) { return; }

    ULONG status = 0;
    ULONG xmlLen = 0;
    char* xml = NULL;
    ACPI_EVAL_OUTPUT_BUFFER* out = NULL;
    const ULONG outSize = FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + ACPI_METHOD_ARGUMENT_LENGTH(EPI_ROM_CHUNK) + 0x10;

    if (EpiQueryDword(hParams, L"EpiDisablePanel", 0) != 0)
    {
        status = 1;
        goto done;
    }

    xml = static_cast<char*>(ExAllocatePool2(POOL_FLAG_PAGED, EPI_XML_MAX, EPI_POOL_TAG));
    out = static_cast<ACPI_EVAL_OUTPUT_BUFFER*>(ExAllocatePool2(POOL_FLAG_PAGED, outSize, EPI_POOL_TAG));
    if (xml == NULL || out == NULL)
    {
        status = 2;
        goto done;
    }

    while (xmlLen < EPI_XML_MAX)
    {
        NTSTATUS st = EpiReadRomChunk(pDxgk, xmlLen, out, outSize);
        if (!NT_SUCCESS(st))
        {
            EpiSetDword(hDiag, L"PanelAcpiStatus", (ULONG)st);
            status = 3;
            goto done;
        }
        if (out->Signature != ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE || out->Count < 1 ||
            out->Argument[0].Type != ACPI_METHOD_ARGUMENT_BUFFER ||
            out->Argument[0].DataLength > EPI_ROM_CHUNK ||
            FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER, Argument) + ACPI_METHOD_ARGUMENT_LENGTH(out->Argument[0].DataLength) > outSize)
        {
            status = 4;
            goto done;
        }
        ULONG got = out->Argument[0].DataLength;
        // Past the end, _ROM returns Buffer(One){0}. A real 1-byte tail would only be the trailing NUL.
        if (got <= 1) { break; }
        if (got > EPI_XML_MAX - xmlLen) { got = EPI_XML_MAX - xmlLen; }
        RtlCopyMemory(xml + xmlLen, out->Argument[0].Data, got);
        xmlLen += got;
        if (got < EPI_ROM_CHUNK) { break; }
    }
    EpiSetDword(hDiag, L"PanelXmlLen", xmlLen);
    if (xmlLen == 0)
    {
        status = 4;
        goto done;
    }

    {
        int missing = -1;
        int rc = EpiEdidFromPanelXml(xml, xmlLen, pPanel->Edid, &pPanel->Timing, &missing);
        if (rc != EPI_EDID_OK)
        {
            EpiSetDword(hDiag, L"PanelMissingTag", (ULONG)missing);
            status = 10 + (ULONG)rc;
            goto done;
        }
    }
    if (!IsEdidHeaderValid(pPanel->Edid) || !IsEdidChecksumValid(pPanel->Edid))
    {
        status = 20;
        goto done;
    }
    pPanel->Valid = TRUE;
    {
        ULONG features = EpiQueryDword(hParams, L"EpiPanelFeatures", 1);
        pPanel->UseEdid = (features & 1) ? TRUE : FALSE;
        pPanel->UseSignal = (features & 2) ? TRUE : FALSE;
        EpiSetDword(hDiag, L"PanelFeatures", features);
    }
    EpiSetValue(hDiag, L"Edid", REG_BINARY, pPanel->Edid, EPI_EDID_SIZE);
    {
        const EPI_PANEL_TIMING& t = pPanel->Timing;
        WCHAR text[160];
        if (NT_SUCCESS(RtlStringCchPrintfW(text, RTL_NUMBER_OF(text),
                L"%ux%u h(fp%u bp%u sw%u) v(fp%u bp%u sw%u) refQ16=0x%X pclk10k=%u invH%u invV%u",
                t.HActive, t.VActive, t.HFrontPorch, t.HBackPorch, t.HSyncPulse,
                t.VFrontPorch, t.VBackPorch, t.VSyncPulse, t.RefreshQ16, t.PixelClock10kHz, t.InvertHsync, t.InvertVsync)))
        {
            EpiSetValue(hDiag, L"PanelTiming", REG_SZ, text, (ULONG)((wcslen(text) + 1) * sizeof(WCHAR)));
        }
    }

done:
    EpiSetDword(hDiag, L"PanelStatus", status);
    if (out != NULL) { ExFreePoolWithTag(out, EPI_POOL_TAG); }
    if (xml != NULL) { ExFreePoolWithTag(xml, EPI_POOL_TAG); }
    ZwClose(hDiag);
    ZwClose(hParams);
}

// ---- DDI call trace (WP2b-2 timing investigation) ----
// Appends "<seq>:<text>;" to an in-memory log (first ~3000 chars since start are kept) and mirrors it to
// EpiDiag\VidPnLog. Called from PASSIVE_LEVEL DDIs only (VidPn, child status, power, present count).
VOID BASIC_DISPLAY_DRIVER::EpiTrace(_In_z_ _Printf_format_string_ PCWSTR Format, ...)
{
    PAGED_CODE();
    WCHAR line[192];
    va_list args;
    va_start(args, Format);
    NTSTATUS st = RtlStringCchVPrintfW(line, RTL_NUMBER_OF(line), Format, args);
    va_end(args);
    if (!NT_SUCCESS(st) && st != STATUS_BUFFER_OVERFLOW) { return; }
    WCHAR head[16];
    if (!NT_SUCCESS(RtlStringCchPrintfW(head, RTL_NUMBER_OF(head), L"%u:", ++m_EpiLogSeq))) { return; }
    size_t used = 0;
    if (!NT_SUCCESS(RtlStringCchLengthW(m_EpiLog, RTL_NUMBER_OF(m_EpiLog), &used))) { return; }
    // When full, keep the beginning of the log but still refresh the counters and the last line.
    const BOOLEAN full = (used + wcslen(head) + wcslen(line) + 2 >= RTL_NUMBER_OF(m_EpiLog)) ? TRUE : FALSE;
    if (!full)
    {
        RtlStringCchCatW(m_EpiLog, RTL_NUMBER_OF(m_EpiLog), head);
        RtlStringCchCatW(m_EpiLog, RTL_NUMBER_OF(m_EpiLog), line);
        RtlStringCchCatW(m_EpiLog, RTL_NUMBER_OF(m_EpiLog), L";");
    }

    HANDLE hParams = NULL;
    HANDLE hDiag = NULL;
    if (!NT_SUCCESS(EpiOpenDiag(m_pPhysicalDevice, &hParams, &hDiag))) { return; }
    if (!full)
    {
        EpiSetValue(hDiag, L"VidPnLog", REG_SZ, m_EpiLog, (ULONG)((wcslen(m_EpiLog) + 1) * sizeof(WCHAR)));
    }
    EpiSetValue(hDiag, L"VidPnLast", REG_SZ, line, (ULONG)((wcslen(line) + 1) * sizeof(WCHAR)));
    EpiSetDword(hDiag, L"PresentCount", m_EpiPresentCount);
    EpiSetDword(hDiag, L"VsyncNotifies", (ULONG)m_Vsync.Notifies);
    EpiSetDword(hDiag, L"VsyncSkipped", (ULONG)m_Vsync.Skipped);
    EpiSetDword(hDiag, L"HwVsyncMode", (ULONG)m_Vsync.HwMode);
    EpiSetDword(hDiag, L"HwVsyncArmed", (ULONG)m_Vsync.HwArmed);
    EpiSetDword(hDiag, L"HwVsyncFailed", (ULONG)m_Vsync.HwFailed);
    EpiSetDword(hDiag, L"HwIsrCalls", (ULONG)m_Vsync.IsrCalls);
    EpiSetDword(hDiag, L"HwIsrFrames", (ULONG)m_Vsync.IsrFrames);
    EpiSetDword(hDiag, L"HwLastTop", m_Vsync.LastTop);
    EpiSetValue(hDiag, L"PresentUsTotal", REG_QWORD, &m_EpiPresentUs, sizeof(m_EpiPresentUs));
    EpiSetValue(hDiag, L"PresentPixelsTotal", REG_QWORD, &m_EpiPresentPixels, sizeof(m_EpiPresentPixels));
    EpiSetDword(hDiag, L"PresentMaxUs", m_EpiPresentMaxUs);
    EpiSetDword(hDiag, L"BrightEnabled", (ULONG)m_Dsi.Enabled);
    EpiSetDword(hDiag, L"BrightLevel", m_Dsi.Level);
    EpiSetDword(hDiag, L"BrightSends", m_Dsi.Sends);
    EpiSetDword(hDiag, L"BrightFails", m_Dsi.Fails);
    EpiSetDword(hDiag, L"TraceTimeMs", (ULONG)(KeQueryInterruptTime() / 10000));
    ZwClose(hDiag);
    ZwClose(hParams);
}

VOID BASIC_DISPLAY_DRIVER::EpiCountPresent(NTSTATUS Status, ULONG Us, _In_ CONST DXGKARG_PRESENT_DISPLAYONLY* pPresent)
{
    PAGED_CODE();
    ++m_EpiPresentCount;
    ULONGLONG px = 0;
    for (ULONG i = 0; i < pPresent->NumMoves; i++)
    {
        const RECT& r = pPresent->pMoves[i].DestRect;
        if (r.right > r.left && r.bottom > r.top) { px += (ULONGLONG)(r.right - r.left) * (ULONGLONG)(r.bottom - r.top); }
    }
    for (ULONG i = 0; i < pPresent->NumDirtyRects; i++)
    {
        const RECT& r = pPresent->pDirtyRect[i];
        if (r.right > r.left && r.bottom > r.top) { px += (ULONGLONG)(r.right - r.left) * (ULONGLONG)(r.bottom - r.top); }
    }
    m_EpiPresentUs += Us;
    m_EpiPresentPixels += px;
    if (Us > m_EpiPresentMaxUs) { m_EpiPresentMaxUs = Us; }
    m_EpiWinUs += Us;
    m_EpiWinPixels += (px > MAXULONG) ? MAXULONG : (ULONG)px;
    m_EpiWinCount++;
    // Trace the first present, failures, and then every 64th (keeps the registry traffic low).
    if (m_EpiPresentCount == 1 || !NT_SUCCESS(Status) || (m_EpiPresentCount & 0x3F) == 0)
    {
        // avg us / avg kpixels per present over the window
        EpiTrace(L"Present#%u st%X vs%u skip%u en%u us%u kpx%u", m_EpiPresentCount, (ULONG)Status,
                 (ULONG)m_Vsync.Notifies, (ULONG)m_Vsync.Skipped, (ULONG)m_Vsync.Enabled,
                 m_EpiWinUs / m_EpiWinCount, m_EpiWinPixels / m_EpiWinCount / 1000);
        m_EpiWinUs = 0;
        m_EpiWinPixels = 0;
        m_EpiWinCount = 0;
    }
}
