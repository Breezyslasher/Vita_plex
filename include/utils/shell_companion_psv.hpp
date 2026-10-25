#pragma once

// VitaPlex and vita-shell-companion's SceShell plugin (lib/vita-shell-
// companion; its docs/PROTOCOL.md). The plugin is what carries VitaPlex's
// work on in SceShell, which no app or game closes: the quick menu's Music
// row, the queue played on once VitaPlex has gone (background_audio_psv.cpp),
// the downloads carried on (DownloadsManager::handOverToShell), the progress
// notification (shell_integration_psv.cpp) and the updates installed
// (app_update.cpp). Every part of VitaPlex reaches it through here, so that it
// is set up once and opened once. Vita only.

#include <string>

namespace vitaplex {
namespace shellcomp {

// The client set up as VitaPlex: its title, its data folder (where the
// plugin's files for it go) and the features it asks for. Called by all of
// these before anything else; once is enough, more are harmless. Any thread.
void init();

// The plugin found in SceShell, or VitaPlex's own loaded into it, and its
// pipes opened; what happened logged under `who`, with the plugin's version
// when it answers. true when open. Waits for the plugin's answer (half a
// second at most) and, the first time in a boot, its load: off the UI thread
// where it can be.
bool open(const char* who);

// A file in VitaPlex's data folder, as the plugin names them (SC_QUEUE,
// SC_DOWNLOADS_STATUS ...).
std::string path(const char* name);

}  // namespace shellcomp
}  // namespace vitaplex
