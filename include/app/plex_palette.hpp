/**
 * VitaPlex — all-Plex palette tokens.
 *
 * One warm, premium dark palette shared by the borealis theme override
 * (Application::applyTheme) and any app-drawn control. Plex gold is the
 * accent (brand / active / selected / primary); focus is a bright warm
 * "gold-white" halo — same family, different treatment, so a focused
 * element never reads as the gold *selected* state.
 *
 * The two rules that keep focus and selection distinct on a TV/controller:
 *   1. Focus is a bright cream HALO/ring (focusHalo), never a fill.
 *   2. A focused element that is ALSO selected brightens its gold fill
 *      (goldBright) so the halo and the fill stay visually separate.
 *
 * Use these literal tokens everywhere a colour is set; do not re-hardcode
 * hex/rgb for these roles.
 */

#pragma once

#include <nanovg.h>

namespace vitaplex {
namespace palette {

// ── surfaces (warm charcoal) ───────────────────────────────────────
inline const NVGcolor bg        = nvgRGB(28, 26, 36);   // #1C1A24 app background
inline const NVGcolor panel     = nvgRGB(36, 34, 48);   // #242230 sidebars / sheets
inline const NVGcolor surface   = nvgRGB(44, 42, 56);   // #2C2A38 cards / cells
inline const NVGcolor surface2  = nvgRGB(52, 50, 63);   // #34323F raised / hover
inline const NVGcolor surface3  = nvgRGB(60, 58, 71);   // #3C3A47 chips / 2ndary buttons
inline const NVGcolor line      = nvgRGB(58, 56, 69);   // #3A3845 hairline borders

// ── text ───────────────────────────────────────────────────────────
inline const NVGcolor text      = nvgRGB(255, 255, 255); // #FFFFFF
inline const NVGcolor muted     = nvgRGB(168, 166, 180); // #A8A6B4
inline const NVGcolor dim       = nvgRGB(128, 126, 140); // #807E8C

// ── accent — Plex gold (brand / active / selected / primary) ───────
inline const NVGcolor gold       = nvgRGB(229, 160, 13); // #E5A00D
inline const NVGcolor goldBright = nvgRGB(255, 194, 61); // #FFC23D focused-AND-selected fill (rule 2)
inline const NVGcolor goldDeep   = nvgRGB(200, 135, 10); // #C8870A pressed
inline const NVGcolor goldInk    = nvgRGB(36, 28, 8);    // #241C08 text/glyphs ON gold (never white)

// gold @ alpha — tinted icon tiles / soft chips (default 16%).
inline NVGcolor goldTint(float a = 0.16f) {
    return nvgRGBA(229, 160, 13, static_cast<unsigned char>(a * 255.0f + 0.5f));
}

// ── focus — warm gold-white halo (NOT a fill) ──────────────────────
inline const NVGcolor focusHalo = nvgRGB(255, 212, 107);            // #FFD46B
inline const NVGcolor focusGlow = nvgRGBA(255, 212, 107, 115);      // outer soft glow (~0.45)
inline const NVGcolor focusMid  = nvgRGBA(229, 160, 13, 115);       // inner ring tie-in (~0.45)

// ── semantic (kept distinct from gold) ─────────────────────────────
inline const NVGcolor live      = nvgRGB(255, 86, 88);   // #FF5658 live / record
inline const NVGcolor ok        = nvgRGB(62, 207, 142);  // #3ECF8E ok / local
inline const NVGcolor info      = nvgRGB(137, 241, 242); // #89F1F2 info / remote

}  // namespace palette
}  // namespace vitaplex
