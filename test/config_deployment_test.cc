// Copyright RIME Developers
// Distributed under the BSD License
#include <gtest/gtest.h>
#include <rime_api.h>
#include <rime/algo/fs.h>
#include <rime/algo/utilities.h>
#include <rime/config.h>
#include <rime/config/config_compiler.h>
#include <rime/config/plugins.h>
#include <rime/resource.h>
#include <rime/lever/deployment_tasks.h>
#include <rime/service.h>
#include <chrono>
#include <filesystem>
#include <fstream>

namespace {
namespace fs = std::filesystem;
fs::path root;

class DeploymentEnvironment : public testing::Environment {
 public:
  void SetUp() override {
    root =
        fs::temp_directory_path() /
        fs::u8path(
            u8"rime-config-deployment-中文-" +
            std::to_string(
                std::chrono::steady_clock::now().time_since_epoch().count()));
    ASSERT_FALSE(fs::exists(root));
    fs::create_directories(root / "source");
    fs::create_directories(root / "build");
    const auto source = (root / "source").u8string();
    const auto build = (root / "build").u8string();
    RIME_STRUCT(RimeTraits, traits);
    traits.shared_data_dir = traits.user_data_dir = source.c_str();
    traits.staging_dir = traits.prebuilt_data_dir = build.c_str();
    traits.app_name = "rime.config-deployment-test";
    auto api = rime_get_api();
    api->setup(&traits);
    api->deployer_initialize(&traits);
  }
  void TearDown() override {
    rime_get_api()->finalize();
    std::error_code error;
    fs::remove_all(root, error);
    EXPECT_FALSE(error) << error.message();
  }
};

class ConfigDeploymentTest : public testing::Test {
 protected:
  void SetUp() override {
    id = testing::UnitTest::GetInstance()->current_test_info()->name();
    source = root / "source" / (id + ".yaml");
    custom = root / "source" / (id + ".custom.yaml");
    output = root / "build" / (id + ".yaml");
    Write(source, "config_version: '1'\nvalue: base\n");
  }
  void Write(const fs::path& file, const std::string& text) {
    std::ofstream stream(file);
    ASSERT_TRUE(stream.good());
    stream << text;
    stream.close();
    ASSERT_TRUE(stream.good());
  }
  bool Deploy() {
    return rime_get_api()->deploy_config_file((id + ".yaml").c_str(),
                                              "config_version");
  }
  std::string Value() {
    rime::Config config;
    EXPECT_TRUE(config.LoadFromFile(output));
    std::string value;
    EXPECT_TRUE(config.GetString("value", &value));
    return value;
  }
  std::string id;
  fs::path source, custom, output;
};

TEST_F(ConfigDeploymentTest, ChecksumReaderPreservesLegacyAccumulation) {
  Write(source, "a");
  Write(custom, "bc");
  boost::crc_32_type crc(0);
  ASSERT_TRUE(rime::ChecksumFileChecked(source, crc));
  ASSERT_TRUE(rime::ChecksumFileChecked(custom, crc));
  // CRC32 of "abc" with the existing zero initial remainder.
  EXPECT_EQ(0x359a672fu, crc.checksum());
  EXPECT_FALSE(rime::ChecksumFileChecked(root / "absent", crc));
  EXPECT_EQ(0x359a672fu, crc.checksum());
#if !defined(_WIN32) || !defined(RIME_BUILD_SHARED_LIBS)
  rime::ChecksumComputer legacy;
  legacy.ProcessFile(source);
  ASSERT_TRUE(legacy.ProcessFileChecked(custom));
  EXPECT_EQ(0x359a672fu, legacy.Checksum());
#endif
}

TEST_F(ConfigDeploymentTest, SameSecondPatchChange) {
  auto first = fs::file_time_type::clock::now() - std::chrono::hours(24);
  auto second = first + std::chrono::milliseconds(100);
  while (rime::filesystem::to_time_t(first) !=
         rime::filesystem::to_time_t(second)) {
    first += std::chrono::milliseconds(100);
    second = first + std::chrono::milliseconds(100);
  }
  Write(custom, "patch:\n  value: enabled\n");
  fs::last_write_time(custom, first);
  const auto actual_first = fs::last_write_time(custom);
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("enabled", Value());
  Write(custom, "patch:\n  value: disabled\n");
  fs::last_write_time(custom, second);
  ASSERT_NE(actual_first, fs::last_write_time(custom));
  ASSERT_EQ(rime::filesystem::to_time_t(actual_first),
            rime::filesystem::to_time_t(fs::last_write_time(custom)));
  ASSERT_TRUE(Deploy());
  EXPECT_EQ("disabled", Value());
}

TEST_F(ConfigDeploymentTest, SameTimestampIncludeChange) {
  const auto dependency = root / "source" / (id + "_dependency.yaml");
  Write(source, "__include: " + id + "_dependency:/\n");
  Write(dependency, "value: before\n");
  const auto time = fs::last_write_time(dependency);
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("before", Value());
  Write(dependency, "value: after\n");
  fs::last_write_time(dependency, time);
  ASSERT_TRUE(Deploy());
  EXPECT_EQ("after", Value());
}

TEST_F(ConfigDeploymentTest, UnchangedSourcesDoNotRewriteOutput) {
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("base", Value());
  const auto sentinel =
      fs::file_time_type::clock::now() - std::chrono::hours(48);
  fs::last_write_time(output, sentinel);
  const auto actual = fs::last_write_time(output);
  ASSERT_TRUE(Deploy());
  EXPECT_EQ(actual, fs::last_write_time(output));
}

TEST_F(ConfigDeploymentTest, OptionalPatchAdditionAndRemoval) {
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("base", Value());
  Write(custom, "patch:\n  value: added\n");
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("added", Value());
  ASSERT_TRUE(fs::remove(custom));
  ASSERT_TRUE(Deploy());
  EXPECT_EQ("base", Value());
}

// Internal compiler plugins are not exported by Windows shared builds.
#if !defined(_WIN32) || !defined(RIME_BUILD_SHARED_LIBS)
TEST_F(ConfigDeploymentTest, ChecksumReadFailurePreservesCompiledOutput) {
  ASSERT_TRUE(Deploy());
  ASSERT_EQ("base", Value());
  const auto previous_time = fs::last_write_time(output);
  // Exercise the real metadata and persistence plugins in their production
  // order.
  struct MetadataAndSave : rime::ConfigCompilerPlugin {
    rime::BuildInfoPlugin metadata;
    rime::SaveOutputPlugin save;
    bool ReviewCompileOutput(rime::ConfigCompiler*,
                             rime::an<rime::ConfigResource>) override {
      return true;
    }
    bool ReviewLinkOutput(rime::ConfigCompiler* compiler,
                          rime::an<rime::ConfigResource> resource) override {
      return metadata.ReviewLinkOutput(compiler, resource) &&
             save.ReviewLinkOutput(compiler, resource);
    }
  } plugins;
  rime::ResourceResolver resolver({"config", "", ".yaml"});
  resolver.set_root_path(root / "source");
  rime::ConfigCompiler compiler(&resolver, &plugins);
  auto resource = compiler.Compile(id);
  ASSERT_TRUE(resource->loaded);
  ASSERT_TRUE(fs::remove(source));
  EXPECT_FALSE(compiler.Link(resource));
  EXPECT_EQ(previous_time, fs::last_write_time(output));
  EXPECT_EQ("base", Value());
}

#endif

TEST_F(ConfigDeploymentTest, LegacyMetadataMigratesOnce) {
  ASSERT_TRUE(Deploy());
  {
    rime::Config config;
    ASSERT_TRUE(config.LoadFromFile(output));
    config["__build_info"]["checksums"].Clear();
    ASSERT_TRUE(config.SaveToFile(output));
  }
  ASSERT_TRUE(Deploy());
  {
    rime::Config config;
    ASSERT_TRUE(config.LoadFromFile(output));
    EXPECT_NE(nullptr, config.GetMap("__build_info/checksums"));
  }
  const auto sentinel =
      fs::file_time_type::clock::now() - std::chrono::hours(48);
  fs::last_write_time(output, sentinel);
  const auto actual = fs::last_write_time(output);
  ASSERT_TRUE(Deploy());
  EXPECT_EQ(actual, fs::last_write_time(output));
}
}  // namespace

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  testing::AddGlobalTestEnvironment(new DeploymentEnvironment);
  return RUN_ALL_TESTS();
}
