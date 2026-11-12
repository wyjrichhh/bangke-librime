#include <rime/build_config.h>
#if defined(RIME_ENABLE_LOGGING) && !defined(_WIN32)
#include <gtest/gtest.h>
#include <rime/posix_log_writer.h>
#include <filesystem>
#include <fstream>
#include <vector>
#include <sys/resource.h>

namespace {
namespace fs = std::filesystem;
class PosixLogWriterTest : public testing::Test {
 protected:
  void SetUp() override {
    char name[] = "/tmp/rime-writer-test-XXXXXX";
    const char* result = mkdtemp(name);
    ASSERT_NE(result, nullptr);
    directory = result;
  }
  void TearDown() override { fs::remove_all(directory); }
  std::unique_ptr<rime::PosixLogWriter> Writer() {
    return std::make_unique<rime::PosixLogWriter>(
        open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC),
        "rime.fcitx-bangke.test", google::GLOG_INFO);
  }
  std::vector<fs::path> Files() {
    std::vector<fs::path> paths;
    for (const auto& entry : fs::directory_iterator(directory))
      if (entry.path().extension() == ".log")
        paths.push_back(entry.path());
    std::sort(paths.begin(), paths.end());
    return paths;
  }
  std::string Read(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), {}};
  }
  fs::path directory;
};

TEST_F(PosixLogWriterTest, SameSecondRolloverPreservesEveryByteAndLastRecord) {
  auto writer = Writer();
  std::string expected;
  const auto timestamp = time(nullptr);
  for (int i = 0; i < 24; ++i) {
    std::string chunk(256 * 1024, char('a' + i));
    expected += chunk;
    writer->Write(false, timestamp, chunk.data(), chunk.size());
  }
  const std::string last = "last-record\n";
  expected += last;
  writer->Write(true, timestamp, last.data(), last.size());
  writer.reset();
  const auto files = Files();
  ASSERT_EQ(files.size(), 2u);
  std::string actual;
  for (const auto& file : files) {
    EXPECT_LE(fs::file_size(file), rime::PosixLogWriter::kLimit);
    actual += Read(file);
  }
  EXPECT_EQ(actual, expected);
}

TEST_F(PosixLogWriterTest, NormalRecordsRemainWholeAcrossRollover) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  const std::string first(rime::PosixLogWriter::kLimit - 5, 'a');
  const std::string last = "last-line\n";
  writer->Write(false, timestamp, first.data(), first.size());
  writer->Write(true, timestamp, last.data(), last.size());
  writer.reset();
  const auto files = Files();
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(Read(files[0]), first);
  EXPECT_EQ(Read(files[1]), last);
}

TEST_F(PosixLogWriterTest, RetentionKeepsTheNewestTwoBoundedGenerations) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  for (int i = 0; i < 4; ++i) {
    const std::string chunk(rime::PosixLogWriter::kLimit, char('a' + i));
    writer->Write(true, timestamp, chunk.data(), chunk.size());
  }
  writer.reset();
  const auto files = Files();
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(Read(files[0]), std::string(rime::PosixLogWriter::kLimit, 'c'));
  EXPECT_EQ(Read(files[1]), std::string(rime::PosixLogWriter::kLimit, 'd'));
}

TEST_F(PosixLogWriterTest, RetentionPreservesHardlinkedExternalBytesAndMode) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  const std::string chunk(rime::PosixLogWriter::kLimit, 'a');
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  const auto first = Files().front();
  const auto external = directory / "external";
  ASSERT_EQ(link(first.c_str(), external.c_str()), 0);
  ASSERT_EQ(chmod(external.c_str(), 0640), 0);
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  writer->Write(true, timestamp, "last", 4);
  struct stat st;
  ASSERT_EQ(stat(external.c_str(), &st), 0);
  EXPECT_EQ(st.st_mode & 0777, 0640);
  EXPECT_EQ(st.st_nlink, 2u);
  EXPECT_EQ(Read(external), chunk);
  EXPECT_TRUE(fs::exists(first));
}

