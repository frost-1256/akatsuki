// SPDX-License-Identifier: MIT
// Epinephrine (Akatsuki Project) - EDID synthesis from the panel XML (PCFG, GPU0 _ROM). New file, not derived from KMDOD.
// Pure code: no kernel or WDK headers, so the same file builds into the driver and into the host test (tools/edid_test).
#pragma once

#define EPI_EDID_SIZE 128

typedef struct _EPI_PANEL_TIMING
{
    unsigned int HActive, HFrontPorch, HBackPorch, HSyncPulse;
    unsigned int VActive, VFrontPorch, VBackPorch, VSyncPulse;
    unsigned int RefreshQ16;      // DSIRefreshRate, 16.16 fixed point (0x3C0000 = 60 Hz)
    unsigned int InvertHsync;     // InvertHsyncPolarity == True
    unsigned int InvertVsync;     // InvertVsyncPolairty (sic, PCFG tag name) == True
    unsigned int PixelClock10kHz; // as written to the EDID detailed timing (truncated)
} EPI_PANEL_TIMING;

// Result codes for EpiEdidFromPanelXml (0 = success). Kept stable: they are recorded in EpiDiag.
enum
{
    EPI_EDID_OK = 0,
    EPI_EDID_NO_EDID_GROUP = 1,   // no <Group id='EDID Configuration'>
    EPI_EDID_MISSING_TAG = 2,     // a required tag is missing or not a number
    EPI_EDID_BAD_TIMING = 3,      // a timing value does not fit the EDID detailed timing fields
};

// Parses Xml[0..XmlLen) and fills Edid (128 bytes, checksum included) and Timing.
// MissingTag receives the index of the first missing required tag (or -1).
int EpiEdidFromPanelXml(const char* Xml, unsigned int XmlLen,
                        unsigned char Edid[EPI_EDID_SIZE], EPI_PANEL_TIMING* Timing, int* MissingTag);
