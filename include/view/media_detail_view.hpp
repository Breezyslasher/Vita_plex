/**
 * VitaPlex - Media Detail View
 * Shows detailed information about a media item
 */

#pragma once

#include <borealis.hpp>
#include "app/plex_client.hpp"

namespace vitaplex {

class MediaDetailView : public brls::Box {
public:
    MediaDetailView(const MediaItem& item);

    static brls::View* create();

private:
    void loadDetails();
    void loadChildren();
    void onPlay(bool resume = false);

    MediaItem m_item;
    std::vector<MediaItem> m_children;

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_yearLabel = nullptr;
    brls::Label* m_ratingLabel = nullptr;
    brls::Label* m_durationLabel = nullptr;
    brls::Label* m_summaryLabel = nullptr;
    brls::Image* m_posterImage = nullptr;
    brls::Button* m_playButton = nullptr;
    brls::Button* m_resumeButton = nullptr;
    brls::Box* m_childrenBox = nullptr;
};

} // namespace vitaplex
