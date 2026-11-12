#include <gtest/gtest.h>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <rime/config.h>

TEST(RimeConfigNativePathTest, LoadsYamlFromUnicodeDirectoryAndFilename) {
  namespace fs = std::filesystem;
  const auto unique =
      std::chrono::steady_clock::now().time_since_epoch().count();
  const auto directory = fs::temp_directory_path() /
                         ("rime-config-native-test-" + std::to_string(unique));
  ASSERT_FALSE(fs::exists(directory));
  struct Cleanup {
    fs::path directory;
    ~Cleanup() {
      std::error_code error;
      fs::remove_all(directory, error);
      EXPECT_FALSE(error) << error.message();
    }
  } cleanup{directory};
  const auto nested = directory / fs::u8path(u8"\u4e2d\u6587\u76ee\u5f55");
  fs::create_directories(nested);
  const auto file = nested / fs::u8path(u8"\u8bbe\u7f6e.yaml");
  {
    std::ofstream output(file);
    ASSERT_TRUE(output.good());
    output << "resource:\n  status: ready\n";
  }
  rime::Config config;
  ASSERT_TRUE(config.LoadFromFile(rime::path(file)));
  std::string status;
  EXPECT_TRUE(config.GetString("resource/status", &status));
  EXPECT_EQ(status, "ready");
}
