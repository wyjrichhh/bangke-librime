#include <rime/build_config.h>

#if defined(_WIN32) && defined(RIME_ENABLE_LOGGING)

#include <gtest/gtest.h>
#include <rime/windows_log_sink.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <thread>
#include <vector>

namespace {
namespace fs = std::filesystem;

class WindowsLogSinkTest : public testing::Test {
 protected:
  void SetUp() override {
    directory_ = fs::temp_directory_path() /
                 (L"rime-log-test-" + std::to_wstring(GetCurrentProcessId()) +
                  L"-" + std::to_wstring(GetTickCount64())) /
                 fs::u8path(u8"\u4e2d\u6587\u65e5\u5fd7");
    ASSERT_FALSE(fs::exists(directory_));
  }

  void TearDown() override {
    std::error_code error;
    fs::remove_all(directory_.parent_path(), error);
    EXPECT_FALSE(error) << error.message();
  }

  fs::path LogPath() const {
    return directory_ / (L"rime.bangke-engine-" +
                         std::to_wstring(GetCurrentProcessId()) + L".log");
  }

  static void Send(rime::WindowsLogSink& sink,
                   const std::string& message,
                   google::LogSeverity severity = google::GLOG_INFO) {
    sink.send(severity, __FILE__, "windows_log_sink_test.cc", __LINE__,
              google::LogMessageTime(), message.data(), message.size());
  }

  static std::string Read(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), {});
  }

  fs::path directory_;
};

TEST_F(WindowsLogSinkTest, WritesUnicodeDirectoryAndFlushesErrors) {
  rime::WindowsLogSink sink;
  ASSERT_TRUE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
  Send(sink, "resource-ready stage=model-open code=0");
  Send(sink, "resource-failed stage=model-open code=2", google::GLOG_ERROR);
  EXPECT_NE(Read(LogPath()).find("resource-ready"), std::string::npos);
  EXPECT_NE(Read(LogPath()).find("resource-failed"), std::string::npos);
}

TEST_F(WindowsLogSinkTest, ActiveFileCannotBeArchivedUntilWriterCloses) {
  {
    rime::WindowsLogSink sink;
    ASSERT_TRUE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
    Send(sink, "resource-ready");
    const HANDLE file =
        CreateFileW(LogPath().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
                    nullptr, OPEN_EXISTING, 0, nullptr);
    const DWORD error = GetLastError();
    EXPECT_EQ(file, INVALID_HANDLE_VALUE);
    EXPECT_EQ(error, DWORD(ERROR_SHARING_VIOLATION));
    if (file != INVALID_HANDLE_VALUE)
      CloseHandle(file);
  }
  const HANDLE file =
      CreateFileW(LogPath().c_str(), GENERIC_READ | DELETE, FILE_SHARE_READ,
                  nullptr, OPEN_EXISTING, 0, nullptr);
  EXPECT_NE(file, INVALID_HANDLE_VALUE);
  if (file != INVALID_HANDLE_VALUE)
    CloseHandle(file);
}

TEST_F(WindowsLogSinkTest,
       ConcurrentWritesKeepCurrentAndThreeClosedFilesBounded) {
  {
    rime::WindowsLogSink sink;
    ASSERT_TRUE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
    std::vector<std::thread> writers;
    for (int thread = 0; thread < 4; ++thread) {
      writers.emplace_back([&sink] {
        const std::string message(32 * 1024, 'x');
        for (int index = 0; index < 200; ++index)
          Send(sink, message);
      });
    }
    for (auto& writer : writers)
      writer.join();
  }
  unsigned count = 0;
  for (const auto& entry : fs::directory_iterator(directory_)) {
    ASSERT_TRUE(entry.is_regular_file());
    EXPECT_LE(entry.file_size(), 4u * 1024 * 1024);
    const auto content = Read(entry.path());
    ASSERT_FALSE(content.empty());
    EXPECT_EQ(content.back(), '\n');
    ++count;
  }
  EXPECT_EQ(count, 4u);
  EXPECT_NE(Read(LogPath()).find(std::string(32, 'x')), std::string::npos);
}

TEST_F(WindowsLogSinkTest, InvalidDirectoryFailsWithoutChangingTheFile) {
  fs::create_directories(directory_.parent_path());
  {
    std::ofstream sentinel(directory_);
    sentinel << "preserve";
  }
  rime::WindowsLogSink sink;
  EXPECT_FALSE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
  EXPECT_EQ(Read(directory_), "preserve");
}

TEST_F(WindowsLogSinkTest, ReparseDirectoryIsNotFollowed) {
  const auto target = directory_.parent_path() / L"target";
  fs::create_directories(target);
  std::error_code error;
  fs::create_directory_symlink(target, directory_, error);
  if (error)
    GTEST_SKIP() << "Directory symlink privilege is unavailable: "
                 << error.message();
  rime::WindowsLogSink sink;
  EXPECT_FALSE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
  EXPECT_TRUE(fs::is_empty(target));
}

TEST_F(WindowsLogSinkTest, HardLinkedLogIsNotModified) {
  fs::create_directories(directory_);
  const auto target = directory_ / L"preserve.txt";
  {
    std::ofstream stream(target);
    stream << "preserve";
  }
  fs::create_hard_link(target, LogPath());
  rime::WindowsLogSink sink;
  EXPECT_FALSE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
  EXPECT_EQ(Read(target), "preserve");
}

TEST_F(WindowsLogSinkTest, LockedHistoryKeepsCurrentLogAndRecoversAfterUnlock) {
  fs::create_directories(directory_);
  const std::string original(4 * 1024 * 1024, 'p');
  {
    std::ofstream current(LogPath(), std::ios::binary);
    current << original;
    std::ofstream(fs::path(LogPath().wstring() + L".previous.2"))
        << "history-two";
    std::ofstream(fs::path(LogPath().wstring() + L".previous.3"))
        << "history-three";
  }
  rime::WindowsLogSink sink;
  ASSERT_TRUE(sink.Configure(directory_.u8string().c_str(), "rime.bangke"));
  const HANDLE locked =
      CreateFileW((LogPath().wstring() + L".previous.3").c_str(), GENERIC_READ,
                  FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
  ASSERT_NE(locked, INVALID_HANDLE_VALUE);
  Send(sink, "blocked-rotation");
  EXPECT_EQ(Read(LogPath()), original);
  CloseHandle(locked);
  Send(sink, "recovered-rotation");
  EXPECT_NE(Read(LogPath()).find("recovered-rotation"), std::string::npos);
  EXPECT_EQ(Read(LogPath().wstring() + L".previous.1"), original);
}

}  // namespace

#endif  // defined(_WIN32) && defined(RIME_ENABLE_LOGGING)
