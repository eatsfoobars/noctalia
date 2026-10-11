#include "shell/osd/audio_osd.h"

#include "pipewire/pipewire_service.h"
#include "pipewire/sound_player.h"
#include "shell/osd/audio_osd_diff.h"
#include "shell/osd/osd_overlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <string>
#include <utility>
#include <vector>

namespace {
  constexpr auto kVolumeSoundCooldown = std::chrono::milliseconds(70);
  constexpr auto kSuppressInputOsdAfterOutput = std::chrono::milliseconds(500);
  constexpr auto kSuppressAutoInputOsdAfterRoute = std::chrono::milliseconds(400);

  [[nodiscard]] int displayPercent(float volume) {
    return static_cast<int>(std::round(std::max(0.0F, volume) * 100.0F));
  }

  [[nodiscard]] audio_osd::DeviceSnapshot*
  findSnapshot(std::vector<audio_osd::DeviceSnapshot>& snapshots, std::uint32_t id) {
    auto it = std::ranges::find(snapshots, id, &audio_osd::DeviceSnapshot::id);
    return it == snapshots.end() ? nullptr : &*it;
  }

  // Echo suppression: the OSD just displayed this node's value, so the next state diff must not read
  // it back as a new change. Nodes without a snapshot (unlisted or removed) are skipped.
  void commitSnapshot(std::vector<audio_osd::DeviceSnapshot>& snapshots, std::uint32_t id, float volume, bool muted) {
    audio_osd::DeviceSnapshot* snapshot = findSnapshot(snapshots, id);
    if (snapshot == nullptr) {
      return;
    }
    snapshot->volume = volume;
    snapshot->percent = displayPercent(volume);
    snapshot->muted = muted;
  }

  // The node an in-place correction belongs to: the one the OSD shows, or the default when that node
  // is gone (id 0, or removed from the device list).
  [[nodiscard]] const AudioNode* shownNode(const PipeWireService& service, std::uint32_t id, bool isInput) {
    const AudioNode* node = isInput ? service.findSource(id) : service.findSink(id);
    if (node != nullptr) {
      return node;
    }
    return isInput ? service.defaultSource() : service.defaultSink();
  }

  OsdContent makeOutputContent(float volume, bool muted, std::string name = {}) {
    const int percent = displayPercent(volume);
    return OsdContent{
        .kind = OsdKind::Volume,
        .icon = audioVolumeGlyph(volume, muted, false),
        .value = std::to_string(percent) + "%",
        .progress = std::clamp(volume, 0.0F, 1.0F),
        .overLimit = percent > 100,
        .inactive = muted,
        .name = std::move(name),
    };
  }

  OsdContent makeOutputNameContent(std::string name, bool muted) {
    return OsdContent{
        .kind = OsdKind::Volume,
        .icon = audioVolumeGlyph(1.0F, muted, false),
        .value = std::move(name),
        .showProgress = false,
        .inactive = muted,
    };
  }

  OsdContent makeInputContent(float volume, bool muted, std::string name = {}) {
    const int percent = displayPercent(volume);
    return OsdContent{
        .kind = OsdKind::Microphone,
        .icon = audioVolumeGlyph(volume, muted, true),
        .value = std::to_string(percent) + "%",
        .progress = std::clamp(volume, 0.0F, 1.0F),
        .overLimit = percent > 100,
        .inactive = muted,
        .name = std::move(name),
    };
  }

  OsdContent makeInputNameContent(std::string name, bool muted) {
    return OsdContent{
        .kind = OsdKind::Microphone,
        .icon = audioVolumeGlyph(1.0F, muted, true),
        .value = std::move(name),
        .showProgress = false,
        .inactive = muted,
    };
  }

} // namespace

void AudioOsd::bindOverlay(OsdOverlay& overlay) { m_overlay = &overlay; }
void AudioOsd::setSoundPlayer(SoundPlayer* soundPlayer) { m_soundPlayer = soundPlayer; }

void AudioOsd::setDeviceOsdOptions(bool allDevices, bool showDeviceName) {
  m_allDevices = allDevices;
  m_showDeviceName = showDeviceName;
}

std::string AudioOsd::displayName(bool isInput, std::uint32_t id, const std::string& label) const {
  if (label.empty()) {
    return {};
  }
  if (m_showDeviceName) {
    return label;
  }
  // A non-default popup is ambiguous without its device; those only exist once all_devices is on.
  const std::uint32_t defaultId = isInput ? m_lastDefaultSourceId : m_lastDefaultSinkId;
  return (m_allDevices && id != defaultId) ? label : std::string{};
}

