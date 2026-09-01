/**
 * VitaPlex - Android TV home-screen rows
 *
 * The TV home screen is assembled from app-published channels and a system
 * "Watch Next" row, so an app that publishes nothing is a tile you have to open
 * before it shows you anything. VitaPlex already knows what you were watching
 * and what arrived recently; this hands both to the TV provider.
 *
 * Android only. Every other port links no-ops, so callers need no #ifdef, and a
 * phone (no TV provider installed) is a no-op at runtime too.
 */

#pragma once

#include <vector>

#include "app/plex_client.hpp"

namespace vitaplex {
namespace tvhome {

/**
 * Publish the Continue Watching items to the system Watch Next row, in the
 * order given. Items with a viewOffset are published as "continue" with their
 * resume position; the rest as "next".
 */
void publishContinueWatching(const std::vector<MediaItem>& items);

/**
 * Publish the Recently Added items to the app's own home-screen channel. The
 * channel is created on first call and offered to the user once; after that it
 * is only refreshed, so a row the user removed stays removed.
 */
void publishRecentlyAdded(const std::vector<MediaItem>& items);

} // namespace tvhome
} // namespace vitaplex
