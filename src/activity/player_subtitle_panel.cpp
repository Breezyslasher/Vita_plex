/**
 * VitaPlex - the subtitle panel over the video OSD
 *
 * The subtitle buttons of the video controls open this instead of the plain
 * track list: a row of four drop-downs along the bottom of the picture,
 * Subtitles (on / off), Language, Size and Style, the video playing on above
 * them so a change shows at once. Each opens its list just above itself; a
 * pick applies straight away and the list closes. Back (or a tap on the
 * picture) closes the list, then the panel.
 *
 * Built in code, as the rest of the OSD's code-made rows are, and sized with
 * ui() so it matches the OSD on every screen.
 */

#include "activity/player_activity.hpp"
#include "app/application.hpp"
#include "player/mpv_player.hpp"
#include "app/plex_palette.hpp"
#include "app/plex_client.hpp"
#include "utils/async.hpp"

#include <borealis.hpp>
#include <yoga/Yoga.h>   // flex-wrap, which borealis does not expose

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <chrono>
#include <thread>
#include <string>
#include <vector>

namespace vitaplex {

namespace {

// SUB_SEARCH is a list with no drop-down of its own: it opens from the
// language list, or by itself when subtitles are turned on and there are none.
// SUB_AUDIO is the audio track list, opened from the OSD's Audio pill with
// no panel behind it.
enum Which { SUB_ONOFF = 0, SUB_LANGUAGE = 1, SUB_SIZE = 2, SUB_STYLE = 3, SUB_TIMING = 4,
             SUB_SEARCH = 5, SUB_AUDIO = 6 };
constexpr int kSubControls = 5;

// "In time", "0.5 s later", "1.25 s earlier".
std::string timingName(double d) {
    if (std::fabs(d) < 0.001) return "In time";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%g s %s", std::fabs(d), d > 0 ? "later" : "earlier");
    return buf;
}

// The timing steps: a quarter second near zero, half a second further out.
std::vector<double> timingSteps() {
    std::vector<double> v;
    for (double d = -10.0; d < -3.0 + 1e-9; d += 0.5) v.push_back(d);
    for (double d = -2.75; d < 3.0 + 1e-9; d += 0.25) v.push_back(d);
    for (double d = 3.5; d < 10.0 + 1e-9; d += 0.5) v.push_back(d);
    return v;
}

const char* kSizeNames[]  = {"Small", "Medium", "Large"};
const char* kStyleNames[] = {"Default", "Dark box", "Yellow", "Outline"};

// One subtitle choice: a Plex stream id, or an mpv track id when the server
// told us nothing.
struct SubChoice {
    int id;
    std::string name;
    bool selected;
};

std::string plexSubName(const PlexStream& ps, size_t i) {
    if (!ps.displayTitle.empty()) return ps.displayTitle;
    std::string s = !ps.language.empty() ? ps.language : "Track " + std::to_string(i + 1);
    if (ps.forced) s += " (Forced)";
    if (ps.hearingImpaired) s += " [CC]";
    return s;
}

}  // namespace

// The subtitles the player can offer, in the order the server lists them.
static std::vector<SubChoice> subtitleChoices(const std::vector<PlexStream>& streams) {
    std::vector<SubChoice> out;
    size_t n = 0;
    for (const auto& ps : streams) {
        if (ps.streamType != 3) continue;
        out.push_back({ps.id, plexSubName(ps, n++), ps.selected});
    }
    if (!out.empty()) return out;
    for (const auto& t : MpvPlayer::getInstance().getTrackList("sub")) {
        std::string name = !t.lang.empty() ? t.lang : "Track " + std::to_string(t.id);
        if (!t.title.empty()) name += " - " + t.title;
        out.push_back({t.id, name, t.selected});
    }
    return out;
}

void PlayerActivity::showSubtitlePanel() {
    if (!playerContainer) return;
    fetchPlexStreams();

    if (!m_subPanel) {
        m_subPanel = new brls::Box(brls::Axis::COLUMN);
        m_subPanel->setPositionType(brls::PositionType::ABSOLUTE);
        m_subPanel->setPositionLeft(0);
        m_subPanel->setPositionRight(0);
        m_subPanel->setPositionBottom(0);
        m_subPanel->setPaddingTop(ui(40));
        m_subPanel->setPaddingBottom(ui(28));
        m_subPanel->setPaddingLeft(ui(40));
        m_subPanel->setPaddingRight(ui(40));
        // Clear at the top, darkening to the bottom edge: the picture stays
        // visible behind it, the words on it stay readable.
        m_subPanel->setBackground(brls::ViewBackground::VERTICAL_LINEAR);

        auto* heading = new brls::Label();
        heading->setText("Subtitles and captions");
        heading->setFontSize(ui(20));
        heading->setTextColor(vitaplex::palette::text);
        heading->setMarginBottom(ui(14));
        m_subPanel->addView(heading);

        auto* row = new brls::Box(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::FLEX_END);
        // On a phone the five controls are wider than the screen: what does
        // not fit (Timing, then Style) goes onto a second line instead of
        // off the right edge. borealis has no wrap setter; yoga's is used.
        YGNodeStyleSetFlexWrap(row->getYGNode(), YGWrapWrap);
        const char* captions[kSubControls] = {"Subtitles", "Language", "Size", "Style", "Timing"};
        for (int i = 0; i < kSubControls; i++) {
            auto* col = new brls::Box(brls::Axis::COLUMN);
            col->setMarginRight(i < kSubControls - 1 ? ui(22) : 0);
            col->setMarginTop(ui(10));   // between the lines when they wrap
            // Fixed widths, as on a TV: a lone On / Off (subtitles off) must
            // not stretch across the screen.
            static const float kColWidth[kSubControls] = {150, 280, 160, 160, 170};
            col->setWidth(ui(kColWidth[i]));
            col->setShrink(1.0f);

            auto* cap = new brls::Label();
            cap->setText(captions[i]);
            cap->setFontSize(ui(14));
            cap->setTextColor(vitaplex::palette::muted);
            cap->setMarginBottom(ui(6));
            col->addView(cap);
            if (i == SUB_LANGUAGE) m_subLangCaption = cap;

            auto* pill = new brls::Box(brls::Axis::ROW);
            pill->setAlignItems(brls::AlignItems::CENTER);
            pill->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
            pill->setHeight(ui(44));
            pill->setPaddingLeft(ui(16));
            pill->setPaddingRight(ui(14));
            pill->setCornerRadius(ui(10));
            pill->setHighlightCornerRadius(ui(10) + 2.5f);
            pill->setBackgroundColor(nvgRGBA(255, 255, 255, 46));
            pill->setFocusable(true);

            auto* value = new brls::Label();
            value->setFontSize(ui(16));
            value->setTextColor(vitaplex::palette::text);
            value->setSingleLine(true);
            value->setShrink(1.0f);
            pill->addView(value);

            // Up while closed, since the list opens above; down while it is open.
            auto* chevron = new brls::Image();
            chevron->setWidth(ui(22));
            chevron->setHeight(ui(22));
            chevron->setScalingType(brls::ImageScalingType::FIT);
            chevron->setMarginLeft(ui(8));
            setIconRes(chevron, "icons/chevron-up.png");
            pill->addView(chevron);
            m_subPillChevrons[i] = chevron;
            if (i == SUB_ONOFF) chevron->setVisibility(brls::Visibility::GONE);   // a switch

            pill->registerClickAction([this, i](brls::View*) {
                if (i == SUB_ONOFF) toggleSubtitlesFromPanel();   // a switch, no list
                else                openSubtitlePopup(i);
                return true;
            });
            pill->addGestureRecognizer(new brls::TapGestureRecognizer(pill));
            // The row is the whole panel: Up and Down stay in it.
            pill->setCustomNavigationRoute(brls::FocusDirection::UP, pill);
            pill->setCustomNavigationRoute(brls::FocusDirection::DOWN, pill);

            col->addView(pill);
            row->addView(col);
            m_subPills[i] = pill;
            m_subPillValues[i] = value;
        }
        m_subPanel->addView(row);

        m_subPanel->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View*) {
            hideSubtitlePanel();
            return true;
        });
        playerContainer->addView(m_subPanel);
    }

    refreshSubtitlePanel();
    hideControls();
    m_subPanelVisible = true;
    updateSubtitlePosition();
    m_subPanel->setVisibility(brls::Visibility::VISIBLE);
    brls::Application::giveFocus(subtitlePanelHome());
}

