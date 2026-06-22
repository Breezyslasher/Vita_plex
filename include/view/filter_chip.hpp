/**
 * VitaPlex — FilterChip
 *
 * A pill button that implements the Plex "pick ladder" so focus and
 * selection stay two independent signals (palette rules 1-2):
 *
 *   default          -> surface-3 fill, muted label
 *   default + focused -> (same fill) + borealis warm halo
 *   picked            -> gold fill, ink label
 *   picked + focused  -> gold-bright fill, ink label  + warm halo  (rule 2)
 *
 * The focus HALO is borealis-native (now warm via the theme override); this
 * class only swaps the *fill* so a picked-and-focused chip brightens its gold
 * rather than reading the same as picked-at-rest. brls::Button::onFocusGained
 * only toggles the shadow (it does not re-apply the style background), so
 * restyling on focus is safe.
 */

#pragma once

#include <borealis/views/button.hpp>

#include "app/plex_palette.hpp"

namespace vitaplex {

class FilterChip : public brls::Button {
public:
    FilterChip() {
        this->setCornerRadius(16);
        this->setHighlightCornerRadius(16);
        this->setPadding(6, 16, 6, 16);
        restyle();
    }

    void setPicked(bool picked) {
        m_picked = picked;
        restyle();
    }
    bool isPicked() const { return m_picked; }

    void onFocusGained() override {
        brls::Button::onFocusGained();
        restyle();  // picked + focused -> brighten the gold fill (rule 2)
    }
    void onFocusLost() override {
        brls::Button::onFocusLost();
        restyle();
    }

private:
    void restyle() {
        namespace pal = vitaplex::palette;
        if (m_picked) {
            this->setBackgroundColor(this->isFocused() ? pal::goldBright : pal::gold);
            this->setTextColor(pal::goldInk);
            this->setBorderColor(pal::goldBright);
            this->setBorderThickness(1.5f);
        } else {
            // Not picked: neutral surface with a bright white label (kept
            // legible at rest); the warm halo signals focus, the gold fill
            // signals selection.
            this->setBackgroundColor(pal::surface3);
            this->setTextColor(pal::text);
            this->setBorderColor(nvgRGBA(0, 0, 0, 0));
            this->setBorderThickness(0.0f);
        }
    }

    bool m_picked = false;
};

}  // namespace vitaplex
