/**
 * VitaPlex - Live TV actions
 *
 * What you can do with a broadcast, shared by the guide, the Home rails
 * and search. The guide had all of this first; the rails and search reach
 * it with a MediaItem rather than a GuideProgram, and a live programme's
 * ratingKey is an EPG key that /library/metadata 404s on, so none of them
 * can fall back on the normal detail view.
 *
 * Two custom dialogs (design_handoff_epg_menu): the program card —
 * art, LIVE chip, air-window progress, Watch Now / Record — and the
 * record-options dialog — This Episode / All Episodes plus per-recording
 * settings that override the app's DVR defaults for one subscription
 * without touching them.
 */

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "app/plex_client.hpp"

namespace vitaplex {

// Tune the channel a programme is airing on and push the live player.
void tuneLiveTVProgram(const MediaItem& item);

// Whether scheduling a recording for a broadcast starting at airStartAt
// would actually keep anything. A programme already under way can only be
// recorded from here on, so the result is a partial recording — which the
// server discards unless "Keep Partial Recordings" is on. Programmes that
// have not started yet are always recordable.
bool canRecordAiring(int64_t airStartAt);

// The program card: art, LIVE chip, channel, title, air-window progress,
// then Watch Now / Record / Cancel. Watch Now appears only while the
// programme is on the air and the channel it is on is known; Record is
// disabled (not hidden) with a warning panel when canRecordAiring() says
// nothing would be kept. Record opens the record-options dialog.
// onScheduled fires with the outcome if a recording gets scheduled.
void showLiveTVProgramMenu(const MediaItem& item,
                           std::function<void(bool)> onScheduled = {});

// The record-options dialog: This Episode / All Episodes (from the
// server's subscription template; hidden when the template offers a
// single option, e.g. movies) plus per-recording settings initialised
// from the app's DVR defaults. Edits apply to this subscription only —
// AppSettings is never written. Schedule posts the subscription.
void showRecordOptions(const MediaItem& item,
                       std::function<void(bool)> onScheduled = {});

}  // namespace vitaplex