TEST_F(PosixLogWriterTest, RetentionPreservesLockedGeneration) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  const std::string chunk(rime::PosixLogWriter::kLimit, 'a');
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  const auto first = Files().front();
  const int busy = open(first.c_str(), O_RDONLY | O_CLOEXEC);
  ASSERT_GE(busy, 0);
  ASSERT_EQ(flock(busy, LOCK_EX | LOCK_NB), 0);
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  writer->Write(true, timestamp, "last", 4);
  EXPECT_TRUE(fs::exists(first));
  EXPECT_EQ(Read(first), chunk);
  close(busy);
}

TEST_F(PosixLogWriterTest, FrontendMaintenanceDoesNotBlockOrLoseBusyFiles) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  const std::string chunk(rime::PosixLogWriter::kLimit, 'a');
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  const auto first = Files().front();
#ifdef __APPLE__
  const auto lock_path = directory / ".archive.lock";
#else
  const auto lock_path = directory / "maintenance.lock";
#endif
  const int lock =
      open(lock_path.c_str(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
  ASSERT_GE(lock, 0);
  ASSERT_EQ(flock(lock, LOCK_EX | LOCK_NB), 0);
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  writer->Write(true, timestamp, "last", 4);
  EXPECT_EQ(Read(first), chunk);
  EXPECT_EQ(Files().size(), 3u);
  close(lock);
}

TEST_F(PosixLogWriterTest, RetentionPreservesSymlinkAndReplacedForeignFile) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  const std::string chunk(rime::PosixLogWriter::kLimit, 'a');
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  const auto first = Files().front();
  const auto external = directory / "external";
  {
    std::ofstream out(external);
    out << "external-bytes";
  }
  ASSERT_EQ(unlink(first.c_str()), 0);
  ASSERT_EQ(symlink(external.c_str(), first.c_str()), 0);
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  const auto second = Files().back();
  fs::rename(second, directory / "saved-original");
  {
    std::ofstream out(second);
    out << "foreign-bytes";
  }
  writer->Write(true, timestamp, chunk.data(), chunk.size());
  writer->Write(true, timestamp, "last", 4);
  EXPECT_TRUE(fs::is_symlink(first));
  EXPECT_EQ(Read(external), "external-bytes");
  EXPECT_EQ(Read(second), "foreign-bytes");
}

TEST_F(PosixLogWriterTest, NullDirectoryUsesGlogEnvironmentSelection) {
  EXPECT_EXIT(
      {
        FLAGS_log_dir = directory.string();
        if (!rime::PosixLogWriter::Install(nullptr, "rime.fcitx-bangke.test"))
          std::exit(1);
        auto* logger = google::base::GetLogger(google::GLOG_INFO);
        const auto timestamp = time(nullptr);
        const std::string chunk(1024, 'a');
        for (int i = 0; i < 6144; ++i)
          static_cast<rime::PosixLogWriter*>(logger)->Write(
              false, timestamp, chunk.data(), chunk.size());
        logger->Flush();
        std::exit(0);
      },
      testing::ExitedWithCode(0), "");
  const auto files = Files();
  ASSERT_EQ(files.size(), 2u);
  EXPECT_EQ(Read(files[0]) + Read(files[1]), std::string(6 * 1024 * 1024, 'a'));
}

class ForeignLogger final : public google::base::Logger {
 public:
  void Write(bool, time_t, const char*, size_t) override {}
  void Flush() override {}
  google::uint32 LogSize() override { return 0; }
};

