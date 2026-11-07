// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - EDID synthesis from the panel XML (PCFG). New file, not derived from KMDOD.
// Layout follows what qcdxkm7325 produced on spacewar (golden EDID of monitor SHP1C70, 2026-11-07):
//   base block EDID 1.x, header/vendor/chromaticity straight from the 'EDID Configuration' group,
//   no established/standard timings, DTD1 = panel active timing, DTD2 dummy, DTD3 = name (0xFC), DTD4 dummy.
#include "epi_edid.hxx"

#ifdef _KERNEL_MODE
#pragma code_seg("PAGE")
#endif

namespace {

const char* FindSeq(const char* p, const char* end, const char* seq)
{
    unsigned int n = 0;
    while (seq[n] != '\0') { ++n; }
    if (n == 0) { return p; }
    for (; p + n <= end; ++p)
    {
        unsigned int i = 0;
        while (i < n && p[i] == seq[i]) { ++i; }
        if (i == n) { return p; }
    }
    return nullptr;
}

// Returns a pointer just after "<Name>" (exact, so "<Name/>" does not match), or nullptr.
const char* FindTagValue(const char* xml, const char* end, const char* name)
{
    char open[64];
    unsigned int k = 0;
    open[k++] = '<';
    for (unsigned int i = 0; name[i] != '\0'; ++i)
    {
        if (k >= sizeof(open) - 2) { return nullptr; }
        open[k++] = name[i];
    }
    open[k++] = '>';
    open[k] = '\0';
    const char* p = FindSeq(xml, end, open);
    return p ? p + k : nullptr;
}

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }

// Decimal or 0x-hex number followed (after optional spaces) by '<'.
bool ParseNumber(const char* p, const char* end, unsigned int* value)
{
    while (p < end && IsSpace(*p)) { ++p; }
    unsigned long long v = 0;
    unsigned int digits = 0;
    if (p + 1 < end && p[0] == '0' && (p[1] == 'x' || p[1] == 'X'))
    {
        p += 2;
        for (; p < end; ++p, ++digits)
        {
            char c = *p;
            unsigned int d;
            if (c >= '0' && c <= '9') { d = (unsigned int)(c - '0'); }
            else if (c >= 'a' && c <= 'f') { d = (unsigned int)(c - 'a' + 10); }
            else if (c >= 'A' && c <= 'F') { d = (unsigned int)(c - 'A' + 10); }
            else { break; }
            v = v * 16 + d;
            if (v > 0xFFFFFFFFull) { return false; }
        }
    }
    else
    {
        for (; p < end && *p >= '0' && *p <= '9'; ++p, ++digits)
        {
            v = v * 10 + (unsigned int)(*p - '0');
            if (v > 0xFFFFFFFFull) { return false; }
        }
    }
    if (digits == 0) { return false; }
    while (p < end && IsSpace(*p)) { ++p; }
    if (p >= end || *p != '<') { return false; }
    *value = (unsigned int)v;
    return true;
}

bool TagIsTrue(const char* xml, const char* end, const char* name)
{
    const char* p = FindTagValue(xml, end, name);
    if (p == nullptr) { return false; }
    while (p < end && IsSpace(*p)) { ++p; }
    return p < end && (*p == 'T' || *p == 't');
}

enum Tag
{
    T_ManufactureID, T_ProductCode, T_SerialNumber, T_Week, T_Year, T_Version, T_Revision,
    T_VideoInput, T_HSizeCm, T_VSizeCm, T_Gamma, T_Feature,
    T_RG, T_BW, T_RX, T_RY, T_GX, T_GY, T_BX, T_BY, T_WX, T_WY,
    T_Est1, T_Est2, T_MfrTiming,
    T_HActive, T_HFP, T_HBP, T_HSync, T_VActive, T_VFP, T_VBP, T_VSync, T_Refresh,
    T_Count
};

