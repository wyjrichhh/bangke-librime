#include <rime/build_config.h>

#ifdef RIME_ENABLE_LOGGING

#include <gtest/gtest.h>
#include <cstdlib>
#include <sstream>
#include <rime/config.h>
#include <rime/dict/user_dictionary.h>
#include <rime/key_event.h>
#ifdef _WIN32
#include <windows.h>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#endif
#ifndef _WIN32
#include <rime/dict/entry_collector.h>
#endif

namespace {
constexpr char kPrivateText[] = "PRIVATE_INPUT_LOG_SENTINEL_4eb8";

class DiagnosticCapture {
 public:
#ifdef _WIN32
  DiagnosticCapture() {
    const char* directory = std::getenv("GLOG_log_dir");
    if (!directory || !*directory)
      throw std::runtime_error("Run through CTest to use its log directory");
    directory_ = std::filesystem::u8path(directory);
    suffix_ = "." + std::to_string(GetCurrentProcessId()) + ".log";
    offsets_ = Snapshot();
  }

  std::string Read() {
    std::string output;
    for (const auto& entry : Snapshot()) {
      const auto found = offsets_.find(entry.first);
      const auto offset = found == offsets_.end() ? 0 : found->second;
      if (entry.second < offset)
        throw std::runtime_error("Diagnostic log shrank during capture");
      std::ifstream input(entry.first, std::ios::binary);
      if (!input || !input.seekg(static_cast<std::streamoff>(offset)))
        throw std::runtime_error("Cannot read current-process diagnostic log");
      std::string appended(static_cast<size_t>(entry.second - offset), '\0');
      if (!appended.empty() &&
          !input.read(appended.data(),
                      static_cast<std::streamsize>(appended.size())))
        throw std::runtime_error("Cannot read new diagnostic log bytes");
      output += appended;
    }
    return output;
  }

 private:
  std::map<std::filesystem::path, std::uintmax_t> Snapshot() const {
    std::map<std::filesystem::path, std::uintmax_t> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory_)) {
      const auto name = entry.path().filename().u8string();
      if (name.compare(0, 10, "rime.test.") == 0 &&
          name.size() >= suffix_.size() &&
          name.compare(name.size() - suffix_.size(), suffix_.size(), suffix_) ==
              0 &&
          !entry.is_symlink() && entry.is_regular_file())
        files.emplace(entry.path(), entry.file_size());
    }
    return files;
  }

  std::filesystem::path directory_;
  std::string suffix_;
  std::map<std::filesystem::path, std::uintmax_t> offsets_;
#else
  DiagnosticCapture() { testing::internal::CaptureStderr(); }
  ~DiagnosticCapture() {
    if (active_)
      testing::internal::GetCapturedStderr();
  }
  std::string Read() {
    active_ = false;
    return testing::internal::GetCapturedStderr();
  }

 private:
  bool active_ = true;
#endif
};

TEST(RimeLogPrivacyTest, InvalidKeysKeepCategoryAndHideRawInput) {
  DiagnosticCapture capture;
  rime::KeyEvent event;
  EXPECT_FALSE(event.Parse(std::string(kPrivateText) + "+a"));
  EXPECT_FALSE(event.Parse(kPrivateText));
  const auto output = capture.Read();
  EXPECT_NE(output.find("key parse failed; type=unknown-modifier"),
            std::string::npos);
  EXPECT_NE(output.find("key parse failed; type=unknown-key"),
            std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}

TEST(RimeLogPrivacyTest, UserDictionaryMetadataHidesTheRejectedValue) {
  DiagnosticCapture capture;
  const auto entry = rime::UserDictionary::CreateDictEntry(
      std::string("a \t") + kPrivateText, std::string("c=") + kPrivateText, 1);
  const auto output = capture.Read();
  EXPECT_FALSE(entry);
  EXPECT_NE(output.find("metadata invalid; field=commits"), std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}

TEST(RimeLogPrivacyTest, InvalidYamlKeepsLocationWithoutQuotingTheDocument) {
  DiagnosticCapture capture;
  rime::Config config;
  std::istringstream source(std::string("value: [") + kPrivateText);
  EXPECT_FALSE(config.LoadFromStream(source));
  const auto output = capture.Read();
  EXPECT_NE(output.find("config load failed; stage=parse line="),
            std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}

#ifndef _WIN32
// EntryCollector is an internal, non-exported class in the Windows DLL.
class RejectingCollector : public rime::EntryCollector {
 public:
  void RejectPendingPhrase(const std::string& phrase) {
    encoder.reset(new RejectingEncoder(this));
    encode_queue.emplace(phrase, "1");
    Finish();
  }

 private:
  class RejectingEncoder : public rime::Encoder {
   public:
    using rime::Encoder::Encoder;
    bool EncodePhrase(const std::string&, const std::string&) override {
      return false;
    }
  };
};

TEST(RimeLogPrivacyTest, DictionaryErrorsAndWarningsHideWordsAndCodes) {
  const char* enabled = std::getenv("GLOG_alsologtostderr");
  if (!enabled || std::string(enabled) != "1")
    GTEST_SKIP() << "Run through CTest to capture warning diagnostics";
  DiagnosticCapture capture;
  rime::EntryCollector duplicate;
  duplicate.CreateEntry(kPrivateText, "a", "1");
  duplicate.CreateEntry(kPrivateText, "a", "1");
  rime::EntryCollector fixed(rime::Syllabary{"a"});
  fixed.CreateEntry(kPrivateText, kPrivateText, "1");
  RejectingCollector failing;
  failing.RejectPendingPhrase(kPrivateText);
  const auto output = capture.Read();
  EXPECT_EQ(duplicate.entries.size(), 1u);
  EXPECT_TRUE(fixed.entries.empty());
  EXPECT_NE(output.find("type=duplicate"), std::string::npos);
  EXPECT_NE(output.find("type=unknown-syllable"), std::string::npos);
  EXPECT_NE(output.find("dictionary encode failed; source=queue"),
            std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}
#endif

}  // namespace

#endif  // RIME_ENABLE_LOGGING
