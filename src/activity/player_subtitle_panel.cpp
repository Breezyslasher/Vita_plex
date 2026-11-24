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

#include <borealis.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace vitaplex {

namespace {

enum Which { SUB_ONOFF = 0, SUB_LANGUAGE = 1, SUB_SIZE = 2, SUB_STYLE = 3 };

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
        const char* captions[4] = {"Subtitles", "Language", "Size", "Style"};
        for (int i = 0; i < 4; i++) {
            auto* col = new brls::Box(brls::Axis::COLUMN);
            col->setMarginRight(i < 3 ? ui(22) : 0);
            col->setGrow(i == SUB_LANGUAGE ? 1.4f : 1.0f);
            col->setShrink(1.0f);
            col->setWidth(ui(10));   // a base; grow shares the row out

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

            auto* chevron = new brls::Label();
            chevron->setText("v");
            chevron->setFontSize(ui(13));
            chevron->setTextColor(vitaplex::palette::muted);
            chevron->setMarginLeft(ui(8));
            pill->addView(chevron);

            pill->registerClickAction([this, i](brls::View*) {
                openSubtitlePopup(i);
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
    m_subPanel->setVisibility(brls::Visibility::VISIBLE);
    brls::Application::giveFocus(m_subPills[SUB_LANGUAGE]);
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
    if (m_subLangCaption)
        m_subLangCaption->setText("Language (" + std::to_string(choices.size()) + ")");
}

void PlayerActivity::hideSubtitlePanel(bool restoreControls) {
    closeSubtitlePopup();
    if (!m_subPanel || !m_subPanelVisible) return;
    m_subPanelVisible = false;
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
    // Off the focused row before the rows go.
    if (which >= 0 && which < 4 && m_subPills[which])
        brls::Application::giveFocus(m_subPills[which]);
    if (playerContainer) playerContainer->removeView(m_subPopup, true);
    m_subPopup = nullptr;
}

void PlayerActivity::openSubtitlePopup(int which) {
    if (which < 0 || which > 3 || !m_subPills[which] || !playerContainer) return;
    if (m_subPopupWhich == which) {
        closeSubtitlePopup();
        return;
    }
    closeSubtitlePopup();

    // The rows: a name, whether it is the current one, and what picking it does.
    struct Row {
        std::string name;
        bool current;
        std::function<void()> pick;
    };
    std::vector<Row> rows;
    std::string heading;
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
                                    if (!sel) selectTrack(TrackSelectMode::SUBTITLE, id);
                                }});
            }
            // What the old track list offered besides the tracks.
            if (!m_mediaKey.empty()) {
                rows.push_back({"Search online...", false, [this]() {
                                    hideSubtitlePanel(false);
                                    showTrackOverlay(TrackSelectMode::SUBTITLE);
                                    brls::sync([this]() { populateSubtitleSearchResults(); });
                                }});
            }
            break;
        case SUB_SIZE:
            heading = "Sizes";
            for (int i = 0; i < 3; i++) {
                rows.push_back({kSizeNames[i], (int)settings.subtitleSize == i, [i]() {
                                    Application& app = Application::getInstance();
                                    app.getSettings().subtitleSize = static_cast<SubtitleSize>(i);
                                    app.saveSettings();
                                    MpvPlayer::getInstance().applySubtitleSize();
                                }});
            }
            break;
        case SUB_STYLE:
            heading = "Styles";
            for (int i = 0; i < 4; i++) {
                rows.push_back({kStyleNames[i], settings.subtitleStyle == i, [i]() {
                                    Application& app = Application::getInstance();
                                    app.getSettings().subtitleStyle = i;
                                    app.saveSettings();
                                    MpvPlayer::getInstance().applySubtitleStyle();
                                }});
            }
            break;
    }

    // Above the pill it belongs to, left edges lined up.
    brls::Box* pill = m_subPills[which];
    const float containerH = playerContainer->getHeight();
    const float pillTop = pill->getY() - playerContainer->getY();
    const float left = pill->getX() - playerContainer->getX();
    const float rowH = ui(46);
    const float maxListH = std::max(rowH * 2, std::min(pillTop - ui(70), containerH * 0.55f));
    const float listH = std::min(maxListH, rowH * (float)std::max<size_t>(rows.size(), 1));
    const float width = std::max(pill->getWidth(), ui(260));

    m_subPopup = new brls::Box(brls::Axis::COLUMN);
    m_subPopup->setPositionType(brls::PositionType::ABSOLUTE);
    m_subPopup->setPositionLeft(std::min(left, playerContainer->getWidth() - width - ui(16)));
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

        auto pick = r.pick;
        item->registerClickAction([this, pick](brls::View*) {
            // After this frame: the pick can close the list this row is in.
            brls::sync([this, pick]() {
                pick();
                if (!m_subPanelVisible) return;   // it handed over to another screen
                closeSubtitlePopup();
                refreshSubtitlePanel();
                // selectTrack ends by putting focus back on the OSD's buttons;
                // the panel is still up, so back to its own row.
                if (m_subPills[SUB_LANGUAGE]) brls::Application::giveFocus(m_subPills[SUB_LANGUAGE]);
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
    if (focusRow) brls::Application::giveFocus(focusRow);
}

bool PlayerActivity::subtitlePanelHit(float x, float y) const {
    auto inside = [x, y](brls::View* v) {
        return v && v->getVisibility() == brls::Visibility::VISIBLE && x >= v->getX() &&
               x <= v->getX() + v->getWidth() && y >= v->getY() && y <= v->getY() + v->getHeight();
    };
    return inside(m_subPopup) || inside(m_subPanel);
}

}  // namespace vitaplex
