/**
 * VitaPlex - Sidebar Editor implementation
 *
 * A seamless in-place editor: a panel sized + coloured to match the live
 * sidebar (so it reads as the sidebar in edit mode, no dim) where the user
 * reorders and shows/hides items. Reorder works on every platform — D-pad /
 * arrow keys (grab with A, move with Up/Down) and touch (drag a row). Commits
 * the order + hidden set to settings.cfg and rebuilds the live sidebar.
 */

#include "view/sidebar_editor.hpp"
#include "activity/main_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"
#include "app/plex_palette.hpp"
#include "app/hint_icons.hpp"

#include <borealis.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <string>
#include <vector>

namespace vitaplex {

namespace {

namespace pal = vitaplex::palette;

std::vector<std::string> splitCsv(const std::string& s) {
    std::vector<std::string> out;
    std::string cur = s;
    size_t pos;
    while ((pos = cur.find(',')) != std::string::npos) {
        std::string t = cur.substr(0, pos);
        if (!t.empty()) out.push_back(t);
        cur.erase(0, pos + 1);
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
bool csvHas(const std::string& csv, const std::string& id) {
    for (const auto& t : splitCsv(csv)) if (t == id) return true;
    return false;
}
std::string joinCsv(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& t : v) { if (!s.empty()) s += ","; s += t; }
    return s;
}

struct EditItem {
    std::string id;        // home, search, livetv, downloads, settings, lib:<key>
    std::string title;
    bool fixed = false;    // home, settings — can't move or hide
    bool hidden = false;
    bool isLibrary = false;
};

// Translucent so the live app (and its real sidebar) renders behind; the panel
// covers the sidebar region exactly, so it looks like the sidebar in edit mode.
class SidebarEditorActivity : public brls::Activity {
public:
    bool isTranslucent() override { return true; }

    // HintIcons::onSourceChanged subscriptions live for the whole process, so
    // guard their callbacks against this editor having been destroyed.
    ~SidebarEditorActivity() override { m_alive->store(false); }

    brls::View* createContentView() override {
        buildModel();

        // Match the live sidebar's metrics so the panel reads as the sidebar.
        brls::Style style = brls::Application::getStyle();
        m_rowH    = style["brls/sidebar/item_height"];
        m_fontSz  = style["brls/sidebar/item_font_size"];
        m_padSide = style["brls/sidebar/item_accent_margin_sides"];
        m_accentW = style["brls/sidebar/item_accent_rect_width"];
        if (m_rowH < 24.0f)   m_rowH = 70.0f;
        if (m_fontSz < 8.0f)  m_fontSz = 22.0f;
        if (m_padSide < 2.0f) m_padSide = 16.0f;
        if (m_accentW < 1.0f) m_accentW = 4.0f;

        float w = 240.0f;
        if (auto* m = MainActivity::getInstance()) {
            float gw = m->getSidebarWidth();
            if (gw > 80.0f) w = gw;
        }

        auto* root = new brls::Box();
        root->setAxis(brls::Axis::ROW);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);

        // ---- Panel (matches the sidebar) ----
        auto* panel = new brls::Box();
        panel->setAxis(brls::Axis::COLUMN);
        panel->setWidth(w);
        panel->setHeightPercentage(100.0f);
        panel->setBackgroundColor(pal::panel);

        // Slim header: eyebrow + Done (Done also commits on tap, for touch)
        auto* head = new brls::Box();
        head->setAxis(brls::Axis::ROW);
        head->setAlignItems(brls::AlignItems::CENTER);
        head->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        head->setPaddingLeft(m_padSide);
        head->setPaddingRight(m_padSide);
        head->setPaddingTop(14.0f);
        head->setPaddingBottom(10.0f);
        auto* eyebrow = new brls::Label();
        eyebrow->setText("EDIT SIDEBAR");
        eyebrow->setFontSize(11.0f);
        eyebrow->setTextColor(pal::muted);
        head->addView(eyebrow);
        panel->addView(head);

        auto* hr = new brls::Box();
        hr->setHeight(1.0f);
        hr->setMarginLeft(m_padSide);
        hr->setMarginRight(m_padSide);
        hr->setMarginBottom(4.0f);
        hr->setBackgroundColor(pal::line);
        panel->addView(hr);

        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1.0f);
        // CENTERED so D-pad / arrow focus changes scroll the focused row into
        // view (the default NATURAL behavior only scrolls on touch).
        scroll->setScrollingBehavior(brls::ScrollingBehavior::CENTERED);
        m_listBox = new brls::Box();
        m_listBox->setAxis(brls::Axis::COLUMN);
        scroll->setContentView(m_listBox);
        panel->addView(scroll);

        // Footer hints (controller / keyboard)
        auto* foot = new brls::Box();
        foot->setAxis(brls::Axis::ROW);
        foot->setAlignItems(brls::AlignItems::CENTER);
        foot->setPaddingLeft(m_padSide);
        foot->setPaddingRight(m_padSide);
        foot->setPaddingTop(10.0f);
        foot->setPaddingBottom(8.0f);
        auto* footHr = new brls::Box();
        footHr->setHeight(1.0f);
        footHr->setMarginLeft(m_padSide);
        footHr->setMarginRight(m_padSide);
        footHr->setBackgroundColor(pal::line);
        panel->addView(footHr);
        addHint(foot, brls::BUTTON_A, "Grab");
        addHint(foot, brls::BUTTON_X, "Hide");
        addHint(foot, brls::BUTTON_B, "Done");
        panel->addView(foot);

        root->addView(panel);

        // Transparent dismiss area over the live content — tap commits.
        auto* rest = new brls::Box();
        rest->setGrow(1.0f);
        rest->setHeightPercentage(100.0f);
        rest->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus s, brls::Sound*) {
                if (s.state == brls::GestureState::END) commit();
            }));
        root->addView(rest);

        // Input: D-pad / arrows move focus (and the grabbed row); A grabs/drops;
        // X hides/shows; B commits. Hidden hints — the footer carries them.
        root->registerAction("", brls::ControllerButton::BUTTON_NAV_UP,
            [this](brls::View*) { moveFocus(-1); return true; }, true, true);
        root->registerAction("", brls::ControllerButton::BUTTON_NAV_DOWN,
            [this](brls::View*) { moveFocus(1); return true; }, true, true);
        root->registerAction("", brls::ControllerButton::BUTTON_A,
            [this](brls::View*) { toggleGrab(); return true; }, true);
        root->registerAction("", brls::ControllerButton::BUTTON_X,
            [this](brls::View*) { toggleHide(); return true; }, true);
        root->registerAction("", brls::ControllerButton::BUTTON_B,
            [this](brls::View*) { commit(); return true; }, true);

        renderRows();
        return root;
    }

    void onContentAvailable() override {
        m_attached = true;
        if (!m_rows.empty()) {
            int f = std::min(std::max(m_focus, 0), (int)m_rows.size() - 1);
            brls::Application::giveFocus(m_rows[f]);
        }
    }

