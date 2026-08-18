/**
 * VitaPlex - Live TV actions
 *
 * What you can do with a broadcast, shared by the guide, the Home rails
 * and search. The guide had all of this first; the rails and search reach
 * it with a MediaItem rather than a GuideProgram, and a live programme's
 * ratingKey is an EPG key that /library/metadata 404s on, so none of them
 * can fall back on the normal detail view.
 */

#pragma once

#include <functional>
#include <string>

#include "app/plex_client.hpp"

namespace vitaplex {

// Schedule a DVR recording. `guid` is the programme's EPG rating key,
// which /media/subscriptions/template takes verbatim; the template's
// pre-encoded parameters are posted back with the user's recording prefs
// layered on. Reports success through a dialog, then calls onDone.
void scheduleLiveTVRecording(const std::string& guid, const std::string& title,
                             std::function<void(bool)> onDone = {});

// Tune the channel a programme is airing on and push the live player.
void tuneLiveTVProgram(const MediaItem& item);

// Whether scheduling a recording for a broadcast starting at airStartAt
// would actually keep anything. A programme already under way can only be
// recorded from here on, so the result is a partial recording — which the
// server discards unless "Keep Partial Recordings" is on. Programmes that
// have not started yet are always recordable.
bool canRecordAiring(int64_t airStartAt);

// Watch Now / Record / Cancel for a live programme, the same choice the
// guide offers. Watch Now appears only while the programme is on the air
// and the channel it is on is known.
void showLiveTVProgramMenu(const MediaItem& item);

}  // namespace vitaplex
