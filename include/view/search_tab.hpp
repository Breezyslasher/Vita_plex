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

public:
    // Which shape the tab is built in. Phones get one of the two mobile
    // layouts; everything else keeps the desktop/TV two-column form, whose
    // 300px keyboard column is unusable at phone width.
    enum class Layout {
        TwoColumn,   // desktop / TV: keyboard beside the results
        NativeIme,   // phone A: no on-screen keyboard, tap the field for the system one
        OnScreen,    // phone B: grid keyboard docked along the bottom
    };

private:
    // Resolve the layout from the setting and the current screen. Re-read on
    // rebuild so a rotation or a setting change lands.
    static Layout resolveLayout();
    static bool   isPhoneSized();

    // Build the whole tab for `m_layout`. Split out of the constructor so the
    // A/B switch can tear down and re-run it without a relaunch.
    void buildLayout();
    void buildTwoColumn();
    void buildMobile();
    // `dock` true puts the keyboard in a bottom bar sized for touch, false
    // builds the narrow desktop column.
    void buildKeyboard(brls::Box* parent, bool dock = false);
    // Swap between the two mobile layouts for this session only — the FAB in A
    // and "Hide" in B. Does not write the setting.
    void switchMobileLayout(Layout to);
    // Open the platform IME on the query. The callback returns the finished
    // string, so results refresh on commit rather than per character.
    void openIme();

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

    Layout m_layout = Layout::TwoColumn;

    // Left column
    brls::Label* m_queryLabel = nullptr;
    brls::Box*   m_keyboardFirstKey = nullptr;   // default focus target
    // Clear affordance on the field; only shown once there is a query.
    brls::Box*   m_clearButton = nullptr;

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
    // Live TV programmes. The EPG provider has its own search feature,
    // separate from the server's, so these arrive from a second request.
    std::vector<MediaItem> m_liveTV;

    // Alive flag + generation counter for crash prevention / stale results.
    std::shared_ptr<bool> m_alive = std::make_shared<bool>(true);
    int m_loadGeneration = 0;
    // ImageLoader needs an atomic flag; recycled per result rebuild.
    std::shared_ptr<std::atomic<bool>> m_imgAlive;

    // Physical-keyboard typing (desktop / USB keyboards). Subscribed to the
    // raw key event for this tab's lifetime; unsubscribed in the destructor.
    brls::InputManager* m_inputManager = nullptr;
    brls::Event<brls::KeyState>::Subscription m_kbSub;
    bool m_kbSubscribed = false;
};

} // namespace vitaplex