void PlayerActivity::refreshSubtitlePanel() {
    if (!m_subPanel) return;
    const auto choices = subtitleChoices(m_plexStreams);
    std::string current;
    for (const auto& c : choices)
        if (c.selected) {
            current = c.name;
            m_lastSubtitleStreamId = c.id;
        }
    const bool on = !current.empty();
    const AppSettings& s = Application::getInstance().getSettings();

    m_subPillValues[SUB_ONOFF]->setText(on ? "On" : "Off");
    m_subPillValues[SUB_LANGUAGE]->setText(
        on ? current : (choices.empty() ? "None available" : "Off"));
    m_subPillValues[SUB_SIZE]->setText(kSizeNames[std::clamp((int)s.subtitleSize, 0, 2)]);
    m_subPillValues[SUB_STYLE]->setText(kStyleNames[std::clamp(s.subtitleStyle, 0, 3)]);
    m_subPillValues[SUB_TIMING]->setText(timingName(m_subUserDelay));
    if (m_subSearchLang.empty()) m_subSearchLang = s.defaultSubtitleLanguage.empty() ? "en" : s.defaultSubtitleLanguage;
    if (m_subLangCaption)
        m_subLangCaption->setText("Language (" + std::to_string(choices.size()) + ")");

    // Off: the language, size and style mean nothing, so only On / Off shows.
    for (int i = SUB_LANGUAGE; i <= SUB_TIMING; i++) {
        brls::View* col = m_subPills[i] ? m_subPills[i]->getParent() : nullptr;
        if (col) col->setVisibility(on ? brls::Visibility::VISIBLE : brls::Visibility::GONE);
    }
}

