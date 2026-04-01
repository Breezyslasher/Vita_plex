#include "platform/android_assets.hpp"

#if defined(__ANDROID__)

void extractAndroidAssets()
{
    // No-op fallback.
    // Android packaging currently expects resources to be available via the
    // app's configured file paths without pre-extraction at startup.
}

#endif
