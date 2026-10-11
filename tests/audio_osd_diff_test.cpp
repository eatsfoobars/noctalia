#include "pipewire/pipewire_service.h"
#include "shell/osd/audio_osd_diff.h"
#include "test_check.h"

#include <cstdint>
#include <vector>

namespace {

  using audio_osd::DeviceSnapshot;

  [[nodiscard]] AudioNode
  device(std::uint32_t id, std::string description, float volume, bool muted = false, bool available = true) {
    return AudioNode{
        .id = id,
        .name = "node-" + std::to_string(id),
        .description = std::move(description),
        .mediaClass = "Audio/Sink",
        .volume = volume,
        .muted = muted,
        .available = available,
    };
  }

  [[nodiscard]] std::vector<DeviceSnapshot> snapshotsOf(const std::vector<AudioNode>& nodes) {
    return audio_osd::diffDevices(nodes, {}, /*defaultId=*/0, /*previousDefaultId=*/0, /*allDevices=*/true).snapshots;
  }

} // namespace

int main() {
  const std::vector<AudioNode> current = {
      device(1, "Speakers", 0.50F),
      device(2, "Headphones", 0.40F),
      device(3, "Monitor HDMI", 0.30F),
  };
  const std::vector<DeviceSnapshot> previous = snapshotsOf(current);

  // Only the default device is reported unless all_devices is enabled.
  {
    std::vector<AudioNode> changed = current;
    changed[1].volume = 0.60F;
    const auto diff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(!diff.change.has_value());
    TEST_CHECK(!diff.defaultIdChanged);

    changed = current;
    changed[0].volume = 0.60F;
    const auto defaultDiff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(defaultDiff.change.has_value());
    TEST_CHECK(defaultDiff.change->id == 1);
    TEST_CHECK(defaultDiff.change->isDefault);
    TEST_CHECK(defaultDiff.change->volumeChanged);
    TEST_CHECK(!defaultDiff.change->muteChanged);
    TEST_CHECK(defaultDiff.change->label == "Speakers");
    TEST_CHECK(defaultDiff.change->percent == 60);
  }

  // With all devices on, the default change still wins over a larger non-default delta.
  {
    std::vector<AudioNode> changed = current;
    changed[0].volume = 0.52F;
    changed[2].volume = 0.90F;
    const auto diff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(diff.change.has_value());
    TEST_CHECK(diff.change->id == 1);
  }

  // Without a default change, the largest non-default delta wins, ties break by list order.
  {
    std::vector<AudioNode> changed = current;
    changed[1].volume = 0.44F;
    changed[2].volume = 0.80F;
    const auto diff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(diff.change.has_value());
    TEST_CHECK(diff.change->id == 3);
    TEST_CHECK(!diff.change->isDefault);

    std::vector<AudioNode> tied = current;
    tied[1].volume = 0.50F;
    tied[2].volume = 0.40F;
    const auto tieDiff = audio_osd::diffDevices(tied, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(tieDiff.change.has_value());
    TEST_CHECK(tieDiff.change->id == 2);
  }

  // A newly appeared node is snapshotted, not reported.
  {
    std::vector<AudioNode> withHotplug = current;
    withHotplug.push_back(device(4, "USB Headset", 0.75F));
    const auto diff = audio_osd::diffDevices(withHotplug, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(!diff.change.has_value());
    TEST_CHECK(diff.snapshots.size() == 4);
    TEST_CHECK(diff.snapshots.back().id == 4);
    TEST_CHECK(diff.snapshots.back().percent == 75);
  }

  // Unavailable non-default nodes are snapshotted but never raise the OSD; the default is exempt.
  {
    std::vector<AudioNode> changed = current;
    changed[2].volume = 0.95F;
    changed[2].available = false;
    const auto offDefault = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(!offDefault.change.has_value());
    TEST_CHECK(offDefault.snapshots.size() == 3);

    std::vector<AudioNode> defaultChanged = current;
    defaultChanged[0].volume = 0.95F;
    defaultChanged[0].available = false;
    const auto onDefault =
        audio_osd::diffDevices(defaultChanged, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(onDefault.change.has_value());
    TEST_CHECK(onDefault.change->id == 1);
    TEST_CHECK(onDefault.change->isDefault);
    TEST_CHECK(onDefault.change->volumeChanged);
    TEST_CHECK(onDefault.change->percent == 95);
    TEST_CHECK(!onDefault.change->muted);

    // Same exemption for a mute-only flip, and with all_devices off (pre-change default behaviour).
    std::vector<AudioNode> defaultMuted = current;
    defaultMuted[0].muted = true;
    defaultMuted[0].available = false;
    const auto onDefaultMute =
        audio_osd::diffDevices(defaultMuted, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(onDefaultMute.change.has_value());
    TEST_CHECK(onDefaultMute.change->muteChanged);
    TEST_CHECK(!onDefaultMute.change->volumeChanged);
  }

  // A mute flip with an unchanged volume reports only the mute change.
  {
    std::vector<AudioNode> changed = current;
    changed[0].muted = true;
    const auto diff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(diff.change.has_value());
    TEST_CHECK(diff.change->muteChanged);
    TEST_CHECK(!diff.change->volumeChanged);
    TEST_CHECK(diff.change->muted);
  }

  // Route changes flip defaultIdChanged; removed devices drop out of the snapshots.
  {
    const auto rerouted = audio_osd::diffDevices(current, previous, /*defaultId=*/2, /*previousDefaultId=*/1, false);
    TEST_CHECK(rerouted.defaultIdChanged);
    TEST_CHECK(!rerouted.change.has_value());

    const auto sameDefault = audio_osd::diffDevices(current, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(!sameDefault.defaultIdChanged);

    std::vector<AudioNode> shrunk = current;
    shrunk.pop_back();
    const auto removed = audio_osd::diffDevices(shrunk, previous, /*defaultId=*/1, /*previousDefaultId=*/1, true);
    TEST_CHECK(removed.snapshots.size() == 2);
    TEST_CHECK(!removed.change.has_value());
  }

  // Volumes inside the epsilon band are not changes; the label falls back to the node name.
  {
    std::vector<AudioNode> changed = current;
    changed[0].volume = 0.502F;
    const auto diff = audio_osd::diffDevices(changed, previous, /*defaultId=*/1, /*previousDefaultId=*/1, false);
    TEST_CHECK(!diff.change.has_value());

    const auto unnamedPrevious = snapshotsOf({device(7, "", 0.20F)});
    const auto unnamedDiff = audio_osd::diffDevices(
        {device(7, "", 0.60F)}, unnamedPrevious, /*defaultId=*/7, /*previousDefaultId=*/7, false
    );
    TEST_CHECK(unnamedDiff.change.has_value());
    TEST_CHECK(unnamedDiff.change->label == "node-7");
    TEST_CHECK(unnamedDiff.change->percent == 60);
  }

  return 0;
}