// Where focus rests in the panel: the language when subtitles are on, the
// on / off switch when they are off (the only one showing then).
brls::View* PlayerActivity::subtitlePanelHome() {
    brls::View* lang = m_subPills[SUB_LANGUAGE];
    if (lang && lang->getParent() &&
        lang->getParent()->getVisibility() == brls::Visibility::VISIBLE)
        return lang;
    return m_subPills[SUB_ONOFF];
}

// The On / Off switch: off, or back on to the track last shown (the first
// when there was none).
void PlayerActivity::toggleSubtitlesFromPanel() {
    const auto choices = subtitleChoices(m_plexStreams);
    bool anyOn = false;
    for (const auto& c : choices) anyOn = anyOn || c.selected;
    if (anyOn) {
        brls::Logger::info("Subtitle panel: off");
        selectTrack(TrackSelectMode::SUBTITLE, -1);
    } else if (choices.empty()) {
        // Nothing to turn on: go and find some.
        if (m_mediaKey.empty()) {
            MpvPlayer::getInstance().showOSD("This video has no subtitles", 2.0);
            return;
        }
        brls::Logger::info("Subtitle panel: none to turn on, searching");
        brls::sync([this]() {
            if (m_subPanelVisible) openSubtitlePopup(SUB_SEARCH);
        });
        return;
    } else {
        int target = m_lastSubtitleStreamId;
        bool known = false;
        for (const auto& c : choices) known = known || c.id == target;
        if (!known) target = choices.front().id;
        brls::Logger::info("Subtitle panel: on, track {}", target);
        selectTrack(TrackSelectMode::SUBTITLE, target);
    }
    // selectTrack moves focus to the OSD's buttons; the panel is still up.
    brls::sync([this]() {
        if (!m_subPanelVisible) return;
        refreshSubtitlePanel();
        if (brls::View* home = subtitlePanelHome()) brls::Application::giveFocus(home);
    });
}

