#include <rime/build_config.h>

#if defined(RIME_ENABLE_LOGGING) && !defined(_WIN32)

#include <gtest/gtest.h>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <rime/service.h>
#include <rime_api.h>

namespace {
namespace fs = std::filesystem;

TEST(RimeLogRetentionTest,
     DeploymentKeepsRecentArchivesAndActiveSymlinkTargets) {
  const char* configured = std::getenv("GLOG_log_dir");
  if (!configured || !*configured)
    GTEST_SKIP() << "Run through CTest to use its isolated log directory";
  const auto root = fs::absolute(fs::u8path(configured));
  ASSERT_TRUE(fs::is_directory(root));
  auto api = rime_get_api();
  api->deployer_initialize(nullptr);
  auto& deployer = rime::Service::instance().deployer();
  struct Restore {
    rime::Deployer& deployer;
    std::string name;
    std::vector<fs::path> files;
    ~Restore() {
      deployer.app_name = name;
      for (const auto& file : files) {
        std::error_code error;
        fs::remove(file, error);
        EXPECT_FALSE(error) << error.message();
      }
    }
  } restore{deployer, deployer.app_name, {}};
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto prefix = "rime.retention-test-" + std::to_string(unique);
  deployer.app_name = prefix;
  const auto make = [&](const std::string& suffix, int days) {
    const auto file = root / (prefix + suffix);
    EXPECT_FALSE(fs::exists(file));
    restore.files.push_back(file);
    std::ofstream(file) << "test-owned diagnostic fixture\n";
    fs::last_write_time(
        file, fs::file_time_type::clock::now() - std::chrono::hours(24 * days));
    return file;
  };
  const auto recent = make(".19990101-recent.log", 2);
  const auto expired = make(".19990101-expired.log", 20);
  const auto relative = make(".19990101-relative.log", 20);
  const auto absolute = make(".19990101-absolute.log", 20);
  const auto future = make(".19990101-future.log", -1);
  const auto compressed = make(".19990101-archived.log.gz", 20);
  const auto unrelated = root / ("another-" + prefix + ".log");
  restore.files.push_back(unrelated);
  std::ofstream(unrelated) << "unrelated application fixture\n";
  fs::last_write_time(unrelated, fs::file_time_type::clock::now() -
                                     std::chrono::hours(24 * 20));
  const auto relative_link = root / (prefix + ".INFO");
  const auto absolute_link = root / (prefix + ".ERROR");
  restore.files.push_back(relative_link);
  restore.files.push_back(absolute_link);
  fs::create_symlink(relative.filename(), relative_link);
  fs::create_symlink(absolute, absolute_link);
  const auto external = root / ("external-" + prefix + ".bin");
  restore.files.push_back(external);
  constexpr char external_bytes[] = "preserve external contents\n";
  std::ofstream(external) << external_bytes;
  const auto external_mode =
      fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read;
  fs::permissions(external, external_mode);
  const auto hard_link = root / (prefix + ".19990101-hard-linked.log");
  restore.files.push_back(hard_link);
  fs::create_hard_link(external, hard_link);
  fs::last_write_time(hard_link, fs::file_time_type::clock::now() -
                                     std::chrono::hours(24 * 20));

  ASSERT_TRUE(api->run_task("clean_old_log_files"));
  EXPECT_TRUE(fs::exists(recent));
  EXPECT_FALSE(fs::exists(expired));
  EXPECT_TRUE(fs::exists(relative));
  EXPECT_TRUE(fs::exists(absolute));
  EXPECT_TRUE(fs::exists(future));
  EXPECT_TRUE(fs::exists(compressed));
  EXPECT_TRUE(fs::exists(unrelated));
  EXPECT_TRUE(fs::exists(hard_link));
  EXPECT_EQ(fs::hard_link_count(external), 2u);
  EXPECT_EQ(fs::status(external).permissions() & fs::perms::all, external_mode);
  std::ifstream external_input(external);
  ASSERT_TRUE(external_input.good());
  EXPECT_EQ(std::string(std::istreambuf_iterator<char>(external_input), {}),
            external_bytes);
}

}  // namespace

#endif  // defined(RIME_ENABLE_LOGGING) && !defined(_WIN32)
