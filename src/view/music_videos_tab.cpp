/**
 * VitaPlex - Music Videos Tab: view/music_videos_tab.hpp.
 */

#include "view/music_videos_tab.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <thread>

#include "app/application.hpp"
#include "platform/paths.hpp"
#include "utils/async.hpp"
#include "view/filter_chip.hpp"
#include "view/media_detail_view.hpp"

namespace vitaplex {

namespace {

// The videos found, kept between runs: a first line "v1<tab><server id>",
// then one video a line, its fields separated by tabs (kFields of them).
const char* const kCacheFile = "music_videos.txt";
const char* const kCacheVersion = "v1";
constexpr int kFields = 7;   // ratingKey, title, artist, artistKey, thumb, duration, year

// Artists asked at once. Each asks the server for one artist's extras; a
// Vita has few threads to spare.
#ifdef __PSV__
constexpr int kWorkers = 2;
#else
constexpr int kWorkers = 4;
#endif
// Artists asked for a page.
constexpr int kArtistPage = 200;

struct SortOpt {
    const char* label;
    int field;   // 0 title, 1 artist, 2 year, 3 length
    bool descending;
};
const SortOpt kSorts[] = {
    {"Title A-Z", 0, false},  {"Title Z-A", 0, true},
    {"Artist A-Z", 1, false}, {"Artist Z-A", 1, true},
    {"Newest", 2, true},      {"Oldest", 2, false},
    {"Longest", 3, true},     {"Shortest", 3, false},
};
// The sort chosen, for as long as VitaPlex runs.
int s_sort = 0;

std::string lower(const std::string& s) {
    std::string out = s;
    for (char& c : out)
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
    return out;
}

// A field as the cache holds it: no tab or line end.
std::string field(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
    return s;
}

bool loadCache(const std::string& serverId, std::vector<MediaItem>& out) {
    FILE* f = std::fopen(platformPath(kCacheFile).c_str(), "rb");
    if (!f) return false;
    std::string text;
    char buf[8192];
    for (size_t n; (n = std::fread(buf, 1, sizeof(buf), f)) > 0;) text.append(buf, n);
    std::fclose(f);

    size_t at = text.find('\n');
    if (at == std::string::npos) return false;
    if (text.substr(0, at) != std::string(kCacheVersion) + "\t" + serverId) return false;
    out.clear();
    for (at++; at < text.size();) {
        size_t end = text.find('\n', at);
        if (end == std::string::npos) end = text.size();
        std::vector<std::string> parts;
        for (size_t p = at; p <= end;) {
            size_t tab = text.find('\t', p);
            if (tab == std::string::npos || tab > end) tab = end;
            parts.push_back(text.substr(p, tab - p));
            p = tab + 1;
        }
        at = end + 1;
        if ((int)parts.size() < kFields || parts[0].empty()) continue;
        MediaItem item;
        item.ratingKey = parts[0];
        item.title = parts[1];
        item.grandparentTitle = parts[2];
        item.grandparentRatingKey = parts[3];
        item.thumb = parts[4];
        item.duration = std::atoi(parts[5].c_str());
        item.year = std::atoi(parts[6].c_str());
        item.type = "clip";
        item.mediaType = MediaType::CLIP;
        item.extraType = 4;
        out.push_back(std::move(item));
    }
    return true;
}

void saveCache(const std::string& serverId, const std::vector<MediaItem>& videos) {
    const std::string path = platformPath(kCacheFile);
    const std::string part = path + ".part";
    FILE* f = std::fopen(part.c_str(), "wb");
    if (!f) return;
    std::string text = std::string(kCacheVersion) + "\t" + serverId + "\n";
    for (const auto& v : videos) {
        text += field(v.ratingKey) + "\t" + field(v.title) + "\t" + field(v.grandparentTitle) +
                "\t" + field(v.grandparentRatingKey) + "\t" + field(v.thumb) + "\t" +
                std::to_string(v.duration) + "\t" + std::to_string(v.year) + "\n";
    }
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    if (std::fclose(f) != 0 || !ok) {
        std::remove(part.c_str());
        return;
    }
    std::remove(path.c_str());
    std::rename(part.c_str(), path.c_str());
}

}  // namespace

struct MusicVideosTab::Scan {
    std::atomic<bool> cancel{false};
    std::vector<MediaItem> artists;   // set before the workers start
    std::atomic<size_t> next{0};      // the next artist to ask
    std::atomic<int> done{0};
    std::atomic<int> failed{0};
    std::atomic<int> workers{0};
    std::mutex mutex;
    std::vector<MediaItem> found;     // under mutex
};

MusicVideosTab::MusicVideosTab() {
    m_alive = std::make_shared<bool>(true);

    this->setAxis(brls::Axis::COLUMN);
    this->setJustifyContent(brls::JustifyContent::FLEX_START);
    this->setAlignItems(brls::AlignItems::STRETCH);
    this->setPadding(20);
    this->setGrow(1.0f);

    // Title row: the page title on the left, the count on the right.
    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::FLEX_END);
    titleRow->setMarginBottom(15);