void PlayerActivity::hideSubtitlePanel(bool restoreControls) {
    closeSubtitlePopup();
    if (!m_subPanel || !m_subPanelVisible) return;
    m_subPanelVisible = false;
    updateSubtitlePosition();
    m_subPanel->setVisibility(brls::Visibility::GONE);
    if (!restoreControls) return;
    showControls();
    if (brls::View* pill = getView("player/subs_pill"))
        if (pill->getVisibility() == brls::Visibility::VISIBLE) brls::Application::giveFocus(pill);
}

void PlayerActivity::closeSubtitlePopup() {
    if (!m_subPopup) return;
    const int which = m_subPopupWhich;
    m_subPopupWhich = -1;
    // Size / Style previewed but not picked: back to the saved one.
    if ((which == SUB_SIZE || which == SUB_STYLE) && !m_subPreviewKept) {
        AppSettings& st = Application::getInstance().getSettings();
        if (which == SUB_SIZE && (int)st.subtitleSize != m_subPreviewSize) {
            st.subtitleSize = static_cast<SubtitleSize>(m_subPreviewSize);
            MpvPlayer::getInstance().applySubtitleSize();
        }
        if (which == SUB_STYLE && st.subtitleStyle != m_subPreviewStyle) {
            st.subtitleStyle = m_subPreviewStyle;
            MpvPlayer::getInstance().applySubtitleStyle();
        }
    }
    if (which == SUB_TIMING && !m_subPreviewKept) {
        m_subUserDelay = m_subPreviewDelay;
        applySubtitleDelay();
    }
    if (which >= 0 && which < kSubControls && m_subPillChevrons[which])
        setIconRes(m_subPillChevrons[which], "icons/chevron-up.png");
    // Off the focused row before the rows go.
    if (which == SUB_AUDIO) {
        if (audioBtn && m_controlsVisible) brls::Application::giveFocus(audioBtn);
    } else if (which >= 0 && which < kSubControls && m_subPills[which])
        brls::Application::giveFocus(m_subPills[which]);
    else if (brls::View* home = subtitlePanelHome())
        brls::Application::giveFocus(home);   // the search list
    if (playerContainer) playerContainer->removeView(m_subPopup, true);
    m_subPopup = nullptr;
}

