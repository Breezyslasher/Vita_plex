/**
 * VitaPlex - Live TV airing windows
 *
 * Live programmes carry their broadcast window in Media[].beginsAt /
 * Media[].endsAt (unix seconds), which is what lets a rail say how far
 * through a broadcast is and when it ends. Header-only: the rails and the
 * cells both need it, and it is a handful of arithmetic either way.
 */

#pragma once

#include <cstdint>
#include <cstdio>
#include <ctime>
#include <string>

namespace vitaplex {

// Local wall-clock time, "8:00 PM". Hour is not zero-padded, matching how
// the Live TV guide prints its times.
inline std::string airClockLabel(int64_t unixSeconds) {
    time_t t = (time_t)unixSeconds;
    struct tm tmv {};
#ifdef _WIN32
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    int hour12 = tmv.tm_hour % 12;
    if (hour12 == 0) hour12 = 12;
    char buf[16];
    snprintf(buf, sizeof(buf), "%d:%02d %s", hour12, tmv.tm_min,
             tmv.tm_hour < 12 ? "AM" : "PM");
    return std::string(buf);
}

// "8:00 – 8:30 PM" for a broadcast window, collapsing the meridiem when
// both ends share it. Empty when either end is unset.
inline std::string airWindowLabel(int64_t startAt, int64_t endAt) {
    if (startAt <= 0 || endAt <= startAt) return {};

    std::string from = airClockLabel(startAt);
    std::string to   = airClockLabel(endAt);
    // Both labels end in " AM" / " PM"; drop the first when they match so
    // the common case reads "8:00 – 8:30 PM" rather than repeating it.
    if (from.size() > 3 && to.size() > 3 &&
        from.compare(from.size() - 2, 2, to, to.size() - 2, 2) == 0) {
        from.resize(from.size() - 3);
    }
    return from + " \xE2\x80\x93 " + to;   // en dash
}

// How far through the window `now` is, 0..1. Negative when the window is
// unset or `now` sits outside it, so callers can treat "not airing" and
// "just started" differently.
inline float airProgress(int64_t startAt, int64_t endAt, int64_t now = 0) {
    if (startAt <= 0 || endAt <= startAt) return -1.0f;
    if (now <= 0) now = (int64_t)time(nullptr);
    if (now < startAt || now > endAt) return -1.0f;
    return (float)(now - startAt) / (float)(endAt - startAt);
}

}  // namespace vitaplex