    auto* title = new brls::Label();
    title->setText("Music Videos");
    title->setFontSize(28);
    titleRow->addView(title);

    auto* titleSpacer = new brls::Box();
    titleSpacer->setGrow(1.0f);
    titleRow->addView(titleSpacer);

    m_countLabel = new brls::Label();
    m_countLabel->setFontSize(13);
    m_countLabel->setTextColor(nvgRGB(0x8a, 0x8a, 0x90));
    m_countLabel->setMarginBottom(5);
    titleRow->addView(m_countLabel);
    this->addView(titleRow);

    // Toolbar: the artist filter and Rescan on the left, Sort on the right.
    auto* toolbar = new brls::Box();
    toolbar->setAxis(brls::Axis::ROW);
    toolbar->setAlignItems(brls::AlignItems::CENTER);
    toolbar->setMarginBottom(15);

    m_artistBtn = new FilterChip();
    m_artistBtn->setMarginRight(10);
    m_artistBtn->registerClickAction([this](brls::View*) {
        showArtistMenu();
        return true;
    });
    toolbar->addView(m_artistBtn);

    m_rescanBtn = new FilterChip();
    m_rescanBtn->setText("Rescan");
    m_rescanBtn->registerClickAction([this](brls::View*) {
        if (!m_scan) startScan();
        return true;
    });
    toolbar->addView(m_rescanBtn);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    toolbar->addView(spacer);

    m_sortBtn = new FilterChip();
    m_sortBtn->registerClickAction([this](brls::View*) {
        showSortMenu();
        return true;
    });
    toolbar->addView(m_sortBtn);
    this->addView(toolbar);

    m_statusLabel = new brls::Label();
    m_statusLabel->setFontSize(14);
    m_statusLabel->setTextColor(nvgRGB(0x8a, 0x8a, 0x90));
    m_statusLabel->setMarginBottom(10);
    m_statusLabel->setVisibility(brls::Visibility::GONE);
    this->addView(m_statusLabel);

    m_grid = new RecyclingGrid();
    m_grid->setGrow(1.0f);
    m_grid->setLandscapeCells(true);
    m_grid->setOnItemSelected([](const MediaItem& item) {
        Application::getInstance().pushPlayerActivity(item.ratingKey);
    });
    this->addView(m_grid);

    updateChips();
}

MusicVideosTab::~MusicVideosTab() {
    if (m_alive) *m_alive = false;
    if (m_scan) m_scan->cancel = true;
}

void MusicVideosTab::onFocusGained() {
    brls::Box::onFocusGained();
    if (m_loaded) return;
    m_loaded = true;

    // From the file a scan before left, read off the UI thread; else a scan.
    const std::string serverId = PlexClient::getInstance().getMachineIdentifier();
    std::weak_ptr<bool> aliveWeak = m_alive;
    asyncRun([this, aliveWeak, serverId]() {
        std::vector<MediaItem> cached;
        const bool have = loadCache(serverId, cached);
        brls::sync([this, aliveWeak, have, cached = std::move(cached)]() mutable {
            auto alive = aliveWeak.lock();
            if (!alive || !*alive) return;
            if (have) {
                brls::Logger::info("MusicVideosTab: {} video(s) from the last scan", cached.size());
                m_all = std::move(cached);
                applyView();
            } else {
                startScan();
            }
        });
    });
}

