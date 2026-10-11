#pragma once

#include "pipewire/pipewire_service.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// Pure per-device volume-OSD state diff: compares a PipeWire device list against the snapshots taken
// at the last state change and picks the single node that should raise the OSD. Keeping this free of
// overlay/service dependencies lets the selection rules be tested without a running shell.
namespace audio_osd {

  struct DeviceSnapshot {
    std::uint32_t id = 0;
    float volume = -1.0F;
    int percent = -1;
    bool muted = false;
  };

  struct DeviceChange {
    std::uint32_t id = 0;
    std::string label;
    float volume = 0.0F;
    int percent = 0;
    bool muted = false;
    bool volumeChanged = false;
    bool muteChanged = false;
    bool isDefault = false;
  };

  struct DeviceDiff {
    // Snapshot of every node in the list, so removed devices drop out.
    std::vector<DeviceSnapshot> snapshots;
    // The node that should raise the OSD, if any.
    std::optional<DeviceChange> change;
    bool defaultIdChanged = false;
  };

  // Nodes missing from `previous` (startup, hotplug) are snapshotted but never reported as changed.
  // Unavailable nodes are snapshotted but never reported either. When `allDevices` is false only a
  // change on `defaultId` is reported; otherwise the default node wins over the largest volume delta
  // of the non-default nodes, ties broken by the earliest position in `nodes`.
  [[nodiscard]] DeviceDiff diffDevices(
      const std::vector<AudioNode>& nodes, const std::vector<DeviceSnapshot>& previous, std::uint32_t defaultId,
      std::uint32_t previousDefaultId, bool allDevices, float epsilon = 0.003F
  );

} // namespace audio_osd