TEST_F(PosixLogWriterTest, RepeatInstallAndInvalidPathPreserveOwnership) {
  EXPECT_EXIT(
      {
        const auto path = directory.string();
        if (!rime::PosixLogWriter::Install(path.c_str(), "rime.repeat"))
          std::exit(1);
        auto* first = google::base::GetLogger(google::GLOG_INFO);
        const auto missing = (directory / "missing").string();
        if (rime::PosixLogWriter::Install(missing.c_str(), "rime.repeat") ||
            google::base::GetLogger(google::GLOG_INFO) != first)
          std::exit(2);
        if (!rime::PosixLogWriter::Install(path.c_str(), "rime.repeat"))
          std::exit(3);
        auto* second = google::base::GetLogger(google::GLOG_INFO);
        if (second == first)
          std::exit(4);
        static_cast<rime::PosixLogWriter*>(second)->Write(true, time(nullptr),
                                                          "after-repeat", 12);
        std::exit(0);
      },
      testing::ExitedWithCode(0), "stage=directory");
  const auto files = Files();
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(Read(files.front()), "after-repeat");
}

TEST_F(PosixLogWriterTest, PartialDescriptorFailureKeepsThePreviousWriters) {
  EXPECT_EXIT(
      {
        const auto path = directory.string();
        if (!rime::PosixLogWriter::Install(path.c_str(), "rime.repeat"))
          std::exit(1);
        auto* previous = google::base::GetLogger(google::GLOG_INFO);
        struct rlimit original;
        if (getrlimit(RLIMIT_NOFILE, &original) != 0)
          std::exit(2);
        int highest = 0;
        for (const auto& entry : fs::directory_iterator("/dev/fd"))
          highest = std::max(highest, std::stoi(entry.path().filename()));
        std::vector<int> fillers;
        int descriptor;
        do {
          descriptor = open("/dev/null", O_RDONLY | O_CLOEXEC);
          if (descriptor < 0)
            std::exit(3);
          fillers.push_back(descriptor);
        } while (descriptor <= highest);
        struct rlimit limited = original;
        // Leave room for the directory and exactly one duplicate descriptor.
        limited.rlim_cur = descriptor + 3;
        if (setrlimit(RLIMIT_NOFILE, &limited) != 0)
          std::exit(4);
        const bool installed =
            rime::PosixLogWriter::Install(path.c_str(), "rime.repeat");
        if (setrlimit(RLIMIT_NOFILE, &original) != 0)
          std::exit(5);
        for (const int fd : fillers)
          close(fd);
        if (installed || google::base::GetLogger(google::GLOG_INFO) != previous)
          std::exit(6);
        static_cast<rime::PosixLogWriter*>(previous)->Write(
            true, time(nullptr), "after-partial-failure", 21);
        if (!rime::PosixLogWriter::Install(path.c_str(), "rime.repeat"))
          std::exit(7);
        std::exit(0);
      },
      testing::ExitedWithCode(0), "stage=duplicate-directory");
  const auto files = Files();
  ASSERT_EQ(files.size(), 1u);
  EXPECT_EQ(Read(files.front()), "after-partial-failure");
}

TEST_F(PosixLogWriterTest, RepeatInstallDoesNotAdoptAnExternalReplacement) {
  EXPECT_EXIT(
      {
        const auto path = directory.string();
        if (!rime::PosixLogWriter::Install(path.c_str(), "rime.repeat"))
          std::exit(1);
        auto* foreign = new ForeignLogger;
        google::base::SetLogger(google::GLOG_INFO, foreign);
        if (rime::PosixLogWriter::Install(path.c_str(), "rime.repeat") ||
            google::base::GetLogger(google::GLOG_INFO) != foreign)
          std::exit(2);
        std::exit(0);
      },
      testing::ExitedWithCode(0), "stage=foreign-logger");
}

TEST_F(PosixLogWriterTest, ActiveHardlinkIsNotAppended) {
  auto writer = Writer();
  const auto timestamp = time(nullptr);
  writer->Write(true, timestamp, "first", 5);
  const auto first = Files().front();
  const auto external = directory / "external";
  ASSERT_EQ(link(first.c_str(), external.c_str()), 0);
  writer->Write(true, timestamp, "rejected", 8);
  EXPECT_EQ(Read(external), "first");
}
}  // namespace
#endif