void AudioOsd::primeFromService(const PipeWireService& service) {
  const auto& st = service.state();
  m_sinkSnapshots = audio_osd::diffDevices(st.sinks, {}, st.defaultSinkId, st.defaultSinkId, false).snapshots;
  m_sourceSnapshots = audio_osd::diffDevices(st.sources, {}, st.defaultSourceId, st.defaultSourceId, false).snapshots;
  m_lastDefaultSinkId = st.defaultSinkId;
  m_lastDefaultSourceId = st.defaultSourceId;
}

void AudioOsd::suppressFor(std::chrono::milliseconds duration) {
  m_suppressUntil = std::chrono::steady_clock::now() + duration;
}

void AudioOsd::showOutput(std::uint32_t sinkId, float volume, bool muted, bool playFeedback, std::string name) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  m_suppressAutoInputOsdUntil = now + kSuppressInputOsdAfterOutput;
  const std::string shownName = displayName(false, sinkId, name);
  if (m_overlay != nullptr) {
    m_overlay->show(makeOutputContent(volume, muted, shownName));
    m_currentKind = OsdKind::Volume;
  }
  if (playFeedback && m_soundPlayer != nullptr && now - m_lastSoundAt >= kVolumeSoundCooldown) {
    m_soundPlayer->play(SoundPlayer::kEventVolumeChange);
    m_lastSoundAt = now;
  }
  m_shownSinkId = sinkId;
  m_shownSinkName = shownName;
  commitSnapshot(m_sinkSnapshots, sinkId, volume, muted);
}

void AudioOsd::showOutputName(std::string name, bool muted) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  m_suppressAutoInputOsdUntil = now + kSuppressInputOsdAfterOutput;
  if (m_overlay != nullptr) {
    m_overlay->show(makeOutputNameContent(std::move(name), muted));
    m_currentKind = OsdKind::Volume;
  }
}

void AudioOsd::showInput(std::uint32_t sourceId, float volume, bool muted, bool playFeedback, std::string name) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  const std::string shownName = displayName(true, sourceId, name);
  if (m_overlay != nullptr) {
    m_overlay->show(makeInputContent(volume, muted, shownName));
    m_currentKind = OsdKind::Microphone;
  }
  if (playFeedback && m_soundPlayer != nullptr && now - m_lastSoundAt >= kVolumeSoundCooldown) {
    m_soundPlayer->play(SoundPlayer::kEventVolumeChange);
    m_lastSoundAt = now;
  }
  m_shownSourceId = sourceId;
  m_shownSourceName = shownName;
  commitSnapshot(m_sourceSnapshots, sourceId, volume, muted);
}

void AudioOsd::showInputName(std::string name, bool muted) {
  const auto now = std::chrono::steady_clock::now();
  if (now < m_suppressUntil) {
    return;
  }
  if (m_overlay != nullptr) {
    m_overlay->show(makeInputNameContent(std::move(name), muted));
    m_currentKind = OsdKind::Microphone;
  }
}

void AudioOsd::showOutputValue(float volume, bool muted, std::string name) {
  if (m_overlay == nullptr) {
    return;
  }
  // A value shown on request always depicts the default device.
  const std::string shownName = displayName(false, m_lastDefaultSinkId, name);
  m_overlay->show(makeOutputContent(volume, muted, shownName));
  m_currentKind = OsdKind::Volume;
  m_shownSinkId = m_lastDefaultSinkId;
  m_shownSinkName = shownName;
}

void AudioOsd::showInputValue(float volume, bool muted, std::string name) {
  if (m_overlay == nullptr) {
    return;
  }
  const std::string shownName = displayName(true, m_lastDefaultSourceId, name);
  m_overlay->show(makeInputContent(volume, muted, shownName));
  m_currentKind = OsdKind::Microphone;
  m_shownSourceId = m_lastDefaultSourceId;
  m_shownSourceName = shownName;
}

