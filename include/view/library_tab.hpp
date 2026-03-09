/**
 * VitaPlex - Library Tab
 * Browse library sections and content
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class LibraryTab : public brls::Box {
public:
    LibraryTab();
    ~LibraryTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void loadSections();
    void loadContent(const std::string& sectionKey);
    void onSectionSelected(const LibrarySection& section);
    void onItemSelected(const MediaItem& item);
    void showAlbumContextMenu(const MediaItem& album);

    brls::Label* m_titleLabel = nullptr;
    brls::HScrollingFrame* m_sectionsScroll = nullptr;
    brls::Box* m_sectionsBox = nullptr;
    RecyclingGrid* m_contentGrid = nullptr;

    std::vector<LibrarySection> m_sections;
    std::vector<MediaItem> m_items;
    std::string m_currentSection;
    bool m_loaded = false;

    // Alive flag for crash prevention on quick tab switching
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
};

} // namespace vitaplex