void PlayerActivity::openSubtitlePopup(int which) {
    if (which < 0 || which > SUB_AUDIO || !playerContainer) return;
    // The search list sits over the language drop-down, or the switch when
    // that is all that shows; the audio list over the Audio pill.
    brls::Box* anchor = which == SUB_AUDIO    ? audioBtn
                      : which == SUB_SEARCH ? dynamic_cast<brls::Box*>(subtitlePanelHome())
                                            : m_subPills[which];
    if (!anchor) return;
    if (m_subPopupWhich == which) {
        closeSubtitlePopup();
        return;
    }
    closeSubtitlePopup();

    // The rows: a name, whether it is the current one, and what picking it does.
    struct Row {
        std::string name;
        bool current;
        std::function<void()> pick;   // empty: a line of information, nothing to pick
        std::function<void()> preview = nullptr;   // as the row is reached (Size, Style)
    };
    std::vector<Row> rows;
    std::string heading;
    m_subPreviewKept = false;
    m_subPreviewSize = (int)Application::getInstance().getSettings().subtitleSize;
    m_subPreviewStyle = Application::getInstance().getSettings().subtitleStyle;
    m_subPreviewDelay = m_subUserDelay;
    AppSettings& settings = Application::getInstance().getSettings();
    const auto choices = subtitleChoices(m_plexStreams);
    bool anyOn = false;
    for (const auto& c : choices) anyOn = anyOn || c.selected;

    switch (which) {
        case SUB_ONOFF: {
            heading = "Subtitles";
            int target = m_lastSubtitleStreamId;
            if (target < 0 && !choices.empty()) target = choices.front().id;
            rows.push_back({"On", anyOn, [this, target, anyOn]() {
                                if (!anyOn && target >= 0) selectTrack(TrackSelectMode::SUBTITLE, target);
                            }});
            rows.push_back({"Off", !anyOn, [this, anyOn]() {
                                if (anyOn) selectTrack(TrackSelectMode::SUBTITLE, -1);
                            }});
            break;
        }
        case SUB_LANGUAGE:
            heading = "Languages (" + std::to_string(choices.size()) + ")";
            for (const auto& c : choices) {
                const int id = c.id;
                const bool sel = c.selected;
                rows.push_back({c.name, sel, [this, id, sel]() {
                                    if (sel) return;
                                    m_subUserDelay = 0.0;   // another file, its own timing
                                    selectTrack(TrackSelectMode::SUBTITLE, id);
                                }});
            }
            // One more: find others online. Opened the frame after this list
            // closes, in its place.
            if (!m_mediaKey.empty()) {
                rows.push_back({"Search online...", false, [this]() {
                                    brls::sync([this]() {
                                        if (m_subPanelVisible) openSubtitlePopup(SUB_SEARCH);
                                    });
                                }});
            }
            break;
        case SUB_SEARCH: {
            heading = "Search online";
            if (m_subSearchState == 0) startSubtitleSearch();
            rows.push_back({"Language: " + m_subSearchLang + " (change)", false,
                            [this]() { promptSubtitleSearchLanguage(); }});
            if (m_subSearchState == 1) {
                rows.push_back({"Searching " + m_subSearchLang + "...", false, nullptr});
            } else if (m_subSearchResults.empty()) {
                rows.push_back({"No subtitles found", false, nullptr});
            } else {
                for (const auto& r : m_subSearchResults) {
                    std::string name = r.displayTitle.empty() ? r.language : r.displayTitle;
                    if (r.hearingImpaired) name += " (SDH)";
                    if (r.forced) name += " (Forced)";
                    if (!r.provider.empty()) name += "  -  " + r.provider;
                    // The release it was made for: one that matches the
                    // video's own file is the one in time.
                    if (!r.release.empty() && r.release != r.displayTitle) name += "  -  " + r.release;
                    const std::string key = r.key;
                    rows.push_back({name, false, [this, key, name]() { installSearchedSubtitle(key, name); }});
                }
            }
            break;
        }
        case SUB_AUDIO: {
            // The server's list (a transcode carries only the chosen track,
            // so mpv sees one); mpv's when the server gave none.
            int n = 0;
            for (const auto& ps : m_plexStreams) {
                if (ps.streamType != 2) continue;
                std::string name = ps.displayTitle;
                if (name.empty()) {
                    name = !ps.language.empty() ? ps.language : "Track " + std::to_string(n + 1);
                    if (!ps.codec.empty()) name += " [" + ps.codec + "]";
                }
                n++;
                const int id = ps.id;
                const bool sel = ps.selected;
                rows.push_back({name, sel, [this, id, sel]() {
                                    if (!sel) selectTrack(TrackSelectMode::AUDIO, id);
                                }});
            }
            if (rows.empty()) {
                for (const auto& t : MpvPlayer::getInstance().getTrackList("audio")) {
                    std::string name = !t.lang.empty() ? t.lang : "Track " + std::to_string(t.id);
                    if (!t.title.empty()) name += " - " + t.title;
                    if (!t.codec.empty()) name += " [" + t.codec + "]";
                    const int id = t.id;
                    const bool sel = t.selected;
                    rows.push_back({name, sel, [this, id, sel]() {
                                        if (!sel) selectTrack(TrackSelectMode::AUDIO, id);
                                    }});
                }
            }
            heading = "Audio (" + std::to_string(rows.size()) + ")";
            if (rows.empty()) rows.push_back({"No other audio tracks", false, nullptr});
            break;
        }
        case SUB_SIZE:
            heading = "Sizes";
            for (int i = 0; i < 3; i++) {
                rows.push_back({kSizeNames[i], (int)settings.subtitleSize == i,
                                [this, i]() {
                                    Application& app = Application::getInstance();
                                    app.getSettings().subtitleSize = static_cast<SubtitleSize>(i);
                                    app.saveSettings();
                                    m_subPreviewKept = true;
                                    MpvPlayer::getInstance().applySubtitleSize();
                                },
                                [i]() {
                                    Application::getInstance().getSettings().subtitleSize =
                                        static_cast<SubtitleSize>(i);
                                    MpvPlayer::getInstance().applySubtitleSize();
                                }});
            }
            break;
        case SUB_TIMING: {
            heading = "Move subtitles";
            // The row nearest the current shift is the current one.
            double nearest = 0.0, best = 1e9;
            const auto steps = timingSteps();
            for (double d : steps)
                if (std::fabs(d - m_subUserDelay) < best) { best = std::fabs(d - m_subUserDelay); nearest = d; }
            for (double d : steps) {
                rows.push_back({timingName(d), std::fabs(d - nearest) < 1e-6,
                                [this, d]() {
                                    m_subUserDelay = d;
                                    m_subPreviewKept = true;
                                    applySubtitleDelay();
                                    brls::Logger::info("Subtitle panel: timing {:+.2f} s", d);
                                },
                                [this, d]() {
                                    m_subUserDelay = d;
                                    applySubtitleDelay();
                                }});
            }
            break;
        }
        case SUB_STYLE:
            heading = "Styles";
            for (int i = 0; i < 4; i++) {
                rows.push_back({kStyleNames[i], settings.subtitleStyle == i,
                                [this, i]() {
                                    Application& app = Application::getInstance();
                                    app.getSettings().subtitleStyle = i;
                                    app.saveSettings();
                                    m_subPreviewKept = true;
                                    MpvPlayer::getInstance().applySubtitleStyle();
                                },
                                [i]() {
                                    Application::getInstance().getSettings().subtitleStyle = i;
                                    MpvPlayer::getInstance().applySubtitleStyle();
                                }});
            }
            break;
    }

    // Above the pill it belongs to, left edges lined up.
    brls::Box* pill = anchor;
    const float containerH = playerContainer->getHeight();
    const float pillTop = pill->getY() - playerContainer->getY();
    const float left = pill->getX() - playerContainer->getX();
    const float rowH = ui(46);
    const float maxListH = std::max(rowH * 2, std::min(pillTop - ui(70), containerH * 0.55f));
    const float listH = std::min(maxListH, rowH * (float)std::max<size_t>(rows.size(), 1));
    const float width = std::max(pill->getWidth(), ui(which == SUB_SEARCH ? 460 : 260));

    m_subPopup = new brls::Box(brls::Axis::COLUMN);
    m_subPopup->setPositionType(brls::PositionType::ABSOLUTE);
    const float popupLeft = std::max(0.0f, std::min(left, playerContainer->getWidth() - width - ui(16)));
    m_subPopup->setPositionLeft(popupLeft);
    m_subPopup->setPositionBottom(containerH - pillTop + ui(10));
    m_subPopup->setWidth(width);
    m_subPopup->setCornerRadius(ui(14));
    m_subPopup->setBackgroundColor(nvgRGBA(44, 46, 54, 240));
    m_subPopup->setPadding(ui(8));

    auto* title = new brls::Label();
    title->setText(heading);
    title->setFontSize(ui(15));
    title->setTextColor(vitaplex::palette::muted);
    title->setMargins(ui(6), ui(10), ui(8), ui(10));
    m_subPopup->addView(title);

    auto* scroll = new brls::ScrollingFrame();
    scroll->setHeight(listH);
    scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
    auto* list = new brls::Box(brls::Axis::COLUMN);
    scroll->setContentView(list);

    brls::View* focusRow = nullptr;
    for (auto& r : rows) {
        auto* item = new brls::Box(brls::Axis::ROW);
        item->setAlignItems(brls::AlignItems::CENTER);
        item->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        item->setHeight(rowH - ui(4));
        item->setMarginBottom(ui(4));
        item->setPaddingLeft(ui(14));
        item->setPaddingRight(ui(14));
        item->setCornerRadius(ui(8));
        item->setHighlightCornerRadius(ui(8) + 2.5f);
        item->setFocusable(true);
        if (r.current) item->setBackgroundColor(vitaplex::palette::gold);

        auto* label = new brls::Label();
        label->setText(r.name);
        label->setFontSize(ui(16));
        label->setSingleLine(true);
        label->setShrink(1.0f);
        label->setTextColor(r.current ? vitaplex::palette::goldInk : vitaplex::palette::text);
        item->addView(label);

        if (r.current) {
            auto* mark = new brls::Label();
            mark->setText("(on)");
            mark->setFontSize(ui(13));
            mark->setTextColor(vitaplex::palette::goldInk);
            mark->setMarginLeft(ui(8));
            item->addView(mark);
        }

        // Size and Style show on the picture as the row is reached; leaving
        // the list without picking puts back what was there.
        if (r.preview) {
            auto preview = r.preview;
            item->getFocusEvent()->subscribe([preview](brls::View*) { preview(); });
        }

        auto pick = r.pick;
        item->registerClickAction([this, pick](brls::View*) {
            if (!pick) return true;
            // After this frame: the pick can close the list this row is in.
            const int which = m_subPopupWhich;
            brls::sync([this, pick, which]() {
                pick();
                if (which == SUB_AUDIO) {
                    // selectTrack puts focus on the OSD; the list just closes.
                    closeSubtitlePopup();
                    return;
                }
                if (!m_subPanelVisible) return;   // it handed over to another screen
                closeSubtitlePopup();
                refreshSubtitlePanel();
                // selectTrack ends by putting focus back on the OSD's buttons;
                // the panel is still up, so back to its own row.
                if (brls::View* home = subtitlePanelHome()) brls::Application::giveFocus(home);
            });
            return true;
        });
        item->addGestureRecognizer(new brls::TapGestureRecognizer(item));
        item->setCustomNavigationRoute(brls::FocusDirection::LEFT, item);
        item->setCustomNavigationRoute(brls::FocusDirection::RIGHT, item);
        list->addView(item);
        if (r.current || !focusRow) focusRow = item;   // the current one, else the first
    }
    m_subPopup->addView(scroll);

    m_subPopup->registerAction("Back", brls::ControllerButton::BUTTON_B, [this](brls::View*) {
        closeSubtitlePopup();
        return true;
    });

    playerContainer->addView(m_subPopup);
    m_subPopupWhich = which;
    brls::Logger::info("Subtitle panel: list {} open, {} rows, at left {} bottom {} ({}x{} over {}x{})",
                       which, rows.size(), popupLeft,
                       containerH - pillTop + ui(10), width, listH,
                       playerContainer->getWidth(), containerH);
    if (which < kSubControls && m_subPillChevrons[which])
        setIconRes(m_subPillChevrons[which], "icons/chevron-down.png");
    if (focusRow) brls::Application::giveFocus(focusRow);
}

