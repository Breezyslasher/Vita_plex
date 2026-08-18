/**
 * VitaPlex - Live TV actions implementation
 *
 * The recording POST is lifted from LiveTVTab::scheduleRecording — the
 * version that has been working against a real DVR — split so the
 * record-options dialog can layer its own values onto it. The two dialogs
 * follow design_handoff_epg_menu (Program Card D / Record Options B).
 */

#include "view/livetv_actions.hpp"

#include <borealis.hpp>

#include <atomic>
#include <ctime>
#include <memory>
#include <vector>

#include "app/application.hpp"
#include "utils/air_time.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"
#include "utils/image_loader.hpp"
#include "platform/platform.hpp"

namespace vitaplex {

namespace {

// ── Design tokens (design_handoff_epg_menu/README.md) ───────────────────
namespace tok {
    inline NVGcolor panel()      { return nvgRGB(0x26, 0x26, 0x2a); }
    inline NVGcolor panelLine()  { return nvgRGB(0x45, 0x45, 0x4d); }
    inline NVGcolor hairline()   { return nvgRGB(0x47, 0x47, 0x47); }
    inline NVGcolor rowSep()     { return nvgRGBA(255, 255, 255, 13); }   // .05
    inline NVGcolor inputBg()    { return nvgRGB(0x2e, 0x2e, 0x34); }
    inline NVGcolor gold()       { return nvgRGB(0xe5, 0xa0, 0x0d); }
    inline NVGcolor goldInk()    { return nvgRGB(0x24, 0x1c, 0x08); }
    inline NVGcolor liveRed()    { return nvgRGB(0xe5, 0x48, 0x4d); }
    inline NVGcolor warnBg()     { return nvgRGBA(0xe5, 0x48, 0x4d, 23); }  // .09
    inline NVGcolor warnBorder() { return nvgRGBA(0xe5, 0x48, 0x4d, 64); }  // .25
    inline NVGcolor warnText()   { return nvgRGB(0xd8, 0xa5, 0xa7); }
    inline NVGcolor text()       { return nvgRGB(255, 255, 255); }
    inline NVGcolor muted()      { return nvgRGB(0xb4, 0xb4, 0xba); }
    inline NVGcolor muted2()     { return nvgRGB(0x8a, 0x8a, 0x90); }
    inline NVGcolor disabled()   { return nvgRGB(0x6a, 0x6a, 0x70); }
    inline NVGcolor track()      { return nvgRGBA(255, 255, 255, 41); }   // .16
    inline NVGcolor btnGray()    { return nvgRGB(0x3e, 0x3e, 0x46); }
    inline NVGcolor scrim()      { return nvgRGBA(10, 9, 14, 150); }
}

// Same offset and quality steps the DVR settings page exposes, so a value
// picked here always round-trips to something that page can show.
const std::vector<int> kOffsetMinutes  = { 0, 1, 2, 3, 5, 10 };
const std::vector<int> kMinQualities   = { 0, 50, 75, 100 };
const std::vector<std::string> kMinQualityLabels = {
    "Any quality", "480p or better", "720p or better", "1080p or better"
};

// Translucent host so the screen behind shows through the scrim (same
// shape as media_detail_view's PopoverActivity, which is file-local there).
class OverlayActivity : public brls::Activity {
public:
    explicit OverlayActivity(brls::Box* content) : brls::Activity(content) {}
    bool isTranslucent() override { return true; }
};

// A Box that owns an ImageLoader alive-flag, flipped when the dialog's
// view tree is destroyed, so an in-flight poster load can't touch a freed
// Image (the same pattern the media cells use).
class FlagBox : public brls::Box {
public:
    std::shared_ptr<std::atomic<bool>> alive = std::make_shared<std::atomic<bool>>(true);
    ~FlagBox() override { *alive = false; }
};

// String-aware walk of the objects of a JSON array — a stray brace inside
// a title or summary must not end the walk (plex_client keeps its own
// copy of this private, and the template response carries free text).
template <typename Fn>
void forEachObject(const std::string& body, const std::string& arrayKey, Fn fn) {
    size_t pos = body.find(arrayKey);
    if (pos == std::string::npos) return;
    pos = body.find('[', pos);
    if (pos == std::string::npos) return;
    pos++;
    while (pos < body.size()) {
        while (pos < body.size() && body[pos] != '{' && body[pos] != ']') pos++;
        if (pos >= body.size() || body[pos] == ']') break;
        size_t objStart = pos;
        int depth = 0;
        bool inString = false;
        for (; pos < body.size(); pos++) {
            const char c = body[pos];
            if (inString) {
                if (c == '\\') { pos++; continue; }
                if (c == '"') inString = false;
                continue;
            }
            if (c == '"') { inString = true; continue; }
            if (c == '{') depth++;
            else if (c == '}' && --depth == 0) { pos++; break; }
        }
        fn(body.substr(objStart, pos - objStart));
    }
}

// ── Subscription template ────────────────────────────────────────────────
// One entry of the template's MediaSubscription[] — "This Episode",
// "All Episodes" — with the server's pre-encoded parameters kept verbatim.
struct TemplateOption {
    std::string title;
    std::string parameters;
    std::string type;
    std::string targetSection;
    bool selected = false;
};

struct RecordingTemplate {
    std::vector<TemplateOption> options;
    int selectedIndex = 0;
};

// The per-recording values the record-options dialog edits. Initialised
// from AppSettings; posted with the subscription; never written back.
struct RecordingPrefs {
    int startOffsetMin   = 0;
    int endOffsetMin     = 0;
    int minVideoQuality  = 0;
    std::string targetSectionId;   // empty = template's recommendation
};

RecordingPrefs defaultPrefs() {
    const AppSettings& s = Application::getInstance().getSettings();
    RecordingPrefs p;
    p.startOffsetMin  = s.dvrStartOffsetMinutes;
    p.endOffsetMin    = s.dvrEndOffsetMinutes;
    p.minVideoQuality = s.dvrMinVideoQuality;
    p.targetSectionId = s.defaultDvrSectionId;
    return p;
}

// GET /media/subscriptions/template?guid=… and parse every offered
// subscription (the old flow kept only the selected one; the dialog needs
// them all to draw the scope rows). Blocking — call from a worker thread.
bool fetchRecordingTemplate(const std::string& guid, RecordingTemplate& out) {
    out.options.clear();
    out.selectedIndex = 0;
    if (guid.empty()) return false;

    PlexClient& client = PlexClient::getInstance();
    HttpClient httpClient;

    HttpRequest req;
    req.url = client.buildApiUrlPublic("/media/subscriptions/template?guid=" + guid);
    req.method = "GET";
    req.headers["Accept"] = "application/json";
    req.timeout = 15;

    brls::Logger::debug("fetchRecordingTemplate: {}", redactTokensInUrl(req.url));
    HttpResponse resp = httpClient.request(req);
    if (resp.statusCode != 200 || resp.body.empty()) {
        brls::Logger::error("fetchRecordingTemplate: HTTP {}", resp.statusCode);
        return false;
    }

    forEachObject(resp.body, "\"MediaSubscription\"", [&](const std::string& ms) {
        TemplateOption opt;
        opt.title         = client.extractJsonValuePublic(ms, "title");
        opt.parameters    = client.extractJsonValuePublic(ms, "parameters");
        opt.type          = client.extractJsonValuePublic(ms, "type");
        opt.targetSection = client.extractJsonValuePublic(ms, "targetLibrarySectionID");
        opt.selected      = ms.find("\"selected\":true") != std::string::npos;
        if (!opt.parameters.empty() && !opt.type.empty()) {
            if (opt.selected) out.selectedIndex = (int)out.options.size();
            out.options.push_back(std::move(opt));
        }
    });

    brls::Logger::info("fetchRecordingTemplate: {} options", out.options.size());
    return !out.options.empty();
}

// POST /media/subscriptions from a template option plus the dialog's
// values — the same request LiveTVTab::scheduleRecording used to build,
// with the prefs made explicit instead of read from the globals.
// Blocking — call from a worker thread.
bool postRecordingSubscription(const TemplateOption& opt, const RecordingPrefs& prefs,
                               bool oneShot) {
    PlexClient& client = PlexClient::getInstance();
    HttpClient httpClient;

    std::string targetSection = opt.targetSection;
    if (!prefs.targetSectionId.empty()) targetSection = prefs.targetSectionId;

    const AppSettings& settings = Application::getInstance().getSettings();

    std::string post = client.buildApiUrlPublic("/media/subscriptions");
    post += "&" + opt.parameters;
    post += "&type=" + opt.type;
    if (!targetSection.empty()) post += "&targetLibrarySectionID=" + targetSection;
    post += "&includeGrabs=1";
    post += std::string("&prefs[oneShot]=") + (oneShot ? "true" : "false");
    post += std::string("&prefs[recordPartials]=") + (settings.dvrRecordPartials ? "true" : "false");
    post += "&prefs[minVideoQuality]=" + std::to_string(prefs.minVideoQuality);
    post += "&prefs[startOffsetMinutes]=" + std::to_string(prefs.startOffsetMin);
    post += "&prefs[endOffsetMinutes]=" + std::to_string(prefs.endOffsetMin);

    HttpRequest req;
    req.url = post;
    req.method = "POST";
    req.headers["Accept"] = "application/json";
    req.timeout = 15;

    brls::Logger::debug("postRecordingSubscription: POST {}", redactTokensInUrl(post));
    HttpResponse resp = httpClient.request(req);
    brls::Logger::debug("postRecordingSubscription: response {} ({} bytes)",
                        resp.statusCode, resp.body.length());
    return resp.statusCode == 200 || resp.statusCode == 201;
}

// ── Small UI helpers ─────────────────────────────────────────────────────

brls::Label* makeLabel(const std::string& text, float size, NVGcolor color,
                       bool singleLine = true) {
    auto* l = new brls::Label();
    l->setText(text);
    l->setFontSize(size);
    l->setTextColor(color);
    l->setSingleLine(singleLine);
    return l;
}

// A dialog button: gold primary, gray secondary, or ghost.
enum class BtnStyle { Gold, Gray, Ghost };

brls::Box* makeButton(const std::string& text, BtnStyle style,
                      std::function<void()> onClick) {
    auto* b = new brls::Box();
    b->setAxis(brls::Axis::ROW);
    b->setJustifyContent(brls::JustifyContent::CENTER);
    b->setAlignItems(brls::AlignItems::CENTER);
    b->setHeight(42.0f);
    b->setCornerRadius(10.0f);
    b->setFocusable(true);
    b->setHighlightCornerRadius(10.0f);

    NVGcolor fg = tok::text();
    if (style == BtnStyle::Gold) {
        b->setBackgroundColor(tok::gold());
        fg = tok::goldInk();
    } else if (style == BtnStyle::Gray) {
        b->setBackgroundColor(tok::btnGray());
        b->setBorderColor(tok::hairline());
        b->setBorderThickness(1.0f);
    } else {
        fg = tok::muted();
    }
    auto* l = makeLabel(text, 13.5f, fg);
    b->addView(l);

    b->registerClickAction([onClick](brls::View*) {
        if (onClick) onClick();
        return true;
    });
    b->addGestureRecognizer(new brls::TapGestureRecognizer(b));
    return b;
}

// "49m left" / "starts in 25 min" relative-time captions.
std::string minutesLeftLabel(int64_t endAt, int64_t now) {
    int64_t mins = (endAt - now + 59) / 60;
    if (mins < 1) mins = 1;
    if (mins >= 60)
        return std::to_string(mins / 60) + "h " + std::to_string(mins % 60) + "m left";
    return std::to_string(mins) + "m left";
}

std::string startsInLabel(int64_t startAt, int64_t now) {
    int64_t mins = (startAt - now + 59) / 60;
    if (mins < 1) mins = 1;
    if (mins >= 60)
        return "starts in " + std::to_string(mins / 60) + "h " + std::to_string(mins % 60) + "m";
    return "starts in " + std::to_string(mins) + " min";
}

// "61.1 WOSCCD (MeTV Toons)" → callsign "WOSCCD", channel number "61.1".
// The Home/search items only carry the composed title, so the chip line is
// parsed back out of it; anything unparseable falls back to the full string.
void splitChannelTitle(const std::string& channelTitle,
                       std::string& callsign, std::string& number) {
    callsign.clear();
    number.clear();
    size_t sp = channelTitle.find(' ');
    if (sp == std::string::npos) { callsign = channelTitle; return; }
    const std::string first = channelTitle.substr(0, sp);
    if (!first.empty() && (isdigit((unsigned char)first[0]))) {
        number = first;
        size_t sp2 = channelTitle.find(' ', sp + 1);
        callsign = channelTitle.substr(sp + 1, sp2 == std::string::npos
                                                   ? std::string::npos : sp2 - sp - 1);
    } else {
        callsign = first;
    }
}

// The existing confirmation dialog, kept as-is per the handoff.
void showOutcome(bool success, const std::string& title,
                 std::function<void(bool)> onScheduled) {
    brls::Dialog* dialog = new brls::Dialog(
        success ? "Recording scheduled: " + title
                : "Failed to schedule recording: " + title);
    dialog->addButton("OK", []() {});
    dialog->open();
    if (onScheduled) onScheduled(success);
}

// A gold-fill progress bar at `fraction` of its width.
brls::Box* makeProgressBar(float width, float fraction) {
    auto* trackBox = new brls::Box();
    trackBox->setWidth(width);
    trackBox->setHeight(3.0f);
    trackBox->setCornerRadius(1.5f);
    trackBox->setBackgroundColor(tok::track());
    if (fraction > 0.0f) {
        auto* fill = new brls::Rectangle();
        float w = width * fraction;
        if (w < 2.0f) w = 2.0f;
        if (w > width) w = width;
        fill->setWidth(w);
        fill->setHeight(3.0f);
        fill->setCornerRadius(1.5f);
        fill->setColor(tok::gold());
        trackBox->addView(fill);
    }
    return trackBox;
}

// ── Record options dialog ────────────────────────────────────────────────

// One settings row: icon slot, label, gold changed-dot, value pill.
// left/right (and click) step the value; `render` refreshes the pill text
// and the dot after every change.
struct SettingRowState {
    brls::Label*     value  = nullptr;
    brls::Rectangle* dot    = nullptr;
};

brls::Box* makeSettingRow(const std::string& label, SettingRowState* state,
                          std::function<void(int)> step,
                          std::function<void()> openPicker = {}) {
    auto* row = new brls::Box();
    row->setAxis(brls::Axis::ROW);
    row->setAlignItems(brls::AlignItems::CENTER);
    row->setHeight(40.0f);
    row->setPadding(0.0f, 10.0f, 0.0f, 10.0f);
    row->setCornerRadius(8.0f);
    row->setFocusable(true);
    row->setHighlightCornerRadius(8.0f);

    auto* l = makeLabel(label, 12.0f, tok::muted());
    row->addView(l);

    // Gold dot marking a value changed from the app default.
    auto* dot = new brls::Rectangle();
    dot->setWidth(5.0f);
    dot->setHeight(5.0f);
    dot->setCornerRadius(2.5f);
    dot->setColor(tok::gold());
    dot->setMarginLeft(6.0f);
    dot->setVisibility(brls::Visibility::INVISIBLE);
    row->addView(dot);

    auto* spacer = new brls::Box();
    spacer->setGrow(1.0f);
    row->addView(spacer);

    auto* pill = new brls::Box();
    pill->setAxis(brls::Axis::ROW);
    pill->setAlignItems(brls::AlignItems::CENTER);
    pill->setHeight(28.0f);
    pill->setPadding(0.0f, 10.0f, 0.0f, 10.0f);
    pill->setCornerRadius(8.0f);
    pill->setBackgroundColor(tok::inputBg());
    pill->setBorderColor(tok::hairline());
    pill->setBorderThickness(1.0f);
    auto* value = makeLabel("", 12.0f, tok::text());
    pill->addView(value);
    if (openPicker) {
        // Chevron marks the rows that open a picker rather than stepping.
        auto* chev = makeLabel("\xE2\x96\xBE", 10.0f, tok::muted2());
        chev->setMarginLeft(6.0f);
        pill->addView(chev);
    }
    row->addView(pill);

    state->value = value;
    state->dot   = dot;

    row->registerAction("Prev", brls::ControllerButton::BUTTON_LEFT,
        [step](brls::View*) { if (step) step(-1); return true; }, true);
    row->registerAction("Next", brls::ControllerButton::BUTTON_RIGHT,
        [step](brls::View*) { if (step) step(+1); return true; }, true);
    row->registerClickAction([step, openPicker](brls::View*) {
        // Click opens the picker where one exists (quality, library);
        // steppers keep click-to-advance. Left/right steps either way.
        if (openPicker) openPicker();
        else if (step)  step(+1);
        return true;
    });
    row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
    return row;
}

// Shared mutable state for the record-options dialog. Held by shared_ptr
// in every closure so nothing dangles while the dialog is open.
struct RecordDialogState {
    RecordingTemplate tmpl;
    RecordingPrefs    prefs;         // live values, edited by the rows
    RecordingPrefs    initial;       // for Reset to defaults + the dots
    int scopeIndex = 0;

