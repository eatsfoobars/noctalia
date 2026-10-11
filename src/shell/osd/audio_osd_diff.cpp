#include "shell/osd/audio_osd_diff.h"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace audio_osd {

  namespace {
    [[nodiscard]] int displayPercent(float volume) {
      return static_cast<int>(std::round(std::max(0.0F, volume) * 100.0F));
    }

    [[nodiscard]] const DeviceSnapshot* findSnapshot(const std::vector<DeviceSnapshot>& snapshots, std::uint32_t id) {
      auto it = std::ranges::find(snapshots, id, &DeviceSnapshot::id);
      return it == snapshots.end() ? nullptr : &*it;
    }
  } // namespace

  DeviceDiff diffDevices(
      const std::vector<AudioNode>& nodes, const std::vector<DeviceSnapshot>& previous, std::uint32_t defaultId,
      std::uint32_t previousDefaultId, bool allDevices, float epsilon
  ) {
    DeviceDiff diff;
    diff.defaultIdChanged = defaultId != previousDefaultId;
    diff.snapshots.reserve(nodes.size());
    for (const auto& node : nodes) {
      diff.snapshots.push_back(
          DeviceSnapshot{
              .id = node.id,
              .volume = node.volume,
              .percent = displayPercent(node.volume),
              .muted = node.muted,
          }
      );
    }

    std::optional<DeviceChange> defaultChange;
    std::optional<DeviceChange> otherChange;
    float otherDelta = 0.0F;

    for (const auto& node : nodes) {
      const DeviceSnapshot* prev = findSnapshot(previous, node.id);
      const bool isDefault = node.id == defaultId;
      // An unavailable active route hides a device from the shell's device lists, but the current
      // default is always kept (same exemption as the control center's device list): otherwise an
      // unplugged-but-still-default sink would silently lose its volume OSD.
      if (prev == nullptr || (!node.available && !isDefault)) {
        continue;
      }
      const bool volumeChanged = std::abs(node.volume - prev->volume) > epsilon;
      const bool muteChanged = node.muted != prev->muted;
      if (!volumeChanged && !muteChanged) {
        continue;
      }
      if (!isDefault && !allDevices) {
        continue;
      }

      DeviceChange change{
          .id = node.id,
          .label = audioDeviceLabel(node),
          .volume = node.volume,
          .percent = displayPercent(node.volume),
          .muted = node.muted,
          .volumeChanged = volumeChanged,
          .muteChanged = muteChanged,
          .isDefault = isDefault,
      };

      if (isDefault) {
        if (!defaultChange.has_value()) {
          defaultChange = std::move(change);
        }
        continue;
      }
      // Largest volume delta wins; an equal delta keeps the earlier node.
      const float delta = std::abs(node.volume - prev->volume);
      if (!otherChange.has_value() || delta > otherDelta) {
        otherDelta = delta;
        otherChange = std::move(change);
      }
    }

    // A non-default change is only recorded when `allDevices` is set, so the default always wins.
    diff.change = defaultChange.has_value() ? std::move(defaultChange) : std::move(otherChange);
    return diff;
  }

} // namespace audio_osd