const char* const g_TagNames[T_Count] = {
    "ManufactureID", "ProductCode", "SerialNumber", "WeekofManufacture", "YearofManufacture", "EDIDVersion", "EDIDRevision",
    "VideoInputDefinition", "HorizontalScreenSize", "VerticalScreenSize", "DisplayTransferCharacteristics", "FeatureSupport",
    "Red.GreenBits", "Blue.WhiteBits", "RedX", "RedY", "GreenX", "GreenY", "BlueX", "BlueY", "WhiteX", "WhiteY",
    "EstablishedTimingsI", "EstablishedTimingsII", "ManufacturesTiming",
    "HorizontalActive", "HorizontalFrontPorch", "HorizontalBackPorch", "HorizontalSyncPulse",
    "VerticalActive", "VerticalFrontPorch", "VerticalBackPorch", "VerticalSyncPulse", "DSIRefreshRate",
};

void DummyDescriptor(unsigned char* d)
{
    for (unsigned int i = 0; i < 18; ++i) { d[i] = 0; }
    d[3] = 0x10;
}

} // namespace

int EpiEdidFromPanelXml(const char* Xml, unsigned int XmlLen,
                        unsigned char Edid[EPI_EDID_SIZE], EPI_PANEL_TIMING* Timing, int* MissingTag)
{
    const char* end = Xml + XmlLen;
    *MissingTag = -1;
    for (unsigned int i = 0; i < EPI_EDID_SIZE; ++i) { Edid[i] = 0; }
    for (unsigned int i = 0; i < sizeof(*Timing); ++i) { reinterpret_cast<unsigned char*>(Timing)[i] = 0; }

    if (FindSeq(Xml, end, "id='EDID Configuration'") == nullptr &&
        FindSeq(Xml, end, "id=\"EDID Configuration\"") == nullptr)
    {
        return EPI_EDID_NO_EDID_GROUP;
    }

    unsigned int v[T_Count];
    for (int t = 0; t < T_Count; ++t)
    {
        const char* p = FindTagValue(Xml, end, g_TagNames[t]);
        if (p == nullptr || !ParseNumber(p, end, &v[t])) { *MissingTag = t; return EPI_EDID_MISSING_TAG; }
    }

    Timing->HActive = v[T_HActive]; Timing->HFrontPorch = v[T_HFP]; Timing->HBackPorch = v[T_HBP]; Timing->HSyncPulse = v[T_HSync];
    Timing->VActive = v[T_VActive]; Timing->VFrontPorch = v[T_VFP]; Timing->VBackPorch = v[T_VBP]; Timing->VSyncPulse = v[T_VSync];
    Timing->RefreshQ16 = v[T_Refresh];
    Timing->InvertHsync = TagIsTrue(Xml, end, "InvertHsyncPolarity") ? 1u : 0u;
    Timing->InvertVsync = (TagIsTrue(Xml, end, "InvertVsyncPolairty") || TagIsTrue(Xml, end, "InvertVsyncPolarity")) ? 1u : 0u;

    const unsigned int hblank = Timing->HFrontPorch + Timing->HBackPorch + Timing->HSyncPulse;
    const unsigned int vblank = Timing->VFrontPorch + Timing->VBackPorch + Timing->VSyncPulse;
    const unsigned long long htotal = Timing->HActive + hblank;
    const unsigned long long vtotal = Timing->VActive + vblank;
    const unsigned long long pclk10k = ((htotal * vtotal * Timing->RefreshQ16) >> 16) / 10000ull;
    const unsigned int hmm = v[T_HSizeCm] * 10, vmm = v[T_VSizeCm] * 10;
    if (Timing->HActive == 0 || Timing->VActive == 0 || Timing->RefreshQ16 == 0 ||
        Timing->HActive > 0xFFF || hblank > 0xFFF || Timing->VActive > 0xFFF || vblank > 0xFFF ||
        Timing->HFrontPorch > 0x3FF || Timing->HSyncPulse > 0x3FF || Timing->VFrontPorch > 0x3F || Timing->VSyncPulse > 0x3F ||
        pclk10k == 0 || pclk10k > 0xFFFF || hmm > 0xFFF || vmm > 0xFFF)
    {
        return EPI_EDID_BAD_TIMING;
    }
    Timing->PixelClock10kHz = (unsigned int)pclk10k;

    unsigned char* e = Edid;
    e[0] = 0x00; for (int i = 1; i <= 6; ++i) { e[i] = 0xFF; } e[7] = 0x00;
    e[8] = (unsigned char)(v[T_ManufactureID] & 0xFF);            // PCFG holds the big-endian vendor word byte-swapped
    e[9] = (unsigned char)((v[T_ManufactureID] >> 8) & 0xFF);
    e[10] = (unsigned char)(v[T_ProductCode] & 0xFF);
    e[11] = (unsigned char)((v[T_ProductCode] >> 8) & 0xFF);
    for (int i = 0; i < 4; ++i) { e[12 + i] = (unsigned char)((v[T_SerialNumber] >> (8 * i)) & 0xFF); }
    e[16] = (unsigned char)v[T_Week]; e[17] = (unsigned char)v[T_Year];
    e[18] = (unsigned char)v[T_Version]; e[19] = (unsigned char)v[T_Revision];
    e[20] = (unsigned char)v[T_VideoInput]; e[21] = (unsigned char)v[T_HSizeCm]; e[22] = (unsigned char)v[T_VSizeCm];
    e[23] = (unsigned char)v[T_Gamma]; e[24] = (unsigned char)v[T_Feature];
    for (int t = T_RG; t <= T_WY; ++t) { e[25 + (t - T_RG)] = (unsigned char)v[t]; }
    e[35] = (unsigned char)v[T_Est1]; e[36] = (unsigned char)v[T_Est2]; e[37] = (unsigned char)v[T_MfrTiming];
    for (int i = 38; i < 54; ++i) { e[i] = 0x01; }                 // standard timings: unused

    unsigned char* d = e + 54;                                    // DTD1: panel timing
    d[0] = (unsigned char)(pclk10k & 0xFF); d[1] = (unsigned char)(pclk10k >> 8);
    d[2] = (unsigned char)(Timing->HActive & 0xFF); d[3] = (unsigned char)(hblank & 0xFF);
    d[4] = (unsigned char)(((Timing->HActive >> 8) << 4) | (hblank >> 8));
    d[5] = (unsigned char)(Timing->VActive & 0xFF); d[6] = (unsigned char)(vblank & 0xFF);
    d[7] = (unsigned char)(((Timing->VActive >> 8) << 4) | (vblank >> 8));
    d[8] = (unsigned char)(Timing->HFrontPorch & 0xFF); d[9] = (unsigned char)(Timing->HSyncPulse & 0xFF);
    d[10] = (unsigned char)(((Timing->VFrontPorch & 0xF) << 4) | (Timing->VSyncPulse & 0xF));
    d[11] = (unsigned char)(((Timing->HFrontPorch >> 8) << 6) | ((Timing->HSyncPulse >> 8) << 4) |
                            ((Timing->VFrontPorch >> 4) << 2) | (Timing->VSyncPulse >> 4));
    d[12] = (unsigned char)(hmm & 0xFF); d[13] = (unsigned char)(vmm & 0xFF);
    d[14] = (unsigned char)(((hmm >> 8) << 4) | (vmm >> 8));
    d[15] = 0; d[16] = 0;
    d[17] = (unsigned char)(0x18 | (Timing->InvertVsync ? 0 : 0x04) | (Timing->InvertHsync ? 0 : 0x02)); // digital separate sync

    DummyDescriptor(e + 72);

    d = e + 90;                                                   // DTD3: display product name
    d[0] = 0; d[1] = 0; d[2] = 0; d[3] = 0xFC; d[4] = 0;
    const char* name = FindTagValue(Xml, end, "PanelName");
    unsigned int n = 0;
    if (name != nullptr)
    {
        for (; n < 13 && name + n < end && name[n] != '<'; ++n) { d[5 + n] = (unsigned char)name[n]; }
    }
    if (n < 13) { d[5 + n++] = 0x0A; }
    for (; n < 13; ++n) { d[5 + n] = 0x20; }

    DummyDescriptor(e + 108);

    e[126] = 0;                                                   // no extension blocks
    unsigned int sum = 0;
    for (int i = 0; i < 127; ++i) { sum += e[i]; }
    e[127] = (unsigned char)((256 - (sum & 0xFF)) & 0xFF);
    return EPI_EDID_OK;
}
