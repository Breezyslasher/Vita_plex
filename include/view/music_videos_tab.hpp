/**
 * VitaPlex - Music Videos Tab
 * Every music video on the server in one grid, sorted by title, artist, year
 * or length, and narrowed to one artist.
 *
 * Plex has no list of them: a music video is an extra of its artist
 * (/library/metadata/<artist>/extras, extraType 4), which is how the artist's
 * page shows its own. So the tab asks each artist of each music library in
 * turn, once, and keeps what it found in a file (music_videos.txt in the data
 * folder) that it opens from after that. "Rescan" asks again.
 */

#pragma once

#include <borealis.hpp>
#include <atomic>
#include <memory>
#include <string>
#include <vector>
#include "app/plex_client.hpp"
#include "view/recycling_grid.hpp"

namespace vitaplex {

class MusicVideosTab : public brls::Box {
public:
    MusicVideosTab();
    ~MusicVideosTab() override;

    void willAppear(bool resetState = false) override;

private:
    // What a scan shares with its threads, which may outlive the tab.
    struct Scan;

    void startScan();
    void onScanProgress(int done, int total, int found);
    void onScanDone(std::vector<MediaItem>&& videos, bool complete);
    // The grid from m_all, by the artist filter and the sort.
    void applyView();
    void showSortMenu();
    void showArtistMenu();
    void updateChips();

    brls::Label* m_countLabel = nullptr;
    brls::Label* m_statusLabel = nullptr;
    brls::Button* m_artistBtn = nullptr;
    brls::Button* m_rescanBtn = nullptr;
    brls::Button* m_sortBtn = nullptr;
    RecyclingGrid* m_grid = nullptr;

    std::vector<MediaItem> m_all;     // every video; grandparentTitle its artist
    std::string m_artistKey;          // the artist shown alone (its name, lower case), "" for all
    std::string m_artistName;
    bool m_loaded = false;
    std::shared_ptr<Scan> m_scan;     // the scan running, if one is
    std::shared_ptr<bool> m_alive;
};

}  // namespace vitaplex
