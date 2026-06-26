/**
 * VitaPlex - Search Tab
 * Two columns: an on-screen keyboard (left) and type-grouped result grids
 * (right). Driven by D-pad / touch; results refresh live as the query changes.
 */

#pragma once

#include <borealis.hpp>
#include <memory>
#include <atomic>
#include <string>
#include <vector>
#include "app/plex_client.hpp"

namespace vitaplex {

class SearchTab : public brls::Box {
public:
    SearchTab();
    ~SearchTab();

    void onFocusGained() override;
    void willDisappear(bool resetState) override;

private:
    void buildKeyboard(brls::Box* parent);

    // Query editing (each mutation refreshes the field + live results).
    void appendChar(const std::string& c);
    void backspace();
    void clearQuery();
    void updateField();

    void performSearch();
    void rebuildResults();
    void addSection(const std::string& title, const std::vector<MediaItem>& items);
    brls::Box* makeCard(const MediaItem& item);
    void onItemSelected(const MediaItem& item);

    // Left column
    brls::Label* m_queryLabel = nullptr;
    brls::Box*   m_keyboardFirstKey = nullptr;   // default focus target

    // Right column
    brls::ScrollingFrame* m_resultsScroll = nullptr;
    brls::Box*            m_resultsContent = nullptr;

    std::string m_query;
    std::vector<MediaItem> m_movies;
    std::vector<MediaItem> m_episodes;
    std::vector<MediaItem> m_shows;
    std::vector<MediaItem> m_artists;
    std::vector<MediaItem> m_albums;
    std::vector<MediaItem> m_tracks;

    // Alive flag + generation counter for crash prevention / stale results.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    int m_loadGeneration = 0;
    // ImageLoader needs an atomic flag; recycled per result rebuild.
    std::shared_ptr<std::atomic<bool>> m_imgAlive;
};

} // namespace vitaplex
