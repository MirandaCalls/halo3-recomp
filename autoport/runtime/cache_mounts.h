// Utility-partition cache mounts (Xenia's `mount_cache`).
//
// The SDK deliberately leaves cache0:/cache1:/cache: unmounted. Titles that
// stream content through the utility partition (Halo: Reach copies map data to
// cache0XX.map and streams textures from it) then silently lose that data and
// render with missing textures. Mount them as host directories under the user
// data root, matching Xenia's layout.

#pragma once

#include <rex/filesystem/devices/host_path_device.h>
#include <rex/filesystem/vfs.h>
#include <rex/runtime.h>

#include <cstdlib>
#include <filesystem>
#include <memory>

namespace autoport {

inline void MountCachePartitions(rex::Runtime* runtime) {
  if (!runtime || !runtime->file_system() || std::getenv("AUTOPORT_NO_CACHE")) return;
  const std::filesystem::path root = runtime->user_data_root() / "cache_partitions";
  struct Mount {
    const char* device;
    const char* link;
    const char* dir;
  };
  for (const Mount& m : {Mount{"\\CACHE0", "cache0:", "cache0"}, Mount{"\\CACHE1", "cache1:", "cache1"},
                         Mount{"\\CACHE", "cache:", "cache"}}) {
    const auto host = root / m.dir;
    std::filesystem::create_directories(host);
    // allow_share_delete: titles mark cache files delete-on-close while other
    // handles are still open.
    auto device = std::make_unique<rex::filesystem::HostPathDevice>(m.device, host, false, true);
    if (!device->Initialize()) continue;
    runtime->file_system()->RegisterDevice(std::move(device));
    runtime->file_system()->RegisterSymbolicLink(m.link, m.device);
  }
}

}  // namespace autoport
