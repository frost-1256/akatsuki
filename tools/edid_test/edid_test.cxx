// SPDX-License-Identifier: MIT
// Host test: synthesize the EDID from a panel XML and compare it with a golden 128-byte EDID (hex string file).
#include <cstdio>
#include <cstdlib>
#include <vector>
#include "../../src/epinephrine/epi_edid.hxx"

static std::vector<char> ReadAll(const char* path)
{
    std::vector<char> b; FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || f == nullptr) { return b; }
    char tmp[4096]; size_t n;
    while ((n = fread(tmp, 1, sizeof(tmp), f)) > 0) { b.insert(b.end(), tmp, tmp + n); }
    fclose(f); return b;
}

int main(int argc, char** argv)
{
    if (argc < 3) { printf("usage: edid_test <panel.xml> <golden.hex>\n"); return 2; }
    std::vector<char> xml = ReadAll(argv[1]);
    std::vector<char> hex = ReadAll(argv[2]);
    unsigned char golden[EPI_EDID_SIZE]; unsigned int g = 0; int hi = -1;
    for (char c : hex)
    {
        int d = (c >= '0' && c <= '9') ? c - '0' : (c >= 'a' && c <= 'f') ? c - 'a' + 10 : (c >= 'A' && c <= 'F') ? c - 'A' + 10 : -1;
        if (d < 0) { continue; }
        if (hi < 0) { hi = d; } else { if (g < EPI_EDID_SIZE) { golden[g++] = (unsigned char)(hi * 16 + d); } hi = -1; }
    }
    if (xml.empty() || g != EPI_EDID_SIZE) { printf("input error xml=%zu golden=%u\n", xml.size(), g); return 2; }
    unsigned char edid[EPI_EDID_SIZE]; EPI_PANEL_TIMING t; int missing;
    int rc = EpiEdidFromPanelXml(xml.data(), (unsigned int)xml.size(), edid, &t, &missing);
    printf("rc=%d missing=%d active=%ux%u h(%u,%u,%u) v(%u,%u,%u) refreshQ16=0x%X pclk10k=%u invH=%u invV=%u\n", rc, missing,
           t.HActive, t.VActive, t.HFrontPorch, t.HBackPorch, t.HSyncPulse, t.VFrontPorch, t.VBackPorch, t.VSyncPulse,
           t.RefreshQ16, t.PixelClock10kHz, t.InvertHsync, t.InvertVsync);
    if (rc != 0) { return 1; }
    int diffs = 0;
    for (int i = 0; i < EPI_EDID_SIZE; ++i) { if (edid[i] != golden[i]) { printf("  diff @%3d syn=%02X gold=%02X\n", i, edid[i], golden[i]); ++diffs; } }
    printf("diffs=%d\n", diffs);
    return diffs == 0 ? 0 : 1;
}
