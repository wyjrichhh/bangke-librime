//
// Copyright RIME Developers
// Distributed under the BSD License
//
#include <filesystem>
#include <rime/service.h>
#include <rime/algo/fs.h>
#include <rime/algo/utilities.h>
#include <rime/config/config_compiler.h>
#include <rime/config/config_types.h>
#include <rime/config/plugins.h>

namespace rime {

bool BuildInfoPlugin::ReviewCompileOutput(ConfigCompiler* compiler,
                                          an<ConfigResource> resource) {
  return true;
}

bool BuildInfoPlugin::ReviewLinkOutput(ConfigCompiler* compiler,
                                       an<ConfigResource> resource) {
  auto build_info = (*resource)["__build_info"];
  build_info["rime_version"] = RIME_VERSION;
#ifndef RIME_NO_TIMESTAMP
  auto timestamps = build_info["timestamps"];
  auto checksums = build_info["checksums"];
  bool readable = true;
  compiler->EnumerateResources([&](an<ConfigResource> resource) {
    if (!resource->loaded) {
      LOG(INFO) << "resource '" << resource->resource_id << "' not loaded.";
      timestamps[resource->resource_id] = 0;
      return;
    }
    const auto& file_path = resource->data->file_path();
    if (file_path.empty()) {
      LOG(WARNING) << "resource '" << resource->resource_id
                   << "' is not persisted.";
      timestamps[resource->resource_id] = 0;
      return;
    }
    ChecksumComputer checksum;
    if (!checksum.ProcessFileChecked(file_path)) {
      LOG(ERROR) << "cannot checksum config resource '" << resource->resource_id
                 << "'.";
      readable = false;
      return;
    }
    // Decimal strings preserve the full CRC32 range on 32-bit platforms.
    checksums[resource->resource_id] = std::to_string(checksum.Checksum());
    // TODO: store as 64-bit number to avoid the year 2038 problem
    timestamps[resource->resource_id] =
        (int)filesystem::to_time_t(std::filesystem::last_write_time(file_path));
  });
  return readable;
#endif
  return true;
}

}  // namespace rime