    std::vector<LibrarySection> sections;   // eligible "Records to" targets

    SettingRowState startRow, endRow, qualityRow, sectionRow;
    std::vector<brls::Label*> scopeTitles;  // to re-tint on selection
    std::vector<brls::Rectangle*> scopeMarks;

    std::function<void()> refresh;   // re-renders values/dots/scope marks
};

int indexOfOffset(int minutes) {
    for (size_t i = 0; i < kOffsetMinutes.size(); i++)
        if (kOffsetMinutes[i] == minutes) return (int)i;
    return 0;
}

int indexOfQuality(int q) {
    for (size_t i = 0; i < kMinQualities.size(); i++)
        if (kMinQualities[i] == q) return (int)i;
    return 0;
}

void openRecordOptionsDialog(const MediaItem& item, RecordingTemplate tmpl,
                             std::vector<LibrarySection> sections,
                             std::function<void(bool)> onScheduled) {
    const float screenW = platform::viewportWidth();
    const float screenH = platform::viewportHeight();
    float panelW = 436.0f;
    if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;

    auto st = std::make_shared<RecordDialogState>();
    st->tmpl       = std::move(tmpl);
    st->prefs      = defaultPrefs();
    st->initial    = st->prefs;
    st->scopeIndex = st->tmpl.selectedIndex;
    st->sections   = std::move(sections);

    // The app-wide default library may be the wrong type for this
    // recording (a TV default while recording a movie). Fall back to the
    // template's recommendation rather than posting a mismatched section
    // while the pill claims "Server default".
    if (!st->prefs.targetSectionId.empty()) {
        bool known = false;
        for (const auto& sec : st->sections)
            if (sec.key == st->prefs.targetSectionId) { known = true; break; }
        if (!known) {
            st->prefs.targetSectionId.clear();
            st->initial.targetSectionId.clear();
        }
    }

    const bool series = st->tmpl.options.size() >= 2;

    // ── Scrim + panel ───────────────────────────────────────────────────
    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(tok::scrim());

    auto* panel = new FlagBox();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(panelW);
    panel->setBackgroundColor(tok::panel());
    panel->setBorderColor(tok::panelLine());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(16.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);

    // ── Header ──────────────────────────────────────────────────────────
    auto* header = new brls::Box();
    header->setAxis(brls::Axis::COLUMN);
    header->setPadding(14.0f, 18.0f, 12.0f, 18.0f);

    auto* titleRow = new brls::Box();
    titleRow->setAxis(brls::Axis::ROW);
    titleRow->setAlignItems(brls::AlignItems::CENTER);
    auto* recDot = new brls::Rectangle();
    recDot->setWidth(10.0f);
    recDot->setHeight(10.0f);
    recDot->setCornerRadius(5.0f);
    recDot->setColor(tok::liveRed());
    recDot->setMarginRight(8.0f);
    titleRow->addView(recDot);
    titleRow->addView(makeLabel("Record", 15.0f, tok::text()));
    header->addView(titleRow);

    std::string programLine = item.title;
    if (!item.liveChannelTitle.empty()) programLine += "  \xC2\xB7  " + item.liveChannelTitle;
    auto* pl = makeLabel(programLine, 12.0f, tok::muted());
    pl->setMarginTop(4.0f);
    header->addView(pl);

    const int64_t now = (int64_t)time(nullptr);
    std::string airLine = airWindowLabel(item.airStartAt, item.airEndAt);
    if (item.airStartAt > now) {
        airLine += (airLine.empty() ? "" : "  \xC2\xB7  ") + startsInLabel(item.airStartAt, now);
    }
    if (!airLine.empty()) {
        auto* al = makeLabel(airLine, 11.0f, tok::muted2());
        al->setMarginTop(2.0f);
        header->addView(al);
    }
    panel->addView(header);

    auto* headerRule = new brls::Box();
    headerRule->setHeight(1.0f);
    headerRule->setAlignSelf(brls::AlignSelf::STRETCH);
    headerRule->setBackgroundColor(tok::hairline());
    panel->addView(headerRule);

    // ── Scope rows (series only) ────────────────────────────────────────
    if (series) {
        struct ScopeSpec { const char* sub; };
        for (size_t i = 0; i < st->tmpl.options.size(); i++) {
            const TemplateOption& opt = st->tmpl.options[i];
            // Subtitles per the design; unknown titles just get no sub.
            std::string sub;
            if (opt.title == "This Episode") sub = "Record only this airing";
            else if (opt.title == "All Episodes") sub = "Series recording on this channel";

            auto* row = new brls::Box();
            row->setAxis(brls::Axis::ROW);
            row->setAlignItems(brls::AlignItems::CENTER);
            row->setHeight(46.0f);
            row->setPadding(0.0f, 18.0f, 0.0f, 18.0f);
            row->setFocusable(true);

            // Selection mark: gold dot on the chosen scope.
            auto* mark = new brls::Rectangle();
            mark->setWidth(7.0f);
            mark->setHeight(7.0f);
            mark->setCornerRadius(3.5f);
            mark->setColor(tok::gold());
            mark->setMarginRight(10.0f);
            row->addView(mark);

            auto* txt = new brls::Box();
            txt->setAxis(brls::Axis::COLUMN);
            auto* t = makeLabel(opt.title, 13.5f, tok::text());
            txt->addView(t);
            if (!sub.empty()) txt->addView(makeLabel(sub, 10.5f, tok::muted2()));
            row->addView(txt);

            st->scopeTitles.push_back(t);
            st->scopeMarks.push_back(mark);

            const int idx = (int)i;
            auto stW = st;
            row->registerClickAction([stW, idx](brls::View*) {
                stW->scopeIndex = idx;
                if (stW->refresh) stW->refresh();
                return true;
            });
            row->addGestureRecognizer(new brls::TapGestureRecognizer(row));
            panel->addView(row);

            if (i + 1 < st->tmpl.options.size()) {
                auto* sep = new brls::Box();
                sep->setHeight(1.0f);
                sep->setAlignSelf(brls::AlignSelf::STRETCH);
                sep->setBackgroundColor(tok::rowSep());
                panel->addView(sep);
            }
        }
        auto* rule = new brls::Box();
        rule->setHeight(1.0f);
        rule->setAlignSelf(brls::AlignSelf::STRETCH);
        rule->setBackgroundColor(tok::hairline());
        panel->addView(rule);
    }

    // ── Settings section ────────────────────────────────────────────────
    auto* settingsBox = new brls::Box();
    settingsBox->setAxis(brls::Axis::COLUMN);
    settingsBox->setPadding(10.0f, 8.0f, 6.0f, 8.0f);

    auto* labelRow = new brls::Box();
    labelRow->setAxis(brls::Axis::ROW);
    labelRow->setAlignItems(brls::AlignItems::CENTER);
    labelRow->setPadding(0.0f, 10.0f, 6.0f, 10.0f);
    // "for this show" reads wrong for a movie (scope rows hidden).
    labelRow->addView(makeLabel(series ? "SETTINGS FOR THIS SHOW"
                                       : "SETTINGS FOR THIS RECORDING",
                                10.0f, tok::muted2()));
    auto* lspacer = new brls::Box();
    lspacer->setGrow(1.0f);
    labelRow->addView(lspacer);

    auto* reset = makeLabel("Reset to defaults", 10.5f, tok::gold());
    auto* resetBox = new brls::Box();
    resetBox->setAxis(brls::Axis::ROW);
    resetBox->setFocusable(true);
    resetBox->setCornerRadius(6.0f);
    resetBox->setHighlightCornerRadius(6.0f);
    resetBox->setPadding(3.0f, 6.0f, 3.0f, 6.0f);
    resetBox->addView(reset);
    {
        auto stW = st;
        resetBox->registerClickAction([stW](brls::View*) {
            stW->prefs = stW->initial;
            if (stW->refresh) stW->refresh();
            return true;
        });
        resetBox->addGestureRecognizer(new brls::TapGestureRecognizer(resetBox));
    }
    labelRow->addView(resetBox);
    settingsBox->addView(labelRow);

    {
        auto stW = st;
        settingsBox->addView(makeSettingRow("Start early", &st->startRow, [stW](int d) {
            int i = indexOfOffset(stW->prefs.startOffsetMin) + d;
            if (i < 0) i = 0;
            if (i >= (int)kOffsetMinutes.size()) i = (int)kOffsetMinutes.size() - 1;
            stW->prefs.startOffsetMin = kOffsetMinutes[i];
            if (stW->refresh) stW->refresh();
        }));
        settingsBox->addView(makeSettingRow("End late", &st->endRow, [stW](int d) {
            int i = indexOfOffset(stW->prefs.endOffsetMin) + d;
            if (i < 0) i = 0;
            if (i >= (int)kOffsetMinutes.size()) i = (int)kOffsetMinutes.size() - 1;
            stW->prefs.endOffsetMin = kOffsetMinutes[i];
            if (stW->refresh) stW->refresh();
        }));
        settingsBox->addView(makeSettingRow("Min video quality", &st->qualityRow,
            [stW](int d) {
                int i = indexOfQuality(stW->prefs.minVideoQuality) + d;
                const int n = (int)kMinQualities.size();
                i = ((i % n) + n) % n;   // wrap: it's a short cycle, not a range
                stW->prefs.minVideoQuality = kMinQualities[i];
                if (stW->refresh) stW->refresh();
            },
            [stW]() {
                auto* dd = new brls::Dropdown("Minimum recording quality",
                    kMinQualityLabels,
                    [stW](int idx) {
                        stW->prefs.minVideoQuality = kMinQualities[(size_t)idx];
                        if (stW->refresh) stW->refresh();
                    },
                    indexOfQuality(stW->prefs.minVideoQuality));
                brls::Application::pushActivity(new brls::Activity(dd));
            }));
        settingsBox->addView(makeSettingRow("Records to", &st->sectionRow,
            [stW](int d) {
                // Left/right still cycles: template default → each library.
                const int n = (int)stW->sections.size() + 1;
                int cur = 0;
                for (size_t i = 0; i < stW->sections.size(); i++)
                    if (stW->sections[i].key == stW->prefs.targetSectionId) { cur = (int)i + 1; break; }
                cur = ((cur + d) % n + n) % n;
                stW->prefs.targetSectionId = (cur == 0) ? "" : stW->sections[cur - 1].key;
                if (stW->refresh) stW->refresh();
            },
            [stW]() {
                std::vector<std::string> options;
                options.reserve(stW->sections.size() + 1);
                options.push_back("Server default");
                for (const auto& sec : stW->sections) options.push_back(sec.title);
                int cur = 0;
                for (size_t i = 0; i < stW->sections.size(); i++)
                    if (stW->sections[i].key == stW->prefs.targetSectionId) { cur = (int)i + 1; break; }
                auto* dd = new brls::Dropdown("Records to", options,
                    [stW](int idx) {
                        stW->prefs.targetSectionId =
                            (idx == 0) ? "" : stW->sections[(size_t)idx - 1].key;
                        if (stW->refresh) stW->refresh();
                    },
                    cur);
                brls::Application::pushActivity(new brls::Activity(dd));
            }));
    }

    auto* caption = makeLabel(series
        ? "Applies to this show only — app defaults stay unchanged (Settings → DVR)"
        : "Applies to this recording only — app defaults stay unchanged (Settings → DVR)",
        10.0f, tok::disabled(), false);
    caption->setMarginTop(4.0f);
    caption->setMarginLeft(10.0f);
    caption->setMarginRight(10.0f);
    settingsBox->addView(caption);
    panel->addView(settingsBox);

    // ── Footer ──────────────────────────────────────────────────────────
    auto* footerRule = new brls::Box();
    footerRule->setHeight(1.0f);
    footerRule->setAlignSelf(brls::AlignSelf::STRETCH);
    footerRule->setBackgroundColor(tok::hairline());
    panel->addView(footerRule);

    auto* footer = new brls::Box();
    footer->setAxis(brls::Axis::ROW);
    footer->setAlignItems(brls::AlignItems::CENTER);
    footer->setPadding(12.0f, 18.0f, 14.0f, 18.0f);

    const std::string title = item.title;
    auto stW = st;
    auto* schedule = makeButton("Schedule", BtnStyle::Gold, [stW, title, onScheduled]() {
        const TemplateOption opt = stW->tmpl.options[(size_t)stW->scopeIndex];
        const RecordingPrefs prefs = stW->prefs;
        // This Episode records once; All Episodes is a standing series
        // subscription. The template's type field is the authoritative
        // signal — type 2 is a show/series subscription (spec example:
        // "All Episodes" type=2, "This Episode" type=4, movies type=1) —
        // where row order would be a guess.
        const bool oneShot = (opt.type != "2");
        brls::Application::popActivity(brls::TransitionAnimation::FADE,
            [opt, prefs, oneShot, title, onScheduled]() {
                asyncRun([opt, prefs, oneShot, title, onScheduled]() {
                    const bool ok = postRecordingSubscription(opt, prefs, oneShot);
                    brls::sync([ok, title, onScheduled]() {
                        showOutcome(ok, title, onScheduled);
                    });
                });
            });
    });
    schedule->setGrow(1.0f);
    footer->addView(schedule);

    auto* cancel = makeButton("Cancel", BtnStyle::Ghost, []() {
        brls::Application::popActivity();
    });
    cancel->setWidth(96.0f);
    cancel->setMarginLeft(10.0f);
    footer->addView(cancel);
    panel->addView(footer);

    // ── Refresh: values, changed-dots, scope tint ───────────────────────
    st->refresh = [stW]() {
        auto setRow = [](SettingRowState& row, const std::string& text, bool changed) {
            if (row.value) row.value->setText(text);
            if (row.dot)
                row.dot->setVisibility(changed ? brls::Visibility::VISIBLE
                                               : brls::Visibility::INVISIBLE);
        };
        setRow(stW->startRow, std::to_string(stW->prefs.startOffsetMin) + " min",
               stW->prefs.startOffsetMin != stW->initial.startOffsetMin);
        setRow(stW->endRow, std::to_string(stW->prefs.endOffsetMin) + " min",
               stW->prefs.endOffsetMin != stW->initial.endOffsetMin);
        setRow(stW->qualityRow, kMinQualityLabels[(size_t)indexOfQuality(stW->prefs.minVideoQuality)],
               stW->prefs.minVideoQuality != stW->initial.minVideoQuality);

        std::string sectionLabel = "Server default";
        for (const auto& s : stW->sections)
            if (s.key == stW->prefs.targetSectionId) { sectionLabel = s.title; break; }
        setRow(stW->sectionRow, sectionLabel,
               stW->prefs.targetSectionId != stW->initial.targetSectionId);

        for (size_t i = 0; i < stW->scopeTitles.size(); i++) {
            const bool sel = ((int)i == stW->scopeIndex);
            stW->scopeTitles[i]->setTextColor(sel ? tok::gold() : tok::text());
            stW->scopeMarks[i]->setVisibility(sel ? brls::Visibility::VISIBLE
                                                  : brls::Visibility::INVISIBLE);
        }
    };
    st->refresh();

    scrim->addView(panel);
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) { brls::Application::popActivity(); return true; });
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    // Keep the dialog inside the viewport on 544-tall screens: the panel
    // is near full height there by design.
    (void)screenH;