// Ask the server for subtitles in m_subSearchLang. The answer reopens the
// search list if it is still the one open.
void PlayerActivity::startSubtitleSearch() {
    if (m_mediaKey.empty()) return;
    m_subSearchState = 1;
    m_subSearchResults.clear();
    const unsigned gen = ++m_subSearchGen;
    const std::string key = m_mediaKey, lang = m_subSearchLang;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
    brls::Logger::info("Subtitle panel: searching {} subtitles", lang);
    asyncRun([this, aliveWeak, gen, key, lang]() {
        std::vector<PlexClient::SubtitleResult> results;
        PlexClient::getInstance().searchSubtitles(key, lang, results);
        brls::sync([this, aliveWeak, gen, results]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load() || gen != m_subSearchGen) return;
            m_subSearchResults = results;
            m_subSearchState = 2;
            brls::Logger::info("Subtitle panel: {} subtitle(s) found", results.size());
            if (m_subPopupWhich == SUB_SEARCH) {
                closeSubtitlePopup();
                openSubtitlePopup(SUB_SEARCH);
            }
        });
    });
}

void PlayerActivity::promptSubtitleSearchLanguage() {
    auto* ime = brls::Application::getImeManager();
    if (!ime) return;
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
    ime->openForText(
        [this, aliveWeak](std::string lang) {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load() || lang.empty()) return;
            m_subSearchLang = lang;
            refreshSubtitlePanel();
            startSubtitleSearch();
            if (m_subPanelVisible) {
                closeSubtitlePopup();
                openSubtitlePopup(SUB_SEARCH);
            }
        },
        "Subtitle language (e.g. en, es, fr)", m_subSearchLang, 8, m_subSearchLang);
}

