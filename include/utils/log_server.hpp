#pragma once

/**
 * VitaPlex - the log, served over the local network
 *
 * For devices with no way to send a file: Android TVs whose share sheet has
 * nothing to share to (a Sony on Android 9), and Apple TV. start() opens a
 * small HTTP server on the device's Wi-Fi / Ethernet address and returns the
 * address to open in a browser on a phone or computer on the same network.
 * It serves this run's log and the previous run's, only under a random path,
 * and stops by itself after a while or with stop().
 *
 * POSIX only (Android, iOS, tvOS): available() is false elsewhere.
 */

#include <string>

namespace vitaplex {
namespace logserver {

bool available();

// Starts (or keeps) the server. Returns the address to open, e.g.
// "http://192.168.1.20:8765/3f9a…/", or "" if it could not start (no network
// address, no free port).
std::string start(int minutes = 15);

void stop();
bool running();

}  // namespace logserver
}  // namespace vitaplex