    brls::Application::pushActivity(new OverlayActivity(scrim));
    brls::Application::giveFocus(schedule);
}

}  // namespace

// ── Public API ───────────────────────────────────────────────────────────

bool canRecordAiring(int64_t airStartAt) {
    if (airStartAt <= 0) return true;                       // unknown window: let the server decide
    if ((int64_t)time(nullptr) < airStartAt) return true;   // hasn't started
    return Application::getInstance().getSettings().dvrRecordPartials;
}

void tuneLiveTVProgram(const MediaItem& item) {
    const std::string channelKey = item.liveChannelKey;
    const std::string programKey = item.key;
    const std::string playerTitle =
        item.liveChannelTitle.empty() ? item.title
                                      : item.liveChannelTitle + " - " + item.title;

    asyncRun([channelKey, programKey, playerTitle]() {
        PlexClient& client = PlexClient::getInstance();
        std::string streamUrl, liveSessionUuid;
        if (client.tuneLiveTVChannel(channelKey, streamUrl, liveSessionUuid, programKey)) {
            brls::sync([streamUrl, liveSessionUuid, playerTitle]() {
                Application::getInstance().pushLiveTVPlayerActivity(streamUrl, playerTitle,
                                                                    liveSessionUuid);
            });
        } else {
            brls::Logger::error("tuneLiveTVProgram: failed to tune {}", playerTitle);
            brls::sync([playerTitle]() {
                brls::Dialog* dialog = new brls::Dialog("Failed to tune: " + playerTitle);
                dialog->addButton("OK", []() {});
                dialog->open();
            });
        }
    });
}

