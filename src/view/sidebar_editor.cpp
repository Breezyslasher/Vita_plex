/**
 * VitaPlex - Sidebar Editor (Direction D) implementation
 */

#include "view/sidebar_editor.hpp"
#include "activity/main_activity.hpp"
#include "app/application.hpp"
#include "app/plex_client.hpp"

#include <borealis.hpp>
#include <algorithm>
#include <string>
#include <vector>

namespace vitaplex {

namespace {

// ---- Direction-D design tokens -------------------------------------------
namespace sb {
    inline NVGcolor rail()    { return nvgRGB(0x16, 0x16, 0x17); }
    inline NVGcolor veil()    { return nvgRGBA(8, 8, 9, 158); }      // rgba .62
    inline NVGcolor rowGrab() { return nvgRGB(0x3a, 0x3a, 0x46); }
    inline NVGcolor noteBg()  { return nvgRGBA(229, 160, 13, 20); }  // gold .08
    inline NVGcolor noteBd()  { return nvgRGBA(229, 160, 13, 56); }  // gold .22
    inline NVGcolor goldTile(){ return nvgRGBA(229, 160, 13, 36); }  // gold .14
    inline NVGcolor gold()    { return nvgRGB(0xe5, 0xa0, 0x0d); }
    inline NVGcolor softGold(){ return nvgRGB(0xe7, 0xc8, 0x7a); }
    inline NVGcolor text()    { return nvgRGB(0xff, 0xff, 0xff); }
    inline NVGcolor muted()   { return nvgRGB(0xb4, 0xb4, 0xba); }
    inline NVGcolor muted2()  { return nvgRGB(0x8a, 0x8a, 0x90); }
    inline NVGcolor muted3()  { return nvgRGB(0x6a, 0x6a, 0x70); }
    inline NVGcolor mutedDim(){ return nvgRGBA(0x8a, 0x8a, 0x90, 115); }  // grip on fixed
    inline NVGcolor hair()    { return nvgRGB(0x2a, 0x2a, 0x2e); }
    inline NVGcolor tagBd()   { return nvgRGB(0x3a, 0x3a, 0x40); }
    inline NVGcolor surface2(){ return nvgRGB(0x40, 0x40, 0x40); }
    inline NVGcolor line()    { return nvgRGB(0x47, 0x47, 0x47); }
    inline NVGcolor capInk()  { return nvgRGB(0xc2, 0xc2, 0xc8); }
}

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
    std::string id;        // home, search, livetv, downloads, library, music, settings, lib:<key>
    std::string title;
    bool fixed = false;    // home, settings — can't move or hide
    bool hidden = false;
    bool isLibrary = false;
};

// Full-screen translucent editor: a 262px rail on the left (opaque) with the
// rest of the app showing through a dim veil.
class SidebarEditorActivity : public brls::Activity {
public:
    bool isTranslucent() override { return true; }