void AudioOsd::onAudioStateChanged(const PipeWireService& service) {
  const auto& st = service.state();
  const auto now = std::chrono::steady_clock::now();

  auto sinkDiff =
      audio_osd::diffDevices(st.sinks, m_sinkSnapshots, st.defaultSinkId, m_lastDefaultSinkId, m_allDevices);
  auto sourceDiff =
      audio_osd::diffDevices(st.sources, m_sourceSnapshots, st.defaultSourceId, m_lastDefaultSourceId, m_allDevices);

  // The bar samples committed audio state live on every refresh; the OSD is a frozen snapshot fed
  // through suppressed/optimistic event paths, so the two can end up showing different mute icons.
  // Whenever committed mute changes for the node the OSD is currently displaying, force its icon
  // back in sync in place. Suppression only gates popping up / switching OSDs, not correcting the
  // one already on screen; volume stays optimistic, only the mute state is reconciled.
  if (m_overlay != nullptr && m_overlay->isVisible()) {
    if (m_currentKind == OsdKind::Volume) {
      const AudioNode* sink = shownNode(service, m_shownSinkId, false);
      const audio_osd::DeviceSnapshot* snapshot = sink != nullptr ? findSnapshot(m_sinkSnapshots, sink->id) : nullptr;
      if (snapshot != nullptr && sink->muted != snapshot->muted) {
        m_overlay->show(makeOutputContent(sink->volume, sink->muted, m_shownSinkName));
      }
    } else if (m_currentKind == OsdKind::Microphone) {
      const AudioNode* source = shownNode(service, m_shownSourceId, true);
      const audio_osd::DeviceSnapshot* snapshot =
          source != nullptr ? findSnapshot(m_sourceSnapshots, source->id) : nullptr;
      if (snapshot != nullptr && source->muted != snapshot->muted) {
        m_overlay->show(makeInputContent(source->volume, source->muted, m_shownSourceName));
      }
    }
  }

  if (now < m_suppressUntil) {
    m_sinkSnapshots = std::move(sinkDiff.snapshots);
    m_sourceSnapshots = std::move(sourceDiff.snapshots);
    m_lastDefaultSinkId = st.defaultSinkId;
    m_lastDefaultSourceId = st.defaultSourceId;
    return;
  }

  const bool sinkDisappeared = st.defaultSinkId == 0 && m_lastDefaultSinkId != 0;
  if (sinkDiff.defaultIdChanged || sinkDisappeared) {
    m_suppressAutoInputOsdUntil = now + kSuppressAutoInputOsdAfterRoute;
  }

  if (m_overlay != nullptr) {
    const AudioNode* newDefaultSink = service.findSink(st.defaultSinkId);
    const AudioNode* newDefaultSource = service.findSource(st.defaultSourceId);
    if (sinkDiff.defaultIdChanged && newDefaultSink != nullptr) {
      const std::string label = audioDeviceLabel(*newDefaultSink);
      showOutputName(label, newDefaultSink->muted);
      m_shownSinkId = newDefaultSink->id;
      // The route popup itself is a name-only text card (its OsdContent::name stays empty), but the
      // node it depicts is the new default, so the name a later in-place mute correction draws follows
      // the same rule as every other default-device volume card: named only when the user enabled
      // osd.audio.show_device_name. displayName() cannot be used here: it compares against
      // m_lastDefaultSinkId, which in this branch is still the previous default, so with all_devices
      // on it would treat the new default as a non-default device and name a card nobody asked for.
      m_shownSinkName = m_showDeviceName ? label : std::string{};
    } else if (sinkDiff.change.has_value()) {
      const auto& change = *sinkDiff.change;
      // Passive PipeWire updates: click only on real volume changes while unmuted.
      showOutput(change.id, change.volume, change.muted, change.volumeChanged && !change.muted, change.label);
    } else if (sourceDiff.defaultIdChanged && newDefaultSource != nullptr && now >= m_suppressAutoInputOsdUntil) {
      const std::string label = audioDeviceLabel(*newDefaultSource);
      showInputName(label, newDefaultSource->muted);
      m_shownSourceId = newDefaultSource->id;
      // Same rule as the sink branch: the shown node is the new default source.
      m_shownSourceName = m_showDeviceName ? label : std::string{};
    } else if (sourceDiff.change.has_value() && now >= m_suppressAutoInputOsdUntil) {
      const auto& change = *sourceDiff.change;
      showInput(change.id, change.volume, change.muted, change.volumeChanged && !change.muted, change.label);
    }
  }

  m_sinkSnapshots = std::move(sinkDiff.snapshots);
  m_sourceSnapshots = std::move(sourceDiff.snapshots);
  m_lastDefaultSinkId = st.defaultSinkId;
  m_lastDefaultSourceId = st.defaultSourceId;
}