void showRecordOptions(const MediaItem& item, std::function<void(bool)> onScheduled) {
    if (item.ratingKey.empty()) {
        showOutcome(false, item.title, onScheduled);
        return;
    }

    // The template decides the dialog's shape (series vs one-off), so it
    // is fetched before the dialog opens; the eligible "Records to"
    // libraries ride along on the same worker (fetchLibrarySections is
    // served from the HTTP cache in the common case).
    MediaItem captured = item;
    asyncRun([captured, onScheduled]() {
        RecordingTemplate tmpl;
        const bool ok = fetchRecordingTemplate(captured.ratingKey, tmpl);

        // Only libraries of the recording's own type: a movie cannot land
        // in a TV Shows library. The item's mediaType decides; the
        // template's subscription type (1 = movie) breaks the tie when the
        // item does not carry one.
        std::string wantType = "show";
        if (captured.mediaType == MediaType::MOVIE) wantType = "movie";
        else if (captured.mediaType == MediaType::UNKNOWN) {
            for (const auto& o : tmpl.options)
                if (o.type == "1") { wantType = "movie"; break; }
        }

        std::vector<LibrarySection> all, eligible;
        PlexClient::getInstance().fetchLibrarySections(all);
        for (const auto& s : all)
            if (s.type == wantType) eligible.push_back(s);

        brls::sync([captured, tmpl, eligible, ok, onScheduled]() {
            if (!ok) {
                showOutcome(false, captured.title, onScheduled);
                return;
            }
            openRecordOptionsDialog(captured, tmpl, eligible, onScheduled);
        });
    });
}