void MusicVideosTab::startScan() {
    if (m_scan) return;
    auto scan = std::make_shared<Scan>();
    m_scan = scan;
    if (m_statusLabel) {
        m_statusLabel->setText("Looking for music videos\xE2\x80\xA6");
        m_statusLabel->setVisibility(brls::Visibility::VISIBLE);
    }
    updateChips();

    std::weak_ptr<bool> aliveWeak = m_alive;
    const std::string serverId = PlexClient::getInstance().getMachineIdentifier();
    asyncRun([this, aliveWeak, scan, serverId]() {
        PlexClient& client = PlexClient::getInstance();

        // Every artist of every music library.
        bool listed = false;
        std::vector<LibrarySection> sections;
        if (client.fetchLibrarySections(sections)) {
            listed = true;
            for (const auto& section : sections) {
                if (section.type != "artist") continue;
                for (int offset = 0; !scan->cancel;) {
                    std::vector<MediaItem> page;
                    int total = 0;
                    if (!client.fetchLibraryContent(section.key, page, 8, kArtistPage, offset,
                                                    &total)) {
                        listed = false;
                        break;
                    }
                    for (auto& artist : page) {
                        MediaItem a;
                        a.ratingKey = artist.ratingKey;
                        a.title = artist.title;
                        scan->artists.push_back(std::move(a));
                    }
                    offset += (int)page.size();
                    if (page.empty() || offset >= total) break;
                }
            }
        }
        const int total = (int)scan->artists.size();
        brls::Logger::info("MusicVideosTab: asking {} artist(s) for their music videos", total);

        // Each worker takes the next artist until none is left.
        const int workers = std::min(kWorkers, std::max(total, 1));
        scan->workers = workers;
        for (int w = 0; w < workers; w++) {
            asyncRun([scan]() {
                PlexClient& client = PlexClient::getInstance();
                for (;;) {
                    const size_t i = scan->next.fetch_add(1);
                    if (scan->cancel || i >= scan->artists.size()) break;
                    const MediaItem& artist = scan->artists[i];
                    std::vector<MediaItem> extras;
                    if (!client.fetchExtras(artist.ratingKey, extras)) {
                        scan->failed++;
                    } else {
                        std::lock_guard<std::mutex> lock(scan->mutex);
                        for (auto& extra : extras) {
                            if (!extra.isMusicVideoExtra()) continue;
                            extra.grandparentTitle = artist.title;
                            extra.grandparentRatingKey = artist.ratingKey;
                            extra.type = "clip";
                            extra.mediaType = MediaType::CLIP;
                            extra.trimForGrid();
                            scan->found.push_back(std::move(extra));
                        }
                    }
                    scan->done++;
                }
                scan->workers--;
            });
        }

        // Progress to the tab twice a second while they work.
        while (scan->workers > 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            if (scan->cancel) continue;
            int found;
            {
                std::lock_guard<std::mutex> lock(scan->mutex);
                found = (int)scan->found.size();
            }
            const int done = scan->done;
            brls::sync([this, aliveWeak, done, total, found]() {
                auto alive = aliveWeak.lock();
                if (alive && *alive) onScanProgress(done, total, found);
            });
        }
        if (scan->cancel) return;

        std::vector<MediaItem> videos;
        {
            std::lock_guard<std::mutex> lock(scan->mutex);
            videos = std::move(scan->found);
        }
        // Whole only if every artist answered: a scan cut short by a server
        // out of reach is shown, but not kept in place of the last whole one.
        const bool complete = listed && scan->failed == 0;
        brls::Logger::info("MusicVideosTab: {} video(s) from {} artist(s), {} not answering{}",
                           videos.size(), total, scan->failed.load(),
                           listed ? "" : "; the artists could not all be listed");
        if (complete) saveCache(serverId, videos);
        brls::sync([this, aliveWeak, complete, videos = std::move(videos)]() mutable {
            auto alive = aliveWeak.lock();
            if (alive && *alive) onScanDone(std::move(videos), complete);
        });
    });
}

void MusicVideosTab::onScanProgress(int done, int total, int found) {
    if (!m_scan || !m_statusLabel) return;
    m_statusLabel->setText("Looking for music videos\xE2\x80\xA6 " + std::to_string(done) + " of " +
                           std::to_string(total) + " artists, " + std::to_string(found) +
                           " found");
}

void MusicVideosTab::onScanDone(std::vector<MediaItem>&& videos, bool complete) {
    m_scan.reset();
    // A scan cut short does not empty a list the last whole one filled.
    if (complete || videos.size() >= m_all.size()) m_all = std::move(videos);
    // The artist shown alone may have none now.
    bool artistStillThere = m_artistKey.empty();
    for (const auto& v : m_all)
        if (v.grandparentRatingKey == m_artistKey) artistStillThere = true;
    if (!artistStillThere) {
        m_artistKey.clear();
        m_artistName.clear();
    }
    applyView();
    if (!complete && m_statusLabel) {
        m_statusLabel->setText("The server did not answer for every artist: Rescan to try again.");
        m_statusLabel->setVisibility(brls::Visibility::VISIBLE);
    }
}

