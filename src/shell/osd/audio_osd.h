#pragma once

#include "shell/osd/audio_osd_diff.h"
#include "shell/osd/osd_overlay.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

class PipeWireService;

class AudioOsd {
public:
  void bindOverlay(OsdOverlay& overlay);
  void setSoundPlayer(class SoundPlayer* soundPlayer);
  // [osd.audio] options, applied at bind time and on every config reload.
  void setDeviceOsdOptions(bool allDevices, bool showDeviceName);
  void primeFromService(const PipeWireService& service);
  void suppressFor(std::chrono::milliseconds duration);

  void showOutput(std::uint32_t sinkId, float volume, bool muted, bool playFeedback = true, std::string name = {});
  void showOutputName(std::string name, bool muted);

  void showInput(std::uint32_t sourceId, float volume, bool muted, bool playFeedback = true, std::string name = {});
  void showInputName(std::string name, bool muted);

  // Paint only: no sound, no suppression, no snapshot writes. Displaying a value on request must not
  // look like a committed audio change to onAudioStateChanged().
  void showOutputValue(float volume, bool muted, std::string name = {});
  void showInputValue(float volume, bool muted, std::string name = {});

  void onAudioStateChanged(const PipeWireService& service);

private:
  // The one place the "name on the OSD" rule lives: show_device_name prints it for every device, and
  // with all_devices a non-default popup always carries its name, which would be ambiguous without it.
  [[nodiscard]] std::string displayName(bool isInput, std::uint32_t id, const std::string& label) const;

  OsdOverlay* m_overlay = nullptr;
  OsdKind m_currentKind = OsdKind::Volume; // what the visible OSD is showing, for live mute correction
  bool m_allDevices = false;
  bool m_showDeviceName = false;
  std::vector<audio_osd::DeviceSnapshot> m_sinkSnapshots;
  std::vector<audio_osd::DeviceSnapshot> m_sourceSnapshots;
  std::uint32_t m_lastDefaultSinkId = 0;
  std::uint32_t m_lastDefaultSourceId = 0;
  // Device the visible OSD belongs to, for in-place mute correction.
  std::uint32_t m_shownSinkId = 0;
  std::uint32_t m_shownSourceId = 0;
  std::string m_shownSinkName;
  std::string m_shownSourceName;
  std::chrono::steady_clock::time_point m_suppressUntil;
  std::chrono::steady_clock::time_point m_suppressAutoInputOsdUntil;
  std::chrono::steady_clock::time_point m_lastSoundAt;
  class SoundPlayer* m_soundPlayer = nullptr;
};