    brls::View* createContentView() override {
        buildModel();

        auto* root = new brls::Box();
        root->setAxis(brls::Axis::ROW);
        root->setWidthPercentage(100.0f);
        root->setHeightPercentage(100.0f);

        // ---- Rail (edit panel) ----
        auto* panel = new brls::Box();
        panel->setAxis(brls::Axis::COLUMN);
        panel->setWidth(262.0f);
        panel->setHeightPercentage(100.0f);
        panel->setBackgroundColor(sb::rail());
        panel->setPaddingTop(18.0f);
        panel->setPaddingBottom(18.0f);
        panel->setShadowType(brls::ShadowType::GENERIC);

        // Header: "Edit Sidebar" + Done
        auto* head = new brls::Box();
        head->setAxis(brls::Axis::ROW);
        head->setAlignItems(brls::AlignItems::CENTER);
        head->setJustifyContent(brls::JustifyContent::SPACE_BETWEEN);
        head->setPaddingLeft(18.0f);
        head->setPaddingRight(18.0f);
        head->setMarginBottom(14.0f);
        auto* htitle = new brls::Label();
        htitle->setText("Edit Sidebar");
        htitle->setFontSize(15.0f);
        htitle->setTextColor(sb::text());
        head->addView(htitle);
        auto* done = new brls::Label();
        done->setText("✓ Done");
        done->setFontSize(12.5f);
        done->setTextColor(sb::gold());
        head->addView(done);
        head->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus s, brls::Sound*) {
                if (s.state == brls::GestureState::END) commit();
            }));
        panel->addView(head);

        // Libraries note — only when libraries-in-sidebar is on
        if (m_libsMode) {
            auto* note = new brls::Box();
            note->setAxis(brls::Axis::ROW);
            note->setAlignItems(brls::AlignItems::CENTER);
            note->setMarginLeft(12.0f);
            note->setMarginRight(12.0f);
            note->setMarginBottom(12.0f);
            note->setPaddingTop(9.0f);
            note->setPaddingBottom(9.0f);
            note->setPaddingLeft(12.0f);
            note->setPaddingRight(12.0f);
            note->setCornerRadius(10.0f);
            note->setBackgroundColor(sb::noteBg());
            note->setBorderColor(sb::noteBd());
            note->setBorderThickness(1.0f);

            auto* tile = new brls::Box();
            tile->setWidth(24.0f);
            tile->setHeight(24.0f);
            tile->setCornerRadius(6.0f);
            tile->setBackgroundColor(sb::goldTile());
            tile->setJustifyContent(brls::JustifyContent::CENTER);
            tile->setAlignItems(brls::AlignItems::CENTER);
            tile->setMarginRight(9.0f);
            auto* tileIco = new brls::Image();
            tileIco->setWidth(14.0f);
            tileIco->setHeight(14.0f);
            tileIco->setScalingType(brls::ImageScalingType::FIT);
            tileIco->setImageFromRes("icons/book-multiple.png");
            tile->addView(tileIco);
            note->addView(tile);

            auto* cap = new brls::Label();
            cap->setText("Libraries appear here because \"Libraries in Sidebar\" is on. "
                         "Change that in Settings.");
            cap->setFontSize(10.5f);
            cap->setTextColor(sb::muted());
            cap->setGrow(1.0f);
            note->addView(cap);
            panel->addView(note);
        }

        // Section label
        auto* sec = new brls::Label();
        sec->setText("PRESS A TO MOVE · X TO HIDE");
        sec->setFontSize(10.5f);
        sec->setTextColor(sb::muted2());
        sec->setMarginLeft(18.0f);
        sec->setMarginRight(18.0f);
        sec->setMarginBottom(8.0f);
        sec->setMarginTop(4.0f);
        panel->addView(sec);

        // Scrollable row list
        auto* scroll = new brls::ScrollingFrame();
        scroll->setGrow(1.0f);
        m_listBox = new brls::Box();
        m_listBox->setAxis(brls::Axis::COLUMN);
        m_listBox->setPaddingLeft(10.0f);
        m_listBox->setPaddingRight(10.0f);
        scroll->setContentView(m_listBox);
        panel->addView(scroll);

        // Footer: top hairline + button hints
        auto* hair = new brls::Box();
        hair->setHeight(1.0f);
        hair->setMarginTop(6.0f);
        hair->setMarginLeft(18.0f);
        hair->setMarginRight(18.0f);
        hair->setBackgroundColor(sb::hair());
        panel->addView(hair);

        auto* foot = new brls::Box();
        foot->setAxis(brls::Axis::ROW);
        foot->setAlignItems(brls::AlignItems::CENTER);
        foot->setPaddingLeft(16.0f);
        foot->setPaddingRight(16.0f);
        foot->setPaddingTop(11.0f);
        addHint(foot, "A", "Grab / move");
        addHint(foot, "X", "Hide");
        addHint(foot, "B", "Done");
        panel->addView(foot);

        root->addView(panel);

        // ---- Veil over the dimmed app ----
        auto* veil = new brls::Box();
        veil->setGrow(1.0f);
        veil->setHeightPercentage(100.0f);
        veil->setBackgroundColor(sb::veil());
        veil->addGestureRecognizer(new brls::TapGestureRecognizer(
            [this](brls::TapGestureStatus s, brls::Sound*) {
                if (s.state == brls::GestureState::END) commit();
            }));
        root->addView(veil);

        // ---- Input ----
        root->registerAction("", brls::ControllerButton::BUTTON_NAV_UP,
            [this](brls::View*) { moveFocus(-1); return true; }, /*hidden*/ true, /*repeat*/ true);
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
    void addHint(brls::Box* parent, const std::string& cap, const std::string& label) {
        auto* k = new brls::Box();
        k->setHeight(18.0f);
        k->setWidth(18.0f);
        k->setPaddingLeft(5.0f);
        k->setPaddingRight(5.0f);
        k->setCornerRadius(5.0f);
        k->setJustifyContent(brls::JustifyContent::CENTER);
        k->setAlignItems(brls::AlignItems::CENTER);
        k->setBackgroundColor(sb::surface2());
        k->setBorderColor(sb::line());
        k->setBorderThickness(1.0f);
        k->setMarginRight(6.0f);
        auto* kl = new brls::Label();
        kl->setText(cap);
        kl->setFontSize(9.0f);
        kl->setTextColor(sb::capInk());
        k->addView(kl);
        parent->addView(k);
        auto* l = new brls::Label();
        l->setText(label);
        l->setFontSize(10.5f);
        l->setTextColor(sb::muted2());
        l->setMarginRight(12.0f);
        parent->addView(l);
    }

    void buildModel() {
        m_items.clear();
        AppSettings& s = Application::getInstance().getSettings();
        m_libsMode = s.showLibrariesInSidebar;
        bool hasLiveTV = PlexClient::getInstance().hasLiveTV();

        m_items.push_back({ "home", "Home", true, false, false });

        // Movable universe (default order) + titles
        std::vector<std::pair<std::string, std::string>> universe;
        if (m_libsMode) {
            std::vector<LibrarySection> sections;
            PlexClient::getInstance().fetchLibrarySections(sections);
            for (const auto& sec : sections)
                universe.push_back({ "lib:" + sec.key, sec.title });
        } else {
            universe.push_back({ "library", "Library" });
            universe.push_back({ "music", "Music" });
        }
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
        row->setHeight(44.0f);
        row->setCornerRadius(9.0f);
        row->setPaddingLeft(10.0f + (it.isLibrary ? 8.0f : 0.0f));  // library rows indent
        row->setPaddingRight(10.0f);
        row->setMarginBottom(6.0f);
        row->setFocusable(true);
        row->setHighlightCornerRadius(9.0f);
        if (grabbed) {
            row->setBackgroundColor(sb::rowGrab());
            row->setBorderColor(sb::gold());
            row->setBorderThickness(2.0f);
        }

        // Grip (three bars). Dimmed for fixed rows (lock substitute).
        auto* grip = new brls::Box();
        grip->setAxis(brls::Axis::COLUMN);
        grip->setJustifyContent(brls::JustifyContent::CENTER);
        grip->setWidth(13.0f);
        grip->setMarginRight(10.0f);
        NVGcolor gripCol = grabbed ? sb::gold() : (it.fixed ? sb::mutedDim() : sb::muted2());
        for (int b = 0; b < 3; b++) {
            auto* bar = new brls::Box();
            bar->setWidth(13.0f);
            bar->setHeight(2.0f);
            bar->setCornerRadius(1.0f);
            bar->setBackgroundColor(gripCol);
            if (b < 2) bar->setMarginBottom(3.0f);
            grip->addView(bar);
        }
        row->addView(grip);

        // Name
        auto* nm = new brls::Label();
        nm->setText(it.title);
        nm->setFontSize(13.0f);
        nm->setSingleLine(true);
        nm->setGrow(1.0f);
        nm->setTextColor(grabbed ? sb::text() : (it.hidden ? sb::muted3() : sb::muted()));
        row->addView(nm);

        // Trailing tag: HIDDEN takes priority, else LIBRARY for library rows
        if (it.hidden) {
            row->addView(makeTag("HIDDEN", sb::muted3()));
        } else if (it.isLibrary) {
            row->addView(makeTag("LIBRARY", sb::muted3()));
        }

        row->getFocusEvent()->subscribe([this, idx](brls::View*) { m_focus = idx; });

        return row;
    }

    brls::Box* makeTag(const std::string& text, NVGcolor color) {
        auto* tag = new brls::Box();
        tag->setCornerRadius(4.0f);
        tag->setPaddingLeft(5.0f);
        tag->setPaddingRight(5.0f);
        tag->setPaddingTop(1.0f);
        tag->setPaddingBottom(1.0f);
        tag->setMarginLeft(6.0f);
        tag->setBorderColor(sb::tagBd());
        tag->setBorderThickness(1.0f);
        auto* l = new brls::Label();
        l->setText(text);
        l->setFontSize(9.0f);
        l->setTextColor(color);
        tag->addView(l);
        return tag;
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

    void moveFocus(int dir) {
        int n = (int)m_items.size();
        if (n == 0) return;
        if (m_grabbed >= 0) {
            // Reorder the grabbed item, staying between Home (0) and Settings (n-1)
            int ni = m_grabbed + dir;
            if (ni < 1 || ni > n - 2) return;
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
        if (m_items[m_focus].fixed) return;  // Home / Settings can't move
        m_grabbed = (m_grabbed == m_focus) ? -1 : m_focus;
        renderRows();
    }

    void toggleHide() {
        if (m_focus < 0 || m_focus >= (int)m_items.size()) return;
        EditItem& it = m_items[m_focus];
        if (it.fixed) return;  // Home / Settings always visible
        it.hidden = !it.hidden;
        renderRows();
    }

    void commit() {
        AppSettings& s = Application::getInstance().getSettings();

        // Ids the editor owns this session (current-mode movable items). Ids in
        // the saved settings that aren't owned belong to the *other* mode and
        // are preserved so toggling "Libraries in Sidebar" doesn't wipe them.
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

        // Libraries are only shown (and thus owned) in libraries-in-sidebar mode;
        // don't touch hiddenLibraries from premade mode.
        if (m_libsMode) {
            std::vector<std::string> hiddenLibs;
            for (const auto& it : m_items)
                if (it.isLibrary && it.hidden) hiddenLibs.push_back(it.id.substr(4));
            s.hiddenLibraries = joinCsv(hiddenLibs);
        }

        Application::getInstance().saveSettings();

        // Pop first, then rebuild the live sidebar on the next frame once the
        // editor activity is gone, so focus lands cleanly on the new sidebar.
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
    bool m_libsMode = false;
    bool m_attached = false;
};

} // namespace

void SidebarEditor::open() {
    brls::Application::pushActivity(new SidebarEditorActivity());
}

} // namespace vitaplex
