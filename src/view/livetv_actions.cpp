/**
 * VitaPlex - Live TV actions implementation
 *
 * Lifted from LiveTVTab::scheduleRecording, which is the version that has
 * been working against a real DVR. Kept as one implementation rather than
 * copied so the guide, the Home rails and search cannot drift apart.
 */

#include "view/livetv_actions.hpp"

#include <borealis.hpp>

#include "app/application.hpp"
#include "utils/air_time.hpp"
#include "utils/async.hpp"
#include "utils/http_client.hpp"

namespace vitaplex {

namespace {

void recordingFailed(const std::string& title, std::function<void(bool)> onDone) {
    brls::sync([title, onDone]() {
        brls::Dialog* dialog = new brls::Dialog("Failed to schedule recording: " + title);
        dialog->addButton("OK", []() {});
        dialog->open();
        if (onDone) onDone(false);
    });
}

}  // namespace

bool canRecordAiring(int64_t airStartAt) {
    if (airStartAt <= 0) return true;                  // unknown window: let the server decide
    if ((int64_t)time(nullptr) < airStartAt) return true;   // hasn't started
    return Application::getInstance().getSettings().dvrRecordPartials;
}

void scheduleLiveTVRecording(const std::string& guid, const std::string& title,
                             std::function<void(bool)> onDone) {
    if (guid.empty()) {
        brls::Logger::error("scheduleLiveTVRecording: missing programme guid");
        recordingFailed(title, std::move(onDone));
        return;
    }

    asyncRun([guid, title, onDone]() {
        PlexClient& client = PlexClient::getInstance();
        HttpClient httpClient;

        // GET /media/subscriptions/template?guid=<programGuid> returns the
        // pre-encoded querystring (hints[*] and params[*]) the server
        // expects, plus the recommended type and target library section.
        // We paste it verbatim and only layer recording prefs on top.
        std::string tmplUrl =
            client.buildApiUrlPublic("/media/subscriptions/template?guid=" + guid);

        HttpRequest tmplReq;
        tmplReq.url = tmplUrl;
        tmplReq.method = "GET";
        tmplReq.headers["Accept"] = "application/json";
        tmplReq.timeout = 15;

        brls::Logger::debug("scheduleLiveTVRecording: template URL: {}",
                            redactTokensInUrl(tmplUrl));
        HttpResponse tmplResp = httpClient.request(tmplReq);
        if (tmplResp.statusCode != 200 || tmplResp.body.empty()) {
            brls::Logger::error("scheduleLiveTVRecording: template failed ({}): {}",
                                tmplResp.statusCode,
                                tmplResp.body.empty() ? "(empty)" : tmplResp.body.substr(0, 300));
            recordingFailed(title, onDone);
            return;
        }

        // The template offers several subscriptions ("This Episode", "All
        // Episodes"); take the one the server marked selected, falling back
        // to the first.
        const std::string& body = tmplResp.body;
        size_t pickAt = std::string::npos;
        {
            size_t scan = 0;
            const std::string sel = "\"selected\":true";
            while (true) {
                size_t at = body.find(sel, scan);
                if (at == std::string::npos) break;
                int depth = 0;
                for (size_t i = at; i > 0; i--) {
                    if (body[i] == '}') depth++;
                    else if (body[i] == '{') {
                        if (depth == 0) { pickAt = i; break; }
                        depth--;
                    }
                }
                if (pickAt != std::string::npos) break;
                scan = at + sel.length();
            }
        }
        if (pickAt == std::string::npos) {
            size_t msArr = body.find("\"MediaSubscription\"");
            if (msArr != std::string::npos) pickAt = body.find('{', msArr);
        }
        if (pickAt == std::string::npos) {
            brls::Logger::error("scheduleLiveTVRecording: template parse failed: {}",
                                body.substr(0, 300));
            recordingFailed(title, onDone);
            return;
        }

        size_t depth = 0;
        size_t objEnd = pickAt;
        for (; objEnd < body.length(); objEnd++) {
            if (body[objEnd] == '{') depth++;
            else if (body[objEnd] == '}') {
                if (--depth == 0) { objEnd++; break; }
            }
        }
        std::string ms = body.substr(pickAt, objEnd - pickAt);

        std::string parameters    = client.extractJsonValuePublic(ms, "parameters");
        std::string typeStr       = client.extractJsonValuePublic(ms, "type");
        std::string targetSection = client.extractJsonValuePublic(ms, "targetLibrarySectionID");

        // User-configured default DVR library wins over the template's
        // recommendation. Lets the user route every recording to one
        // section ("DVR TV Shows") instead of whatever Plex picked for
        // each individual program.
        const std::string& userTarget =
            Application::getInstance().getSettings().defaultDvrSectionId;
        if (!userTarget.empty()) {
            brls::Logger::debug("scheduleLiveTVRecording: overriding targetLibrarySectionID "
                                "{} → {} (user default)",
                                targetSection.empty() ? "(none)" : targetSection, userTarget);
            targetSection = userTarget;
        }

        if (parameters.empty() || typeStr.empty()) {
            brls::Logger::error("scheduleLiveTVRecording: template missing required fields "
                                "(parameters={}, type={})",
                                parameters.empty() ? "(empty)" : "ok",
                                typeStr.empty() ? "(empty)" : typeStr);
            recordingFailed(title, onDone);
            return;
        }

        const AppSettings& settings = Application::getInstance().getSettings();

        std::string post = client.buildApiUrlPublic("/media/subscriptions");
        post += "&" + parameters;
        post += "&type=" + typeStr;
        if (!targetSection.empty()) post += "&targetLibrarySectionID=" + targetSection;
        post += "&includeGrabs=1";
        post += "&prefs[oneShot]=true";
        post += std::string("&prefs[recordPartials]=") + (settings.dvrRecordPartials ? "true" : "false");
        post += "&prefs[minVideoQuality]=" + std::to_string(settings.dvrMinVideoQuality);
        post += "&prefs[startOffsetMinutes]=" + std::to_string(settings.dvrStartOffsetMinutes);
        post += "&prefs[endOffsetMinutes]=" + std::to_string(settings.dvrEndOffsetMinutes);

        HttpRequest req;
        req.url = post;
        req.method = "POST";
        req.headers["Accept"] = "application/json";
        req.timeout = 15;

        brls::Logger::debug("scheduleLiveTVRecording: POST {}", redactTokensInUrl(post));
        HttpResponse resp = httpClient.request(req);

        const bool success = (resp.statusCode == 200 || resp.statusCode == 201);
        brls::Logger::debug("scheduleLiveTVRecording: response {} ({} bytes): {}",
                            resp.statusCode, resp.body.length(), resp.body.substr(0, 500));

        brls::sync([success, title, onDone]() {
            brls::Dialog* dialog = new brls::Dialog(
                success ? "Recording scheduled: " + title
                        : "Failed to schedule recording: " + title);
            dialog->addButton("OK", []() {});
            dialog->open();
            if (onDone) onDone(success);
        });
    });
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

void showLiveTVProgramMenu(const MediaItem& item) {
    // Everything on the On Now rails is already under way, so recording one
    // can only capture the remainder. With "Keep Partial Recordings" off the
    // server throws that away, so say so rather than offering a button that
    // quietly does nothing.
    const bool recordable = canRecordAiring(item.airStartAt);
    const bool onAir = !item.liveChannelKey.empty() &&
                       airProgress(item.airStartAt, item.airEndAt) >= 0.0f;

    std::string message = item.title;
    const std::string window = airWindowLabel(item.airStartAt, item.airEndAt);
    if (!window.empty()) {
        message += "\n\n" + window;
        if (!item.liveChannelTitle.empty()) message += " on " + item.liveChannelTitle;
    }
    if (!item.summary.empty()) message += "\n\n" + item.summary;
    if (!recordable) {
        message += "\n\nAlready started — turn on Keep Partial Recordings in "
                   "Settings to record the rest of a programme.";
    }

    brls::Dialog* dialog = new brls::Dialog(message);

    // Watch Now only while it is actually on the air and we know where:
    // tuning a channel for a programme that starts in three hours would
    // hand the user whatever happens to be on instead.
    if (onAir) {
        MediaItem captured = item;
        dialog->addButton("Watch Now", [captured]() { tuneLiveTVProgram(captured); });
    }
    if (recordable) {
        const std::string guid = item.ratingKey;
        const std::string title = item.title;
        dialog->addButton("Record", [guid, title]() { scheduleLiveTVRecording(guid, title); });
    }
    dialog->addButton("Cancel", []() {});
    dialog->open();
}

}  // namespace vitaplex
