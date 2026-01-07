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
    void loadMusicCategories();
    void onPlay(bool resume = false);
    void onDownload();
    void showDownloadOptions();
    void downloadAll();
    void downloadUnwatched(int maxCount = -1);

    brls::HScrollingFrame* createMediaRow(const std::string& title, brls::Box** contentOut);

    MediaItem m_item;
    std::vector<MediaItem> m_children;

    // Main layout
    brls::ScrollingFrame* m_scrollView = nullptr;
    brls::Box* m_mainContent = nullptr;

    brls::Label* m_titleLabel = nullptr;
    brls::Label* m_yearLabel = nullptr;
    brls::Label* m_ratingLabel = nullptr;
    brls::Label* m_durationLabel = nullptr;
    brls::Label* m_summaryLabel = nullptr;
    brls::Image* m_posterImage = nullptr;
    brls::Button* m_playButton = nullptr;
    brls::Button* m_resumeButton = nullptr;
    brls::Button* m_downloadButton = nullptr;
    brls::Box* m_childrenBox = nullptr;

    // Music category rows for artists
    brls::Box* m_musicCategoriesBox = nullptr;
    brls::Box* m_albumsContent = nullptr;
    brls::Box* m_singlesContent = nullptr;
    brls::Box* m_epsContent = nullptr;
    brls::Box* m_compilationsContent = nullptr;
    brls::Box* m_soundtracksContent = nullptr;
};

} // namespace vitaplex