void showLiveTVProgramMenu(const MediaItem& item, std::function<void(bool)> onScheduled) {
    const int64_t now = (int64_t)time(nullptr);
    const float progress = airProgress(item.airStartAt, item.airEndAt, now);
    const bool onAir = !item.liveChannelKey.empty() && progress >= 0.0f;
    const bool recordable = canRecordAiring(item.airStartAt);

    const float screenW = platform::viewportWidth();
    float panelW = 432.0f;
    if (panelW + 80.0f > screenW) panelW = screenW - 80.0f;

    // ── Scrim + panel ───────────────────────────────────────────────────
    auto* scrim = new brls::Box();
    scrim->setAxis(brls::Axis::COLUMN);
    scrim->setWidthPercentage(100.0f);
    scrim->setHeightPercentage(100.0f);
    scrim->setJustifyContent(brls::JustifyContent::CENTER);
    scrim->setAlignItems(brls::AlignItems::CENTER);
    scrim->setBackgroundColor(tok::scrim());

    auto* panel = new FlagBox();
    panel->setAxis(brls::Axis::COLUMN);
    panel->setWidth(panelW);
    panel->setBackgroundColor(tok::panel());
    panel->setBorderColor(tok::panelLine());
    panel->setBorderThickness(1.0f);
    panel->setCornerRadius(16.0f);
    panel->setShadowType(brls::ShadowType::GENERIC);

    // ── Header: poster + meta column ────────────────────────────────────
    auto* head = new brls::Box();
    head->setAxis(brls::Axis::ROW);
    head->setPadding(16.0f, 18.0f, 12.0f, 18.0f);

    auto* poster = new brls::Box();
    poster->setWidth(74.0f);
    poster->setHeight(104.0f);
    poster->setCornerRadius(8.0f);
    poster->setBackgroundColor(tok::inputBg());
    poster->setMarginRight(14.0f);
    auto* posterImg = new brls::Image();
    posterImg->setWidth(74.0f);
    posterImg->setHeight(104.0f);
    posterImg->setCornerRadius(8.0f);
    posterImg->setScalingType(brls::ImageScalingType::FILL);
    posterImg->setVisibility(brls::Visibility::INVISIBLE);
    poster->addView(posterImg);
    {
        std::string art = item.grandparentThumb.empty() ? item.thumb : item.grandparentThumb;
        if (!art.empty()) {
            std::string url = PlexClient::getInstance().getThumbnailUrl(art, 148, 208);
            ImageLoader::loadAsync(url, [](brls::Image* img) {
                if (img) img->setVisibility(brls::Visibility::VISIBLE);
            }, posterImg, panel->alive);
        }
    }
    head->addView(poster);

    auto* meta = new brls::Box();
    meta->setAxis(brls::Axis::COLUMN);
    meta->setGrow(1.0f);
    meta->setShrink(1.0f);

    // Chip line: LIVE (on air only) + callsign + CH n.
    auto* chips = new brls::Box();
    chips->setAxis(brls::Axis::ROW);
    chips->setAlignItems(brls::AlignItems::CENTER);
    chips->setMarginBottom(5.0f);
    if (onAir) {
        auto* live = new brls::Box();
        live->setAxis(brls::Axis::ROW);
        live->setAlignItems(brls::AlignItems::CENTER);
        live->setHeight(18.0f);
        live->setPadding(0.0f, 7.0f, 0.0f, 7.0f);
        live->setCornerRadius(9.0f);
        live->setBackgroundColor(tok::liveRed());
        auto* dot = new brls::Rectangle();
        dot->setWidth(5.0f);
        dot->setHeight(5.0f);
        dot->setCornerRadius(2.5f);
        dot->setColor(tok::text());
        dot->setMarginRight(5.0f);
        live->addView(dot);
        live->addView(makeLabel("LIVE", 9.5f, tok::text()));
        live->setMarginRight(9.0f);
        chips->addView(live);
    }
    {
        std::string callsign, number;
        splitChannelTitle(item.liveChannelTitle, callsign, number);
        if (!callsign.empty()) {
            auto* cs = makeLabel(callsign, 11.5f, tok::muted());
            cs->setMarginRight(8.0f);
            chips->addView(cs);
        }
        if (!number.empty())
            chips->addView(makeLabel("CH " + number, 11.5f, tok::muted2()));
    }
    meta->addView(chips);

    auto* title = makeLabel(item.title, 16.0f, tok::text());
    meta->addView(title);

    // Summary, clamped — the panel has no scroll, so long descriptions
    // are cut at a sentence-ish length rather than wrapped forever.
    if (!item.summary.empty()) {
        std::string sum = item.summary;
        if (sum.size() > 140) sum = sum.substr(0, 137) + "...";
        auto* s = makeLabel(sum, 11.5f, tok::muted(), false);
        s->setMarginTop(3.0f);
        meta->addView(s);
    }

    // Air-window progress + captions.
    if (item.airStartAt > 0 && item.airEndAt > item.airStartAt) {
        const float barW = panelW - 36.0f - 74.0f - 14.0f;
        auto* bar = makeProgressBar(barW, progress);
        bar->setMarginTop(8.0f);
        meta->addView(bar);

        auto* times = new brls::Box();
        times->setAxis(brls::Axis::ROW);
        times->setAlignItems(brls::AlignItems::CENTER);
        times->setMarginTop(4.0f);
        times->addView(makeLabel(airWindowLabel(item.airStartAt, item.airEndAt),
                                 10.5f, tok::muted2()));
        auto* tsp = new brls::Box();
        tsp->setGrow(1.0f);
        times->addView(tsp);
        if (onAir)
            times->addView(makeLabel(minutesLeftLabel(item.airEndAt, now), 10.5f, tok::muted2()));
        else if (item.airStartAt > now)
            times->addView(makeLabel(startsInLabel(item.airStartAt, now), 10.5f, tok::muted2()));
        meta->addView(times);
    }
    head->addView(meta);
    panel->addView(head);

    // ── Warning panel (not recordable only) ─────────────────────────────
    if (!recordable) {
        auto* warn = new brls::Box();
        warn->setAxis(brls::Axis::ROW);
        warn->setAlignItems(brls::AlignItems::CENTER);
        warn->setMarginLeft(18.0f);
        warn->setMarginRight(18.0f);
        warn->setMarginBottom(12.0f);
        warn->setPadding(9.0f, 11.0f, 9.0f, 11.0f);
        warn->setCornerRadius(9.0f);
        warn->setBackgroundColor(tok::warnBg());
        warn->setBorderColor(tok::warnBorder());
        warn->setBorderThickness(1.0f);
        auto* w = makeLabel(
            "Already started — recording now would only keep the rest, and "
            "Keep Partial Recordings is off. Turn it on in Settings → DVR to "
            "record from here.",
            11.0f, tok::warnText(), false);
        w->setShrink(1.0f);
        warn->addView(w);
        panel->addView(warn);
    }

    // ── Buttons ─────────────────────────────────────────────────────────
    auto* btnRow = new brls::Box();
    btnRow->setAxis(brls::Axis::ROW);
    btnRow->setAlignItems(brls::AlignItems::CENTER);
    btnRow->setPadding(2.0f, 18.0f, 16.0f, 18.0f);

    brls::Box* watch = nullptr;
    if (onAir) {
        MediaItem captured = item;
        watch = makeButton("\xE2\x96\xB6  Watch Now", BtnStyle::Gold, [captured]() {
            brls::Application::popActivity(brls::TransitionAnimation::FADE,
                [captured]() { tuneLiveTVProgram(captured); });
        });
        watch->setGrow(1.0f);
        btnRow->addView(watch);
    }

    // The action itself is gated on recordable, so the disabled state is
    // inert for touch as well as controller — borealis has no API to strip
    // a registered gesture afterwards.
    MediaItem recCaptured = item;
    auto* record = makeButton("\xE2\x97\x8F  Record", BtnStyle::Gray,
        [recCaptured, onScheduled, recordable]() {
            if (!recordable) return;
            brls::Application::popActivity(brls::TransitionAnimation::FADE,
                [recCaptured, onScheduled]() { showRecordOptions(recCaptured, onScheduled); });
        });
    record->setGrow(1.0f);
    if (watch) record->setMarginLeft(10.0f);
    if (!recordable) {
        // Disabled, not hidden: the warning above explains why, and a
        // vanished button reads as a bug. 45% opacity per the design.
        record->setAlpha(0.45f);
        record->setFocusable(false);
    }
    btnRow->addView(record);

    auto* cancelBtn = makeButton("Cancel", BtnStyle::Ghost, []() {
        brls::Application::popActivity();
    });
    cancelBtn->setWidth(92.0f);
    cancelBtn->setMarginLeft(10.0f);
    btnRow->addView(cancelBtn);
    panel->addView(btnRow);

    scrim->addView(panel);
    scrim->registerAction("Back", brls::ControllerButton::BUTTON_B,
        [](brls::View*) { brls::Application::popActivity(); return true; });
    scrim->addGestureRecognizer(new brls::TapGestureRecognizer(scrim,
        []() { brls::Application::popActivity(); }));

    brls::Application::pushActivity(new OverlayActivity(scrim));
    if (watch) brls::Application::giveFocus(watch);
    else if (recordable) brls::Application::giveFocus(record);
    else brls::Application::giveFocus(cancelBtn);
}

}  // namespace vitaplex
