#include <rime/build_config.h>

#ifdef RIME_ENABLE_LOGGING

#include <gtest/gtest.h>
#include <cstdlib>
#include <sstream>
#include <rime/config.h>
#include <rime/dict/user_dictionary.h>
#include <rime/key_event.h>
#ifndef _WIN32
#include <rime/dict/entry_collector.h>
#endif

namespace {
constexpr char kPrivateText[] = "PRIVATE_INPUT_LOG_SENTINEL_4eb8";

class StderrCapture {
 public:
  StderrCapture() { testing::internal::CaptureStderr(); }
  ~StderrCapture() {
    if (active_)
      testing::internal::GetCapturedStderr();
  }
  std::string Read() {
    active_ = false;
    return testing::internal::GetCapturedStderr();
  }

 private:
  bool active_ = true;
};

TEST(RimeLogPrivacyTest, InvalidKeysKeepCategoryAndHideRawInput) {
  StderrCapture capture;
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
  StderrCapture capture;
  const auto entry = rime::UserDictionary::CreateDictEntry(
      std::string("a \t") + kPrivateText, std::string("c=") + kPrivateText, 1);
  const auto output = capture.Read();
  EXPECT_FALSE(entry);
  EXPECT_NE(output.find("metadata invalid; field=commits"), std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}

TEST(RimeLogPrivacyTest, InvalidYamlKeepsLocationWithoutQuotingTheDocument) {
  StderrCapture capture;
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
  StderrCapture capture;
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
