#include "player/mpv_player.hpp"

#if defined(__ANDROID__)

#include <borealis.hpp>

namespace vitaplex {

MpvPlayer& MpvPlayer::getInstance() {
    static MpvPlayer instance;
    return instance;
}

MpvPlayer::~MpvPlayer() = default;

bool MpvPlayer::init() {
    brls::Logger::warning("MpvPlayer: video playback is disabled on Android build");
    m_errorMessage = "Video playback is disabled on Android";
    m_state = MpvPlayerState::ERROR;
    return false;
}

void MpvPlayer::shutdown() {
    m_mpv = nullptr;
    m_mpvRenderCtx = nullptr;
    m_state = MpvPlayerState::IDLE;
}

bool MpvPlayer::loadUrl(const std::string&, const std::string&) { return false; }
bool MpvPlayer::loadFile(const std::string&) { return false; }
void MpvPlayer::play() {}
void MpvPlayer::pause() {}
void MpvPlayer::togglePause() {}
void MpvPlayer::stop() {}
void MpvPlayer::setAudioOnly(bool audioOnly) { m_audioOnly = audioOnly; }
void MpvPlayer::seekTo(double) {}
void MpvPlayer::seekRelative(double) {}
void MpvPlayer::seekPercent(double) {}
void MpvPlayer::seekChapter(int) {}
void MpvPlayer::setVolume(int) {}
int MpvPlayer::getVolume() const { return 0; }
void MpvPlayer::adjustVolume(int) {}
void MpvPlayer::setMute(bool) {}
bool MpvPlayer::isMuted() const { return false; }
void MpvPlayer::toggleMute() {}
void MpvPlayer::setSubtitleTrack(int) {}
void MpvPlayer::setAudioTrack(int) {}
void MpvPlayer::setVideoTrack(int) {}
void MpvPlayer::cycleSubtitle() {}
void MpvPlayer::cycleAudio() {}
void MpvPlayer::toggleSubtitles() {}
void MpvPlayer::setSubtitleDelay(double) {}
void MpvPlayer::setAudioDelay(double) {}
void MpvPlayer::disableSubtitles() {}
void MpvPlayer::loadSubtitleUrl(const std::string&) {}
void MpvPlayer::removeExternalSubtitles() {}
std::vector<MpvPlayer::TrackInfo> MpvPlayer::getTrackList(const std::string&) const { return {}; }
double MpvPlayer::getPosition() const { return 0.0; }
double MpvPlayer::getDuration() const { return 0.0; }
double MpvPlayer::getPercentPosition() const { return 0.0; }
void MpvPlayer::showOSD(const std::string&, double) {}
void MpvPlayer::toggleOSD() {}
void MpvPlayer::setOption(const std::string&, const std::string&) {}
std::string MpvPlayer::getProperty(const std::string&) const { return {}; }
void MpvPlayer::update() {}
void MpvPlayer::render() {}
void MpvPlayer::flushGpu() {}
bool MpvPlayer::initRenderContext() { return false; }
void MpvPlayer::cleanupRenderContext() {}
void MpvPlayer::eventMainLoop() {}
void MpvPlayer::updatePlaybackInfo() {}
void MpvPlayer::handleEvent(mpv_event*) {}
void MpvPlayer::handlePropertyChange(mpv_event_property*, uint64_t) {}
void MpvPlayer::setState(MpvPlayerState newState) { m_state = newState; }

} // namespace vitaplex

#endif