void MusicVideosTab::applyView() {
    std::vector<MediaItem> view;
    view.reserve(m_all.size());
    for (const auto& v : m_all)
        if (m_artistKey.empty() || v.grandparentRatingKey == m_artistKey) view.push_back(v);

    const SortOpt& sort = kSorts[s_sort];
    std::stable_sort(view.begin(), view.end(), [&sort](const MediaItem& a, const MediaItem& b) {
        const std::string at = lower(a.title), bt = lower(b.title);
        auto less = [&](const MediaItem& x, const MediaItem& y, const std::string& xt,
                        const std::string& yt) {
            switch (sort.field) {
                case 1: {
                    const std::string xa = lower(x.grandparentTitle),
                                      ya = lower(y.grandparentTitle);
                    if (xa != ya) return xa < ya;
                    break;
                }
                case 2:
                    if (x.year != y.year) return x.year < y.year;
                    break;
                case 3:
                    if (x.duration != y.duration) return x.duration < y.duration;
                    break;
                default:
                    break;
            }
            return xt < yt;
        };
        return sort.descending ? less(b, a, bt, at) : less(a, b, at, bt);
    });

    if (m_grid) m_grid->setDataSource(view);
    if (m_countLabel)
        m_countLabel->setText(std::to_string(view.size()) +
                              (view.size() == 1 ? " video" : " videos"));
    if (m_statusLabel) {
        if (view.empty() && !m_scan) {
            m_statusLabel->setText("No music videos found on this server.");
            m_statusLabel->setVisibility(brls::Visibility::VISIBLE);
        } else if (!m_scan) {
            m_statusLabel->setVisibility(brls::Visibility::GONE);
        }
    }
    updateChips();
}

void MusicVideosTab::updateChips() {
    if (m_artistBtn) {
        m_artistBtn->setText(m_artistKey.empty() ? "Artist: All" : "Artist: " + m_artistName);
        if (auto* chip = dynamic_cast<FilterChip*>(m_artistBtn))
            chip->setPicked(!m_artistKey.empty());
    }
    if (m_rescanBtn) m_rescanBtn->setText(m_scan ? "Scanning\xE2\x80\xA6" : "Rescan");
    if (m_sortBtn) m_sortBtn->setText(std::string("Sort: ") + kSorts[s_sort].label);
}

void MusicVideosTab::showSortMenu() {
    std::vector<OptionRow> rows;
    const int count = (int)(sizeof(kSorts) / sizeof(kSorts[0]));
    for (int i = 0; i < count; i++) {
        rows.push_back({i == s_sort ? "check-circle.png" : "", kSorts[i].label, "", false, false,
                        [this, i](brls::View*) {
                            s_sort = i;
                            applyView();
                            return true;
                        }});
    }
    MediaDetailView::showCenteredChoice("Sort by", "", std::move(rows), /*scrollable=*/true);
}

void MusicVideosTab::showArtistMenu() {
    // The artists that have videos, by name, each with how many.
    struct Entry {
        std::string key, name;
        int count = 0;
    };
    std::map<std::string, Entry> byKey;
    for (const auto& v : m_all) {
        Entry& e = byKey[v.grandparentRatingKey];
        e.key = v.grandparentRatingKey;
        e.name = v.grandparentTitle;
        e.count++;
    }
    std::vector<Entry> artists;
    for (auto& kv : byKey) artists.push_back(std::move(kv.second));
    std::sort(artists.begin(), artists.end(),
              [](const Entry& a, const Entry& b) { return lower(a.name) < lower(b.name); });

    std::vector<OptionRow> rows;
    rows.push_back({m_artistKey.empty() ? "check-circle.png" : "", "All Artists",
                    std::to_string(m_all.size()), false, false, [this](brls::View*) {
                        m_artistKey.clear();
                        m_artistName.clear();
                        applyView();
                        return true;
                    }});
    for (const auto& a : artists) {
        const std::string key = a.key, name = a.name;
        rows.push_back({key == m_artistKey ? "check-circle.png" : "", name,
                        std::to_string(a.count), false, false, [this, key, name](brls::View*) {
                            m_artistKey = key;
                            m_artistName = name;
                            applyView();
                            return true;
                        }});
    }
    MediaDetailView::showCenteredChoice("Artist", "", std::move(rows), /*scrollable=*/true);
}

}  // namespace vitaplex