private:
    // A platform/input-aware button hint (glyph + label), matching the app's
    // media-cell hints: the glyph PNG comes from HintIcons (PlayStation shapes
    // on Vita/PS4, keyboard keys on desktop, touch art on Android, etc.) and
    // refreshes live when the input source flips.
    void addHint(brls::Box* parent, brls::ControllerButton button, const std::string& label) {
        auto* icon = new brls::Image();
        icon->setWidth(18.0f);
        icon->setHeight(18.0f);
        icon->setScalingType(brls::ImageScalingType::FIT);
        icon->setMarginRight(5.0f);
        std::string path = HintIcons::getResPath(button);
        if (!path.empty())
            icon->setImageFromRes(path);
        else
            icon->setVisibility(brls::Visibility::GONE);  // e.g. touch: no glyph
        std::weak_ptr<std::atomic<bool>> aliveWeak = m_alive;
        HintIcons::onSourceChanged([aliveWeak, icon, button]() {
            auto a = aliveWeak.lock();
            if (!a || !a->load()) return;
            std::string p = HintIcons::getResPath(button);
            if (!p.empty()) {
                icon->setImageFromRes(p);
                icon->setVisibility(brls::Visibility::VISIBLE);
            } else {
                icon->setVisibility(brls::Visibility::GONE);
            }
        });
        parent->addView(icon);

        auto* l = new brls::Label();
        l->setText(label);
        l->setFontSize(11.0f);
        l->setTextColor(pal::muted);
        l->setMarginRight(14.0f);
        parent->addView(l);
    }

    void buildModel() {
        m_items.clear();
        AppSettings& s = Application::getInstance().getSettings();
        bool hasLiveTV = PlexClient::getInstance().hasLiveTV();

        m_items.push_back({ "home", "Home", true, false, false });

        // Movable universe (default order) + titles: each library, then built-ins.
        std::vector<std::pair<std::string, std::string>> universe;
        std::vector<LibrarySection> sections;
        m_libsFetched = PlexClient::getInstance().fetchLibrarySections(sections);
        for (const auto& sec : sections)
            universe.push_back({ "lib:" + sec.key, sec.title });
        universe.push_back({ "search", "Search" });
        if (hasLiveTV) universe.push_back({ "livetv", "Live TV" });
        universe.push_back({ "downloads", "Downloads" });

        auto titleOf = [&](const std::string& id) -> std::string {
            for (const auto& u : universe) if (u.first == id) return u.second;
            return id;
        };
        auto inUniverse = [&](const std::string& id) {
            for (const auto& u : universe) if (u.first == id) return true;
            return false;
        };

        std::vector<std::string> order;
        auto placed = [&](const std::string& id) {
            return std::find(order.begin(), order.end(), id) != order.end();
        };
        for (const auto& id : splitCsv(s.sidebarOrder))
            if (inUniverse(id) && !placed(id)) order.push_back(id);
        for (const auto& u : universe)
            if (!placed(u.first)) order.push_back(u.first);

        for (const auto& id : order) {
            EditItem it;
            it.id = id;
            it.title = titleOf(id);
            if (id.rfind("lib:", 0) == 0) {
                it.isLibrary = true;
                it.hidden = csvHas(s.hiddenLibraries, id.substr(4));
            } else {
                it.hidden = csvHas(s.hiddenSidebarItems, id);
            }
            m_items.push_back(it);
        }

        m_items.push_back({ "settings", "Settings", true, false, false });

        m_focus = (m_items.size() > 2) ? 1 : 0;  // first movable
        m_grabbed = -1;
    }

    brls::Box* makeRow(int idx) {
        const EditItem& it = m_items[idx];
        const bool grabbed = (idx == m_grabbed);

        auto* row = new brls::Box();
        row->setAxis(brls::Axis::ROW);
        row->setAlignItems(brls::AlignItems::CENTER);
        row->setHeight(m_rowH);
        row->setFocusable(true);
        if (grabbed) row->setBackgroundColor(pal::goldTint());

        // Gold accent rect on the left, like the sidebar's active item — shown
        // when grabbed. Reserves its slot otherwise so labels stay aligned.
        auto* accent = new brls::Box();
        accent->setWidth(m_accentW);
        accent->setHeight(m_rowH * 0.46f);
        accent->setCornerRadius(m_accentW * 0.5f);
        accent->setMarginLeft(m_padSide * 0.5f);
        accent->setMarginRight(m_padSide * 0.5f);
        accent->setBackgroundColor(pal::gold);
        accent->setVisibility(grabbed ? brls::Visibility::VISIBLE : brls::Visibility::INVISIBLE);
        row->addView(accent);

        auto* nm = new brls::Label();
        nm->setText(it.title);
        nm->setFontSize(m_fontSz);
        nm->setSingleLine(true);
        nm->setGrow(1.0f);
        nm->setTextColor(grabbed ? pal::gold : (it.hidden ? pal::dim : pal::text));
        row->addView(nm);

        // Hide toggle (touch). Fixed items reserve the slot instead.
        if (!it.fixed) {
            auto* eye = new brls::Box();
            eye->setWidth(30.0f);
            eye->setHeight(30.0f);
            eye->setCornerRadius(6.0f);
            eye->setJustifyContent(brls::JustifyContent::CENTER);
            eye->setAlignItems(brls::AlignItems::CENTER);
            eye->setMarginRight(m_padSide * 0.5f);
            auto* eyeIco = new brls::Image();
            eyeIco->setWidth(18.0f);
            eyeIco->setHeight(18.0f);
            eyeIco->setScalingType(brls::ImageScalingType::FIT);
            eyeIco->setImageFromRes(it.hidden ? "icons/hide.png" : "icons/show.png");
            eye->addView(eyeIco);
            eye->addGestureRecognizer(new brls::TapGestureRecognizer(
                [this, idx](brls::TapGestureStatus s, brls::Sound*) {
                    if (s.state == brls::GestureState::END) { m_focus = idx; toggleHide(); }
                }));
            row->addView(eye);
        } else {
            auto* sp = new brls::Box();
            sp->setWidth(30.0f);
            sp->setMarginRight(m_padSide * 0.5f);
            row->addView(sp);
        }

        row->getFocusEvent()->subscribe([this, idx](brls::View*) { m_focus = idx; });

        // Touch: drag a row to reorder it (fixed rows can't be dragged).
        if (!it.fixed) {
            row->addGestureRecognizer(new brls::PanGestureRecognizer(
                [this, idx](brls::PanGestureStatus st, brls::Sound*) { onRowPan(idx, st); },
                brls::PanAxis::VERTICAL));
        }

        return row;
    }

    void renderRows() {
        if (!m_listBox) return;
        m_listBox->clearViews();
        m_rows.clear();
        for (int i = 0; i < (int)m_items.size(); i++) {
            brls::Box* row = makeRow(i);
            m_listBox->addView(row);
            m_rows.push_back(row);
        }
        if (m_attached && !m_rows.empty()) {
            int f = std::min(std::max(m_focus, 0), (int)m_rows.size() - 1);
            brls::Application::giveFocus(m_rows[f]);
        }
    }

    // Touch drag-to-reorder: the dragged row follows the finger, neighbours
    // slide aside, and the drop reorders the model (clamped between the fixed
    // Home / Settings rows).
    void onRowPan(int idx, brls::PanGestureStatus st) {
        int n = (int)m_items.size();
        if (idx < 0 || idx >= n || m_items[idx].fixed) return;
        using GS = brls::GestureState;

        if (st.state == GS::UNSURE) {
            m_dragActive = true;
            m_dragFrom = idx;
            m_dragTarget = idx;
            m_dragStartY = st.position.y;
        } else if (st.state == GS::START || st.state == GS::STAY) {
            if (!m_dragActive) return;
            float dy = st.position.y - m_dragStartY;
            if (m_dragFrom < (int)m_rows.size()) {
                m_rows[m_dragFrom]->setTranslationY(dy);
                m_rows[m_dragFrom]->setBackgroundColor(pal::goldTint());
            }
            int target = m_dragFrom + (int)std::lround(dy / m_rowH);
            if (target < 1) target = 1;
            if (target > n - 2) target = n - 2;
            m_dragTarget = target;
            for (int i = 0; i < (int)m_rows.size(); i++) {
                if (i == m_dragFrom) continue;
                float shift = 0.0f;
                if (target > m_dragFrom && i > m_dragFrom && i <= target) shift = -m_rowH;
                else if (target < m_dragFrom && i >= target && i < m_dragFrom) shift = m_rowH;
                m_rows[i]->setTranslationY(shift);
            }
        } else if (st.state == GS::END || st.state == GS::FAILED) {
            if (!m_dragActive) return;
            m_dragActive = false;
            for (auto* r : m_rows) r->setTranslationY(0);
            if (st.state == GS::END && m_dragTarget != m_dragFrom &&
                m_dragFrom >= 1 && m_dragFrom <= n - 2 &&
                m_dragTarget >= 1 && m_dragTarget <= n - 2) {
                EditItem moved = m_items[m_dragFrom];
                m_items.erase(m_items.begin() + m_dragFrom);
                m_items.insert(m_items.begin() + m_dragTarget, moved);
                m_focus = m_dragTarget;
            }
            renderRows();
        }
    }

    void moveFocus(int dir) {
        int n = (int)m_items.size();
        if (n == 0) return;
        if (m_grabbed >= 0) {
            int ni = m_grabbed + dir;
            if (ni < 1 || ni > n - 2) return;  // keep Home / Settings pinned
            std::swap(m_items[m_grabbed], m_items[ni]);
            m_grabbed = ni;
            m_focus = ni;
            renderRows();
        } else {
            int ni = m_focus + dir;
            if (ni < 0 || ni > n - 1) return;
            m_focus = ni;
            if (m_attached && ni < (int)m_rows.size())
                brls::Application::giveFocus(m_rows[ni]);
        }
    }

    void toggleGrab() {
        if (m_focus < 0 || m_focus >= (int)m_items.size()) return;
        if (m_items[m_focus].fixed) return;
        m_grabbed = (m_grabbed == m_focus) ? -1 : m_focus;
        renderRows();
    }

    void toggleHide() {
        if (m_focus < 0 || m_focus >= (int)m_items.size()) return;
        EditItem& it = m_items[m_focus];
        if (it.fixed) return;
        it.hidden = !it.hidden;
        renderRows();
    }

    void commit() {
        AppSettings& s = Application::getInstance().getSettings();

        std::vector<std::string> owned;
        for (const auto& it : m_items)
            if (!it.fixed) owned.push_back(it.id);
        auto isOwned = [&](const std::string& id) {
            return std::find(owned.begin(), owned.end(), id) != owned.end();
        };

        std::vector<std::string> order;
        for (const auto& it : m_items)
            if (!it.fixed) order.push_back(it.id);
        for (const auto& id : splitCsv(s.sidebarOrder))
            if (!isOwned(id)) order.push_back(id);

        std::vector<std::string> hiddenItems;
        for (const auto& it : m_items)
            if (!it.fixed && !it.isLibrary && it.hidden) hiddenItems.push_back(it.id);
        for (const auto& id : splitCsv(s.hiddenSidebarItems))
            if (!isOwned(id)) hiddenItems.push_back(id);

        s.sidebarOrder = joinCsv(order);
        s.hiddenSidebarItems = joinCsv(hiddenItems);

        if (m_libsFetched) {
            std::vector<std::string> hiddenLibs;
            for (const auto& it : m_items)
                if (it.isLibrary && it.hidden) hiddenLibs.push_back(it.id.substr(4));
            for (const auto& key : splitCsv(s.hiddenLibraries))
                if (!isOwned("lib:" + key)) hiddenLibs.push_back(key);
            s.hiddenLibraries = joinCsv(hiddenLibs);
        }

        Application::getInstance().saveSettings();

        brls::Application::popActivity();
        brls::sync([]() {
            if (auto* main = MainActivity::getInstance())
                main->rebuildSidebar();
        });
    }

    brls::Box* m_listBox = nullptr;
    std::vector<brls::Box*> m_rows;
    std::vector<EditItem> m_items;
    int m_focus = 0;
    int m_grabbed = -1;
    bool m_libsFetched = false;
    bool m_attached = false;

    // Sidebar metrics (matched at open) + touch-drag state.
    float m_rowH = 70.0f, m_fontSz = 22.0f, m_padSide = 16.0f, m_accentW = 4.0f;
    bool  m_dragActive = false;
    int   m_dragFrom = -1, m_dragTarget = -1;
    float m_dragStartY = 0.0f;

    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
};

} // namespace

void SidebarEditor::open() {
    brls::Application::pushActivity(new SidebarEditorActivity());
}

} // namespace vitaplex
