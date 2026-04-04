#pragma once

/**
 * Android APK asset extraction.
 *
 * On Android, borealis loads resources via standard fopen() / tinyxml2::LoadFile()
 * which cannot read from APK assets directly. This module extracts the bundled
 * assets/resources/ directory from the APK to the app's internal storage so that
 * normal file I/O works.
 *
 * Call extractAndroidAssets() BEFORE brls::Application::init().
 */

#if defined(__ANDROID__)

/**
 * Extract APK assets/resources/ to internal storage and chdir there.
 * After this call, fopen("resources/...") will find the extracted files.
 */
void extractAndroidAssets();

#endif