// Have the server add the found subtitle to the item, then play with it:
// it becomes one of the item's subtitles, selected, and the stream restarts
// with it where the video is.
void PlayerActivity::installSearchedSubtitle(const std::string& subKey, const std::string& name) {
    MpvPlayer::getInstance().showOSD("Adding subtitle...", 4.0);
    m_subUserDelay = 0.0;   // another file, its own timing
    const std::string mediaKey = m_mediaKey;
    const int partId = m_partId;
    std::vector<int> known;
    for (const auto& ps : m_plexStreams)
        if (ps.streamType == 3) known.push_back(ps.id);
    std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
    asyncRun([this, aliveWeak, mediaKey, partId, subKey, name, known]() {
        const bool ok = PlexClient::getInstance().selectSearchedSubtitle(mediaKey, partId, subKey);
        std::vector<PlexStream> fresh;
        int freshPart = partId;
        // The server answers at once but fetches the file afterwards: the
        // item lists the new subtitle only some time later (not yet a quarter
        // of a second on, in a log). Ask again until a subtitle it did not
        // have shows up, for up to 15 s.
        for (int attempt = 0; ok && attempt < 15; attempt++) {
            if (attempt > 0) std::this_thread::sleep_for(std::chrono::seconds(1));
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;
            fresh.clear();
            freshPart = partId;
            PlexClient::getInstance().fetchStreams(mediaKey, fresh, freshPart);
            bool added = false;
            for (const auto& ps : fresh)
                if (ps.streamType == 3 &&
                    std::find(known.begin(), known.end(), ps.id) == known.end())
                    added = true;
            if (added) break;
        }
        brls::sync([this, aliveWeak, ok, fresh, freshPart, name, known]() {
            auto alive = aliveWeak.lock();
            if (!alive || !alive->load()) return;
            if (!ok || fresh.empty()) {
                MpvPlayer::getInstance().showOSD("Couldn't add the subtitle", 2.5);
                return;
            }
            m_plexStreams = fresh;
            m_partId = freshPart;
            m_streamsLoaded = true;
            // The one that was not there before, else the one the server has
            // selected, else the last.
            int id = -1;
            for (const auto& ps : m_plexStreams)
                if (ps.streamType == 3 && std::find(known.begin(), known.end(), ps.id) == known.end())
                    id = ps.id;
            if (id < 0)
                for (const auto& ps : m_plexStreams)
                    if (ps.streamType == 3 && ps.selected) id = ps.id;
            if (id < 0)
                for (const auto& ps : m_plexStreams)
                    if (ps.streamType == 3) id = ps.id;
            if (id < 0) {
                MpvPlayer::getInstance().showOSD("The server has not added the subtitle yet", 3.0);
                brls::Logger::warning("Subtitle panel: {} not listed by the server after 15 s", name);
                return;
            }
            brls::Logger::info("Subtitle panel: added {} as stream {}", name, id);
            selectTrack(TrackSelectMode::SUBTITLE, id);
            if (!m_subPanelVisible) return;
            closeSubtitlePopup();
            refreshSubtitlePanel();
            if (brls::View* home = subtitlePanelHome()) brls::Application::giveFocus(home);
        });
    });
}

// The OSD's Audio pill: straight to the list of audio tracks.
void PlayerActivity::showAudioTrackPopup() {
    fetchPlexStreams();
    openSubtitlePopup(SUB_AUDIO);
}

bool PlayerActivity::subtitlePanelHit(float x, float y) const {
    auto inside = [x, y](brls::View* v) {
        return v && v->getVisibility() == brls::Visibility::VISIBLE && x >= v->getX() &&
               x <= v->getX() + v->getWidth() && y >= v->getY() && y <= v->getY() + v->getHeight();
    };
    return inside(m_subPopup) || inside(m_subPanel);
}

}  // namespace vitaplex
