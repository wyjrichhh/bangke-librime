#include <rime/build_config.h>

#ifdef RIME_ENABLE_LOGGING

#include <gtest/gtest.h>
#include <cstdlib>
#include <sstream>
#include <rime/config.h>
#include <rime/dict/user_dictionary.h>
#include <rime/key_event.h>
#include <initializer_list>
#ifdef _WIN32
#include <vector>
#endif
#ifndef _WIN32
#include <rime/dict/entry_collector.h>
#include <rime/dict/preset_vocabulary.h>
#include <fstream>
#include <unistd.h>
#endif

namespace {
constexpr char kPrivateText[] = "PRIVATE_INPUT_LOG_SENTINEL_4eb8";

#ifdef _WIN32
class RequiredDiagnostics final
    : public testing::MatcherInterface<const std::string&> {
 public:
  explicit RequiredDiagnostics(std::initializer_list<const char*> tokens)
      : tokens_(tokens.begin(), tokens.end()) {}

  bool MatchAndExplain(const std::string& output,
                       testing::MatchResultListener* listener) const override {
    if (output.find(kPrivateText) != std::string::npos) {
      *listener << "contains the private input sentinel";
      return false;
    }
    for (const auto& token : tokens_) {
      if (output.find(token) == std::string::npos) {
        *listener << "is missing diagnostic: " << token;
        return false;
      }
    }
    return true;
  }

  void DescribeTo(std::ostream* stream) const override {
    *stream
        << "contains every required diagnostic and no private input sentinel";
  }

 private:
  std::vector<std::string> tokens_;
};

template <class Action>
void ExpectDiagnostic(Action action,
                      std::initializer_list<const char*> tokens) {
  const testing::Matcher<const std::string&> matcher(
      new RequiredDiagnostics(tokens));
  // GTest starts the child with an OS stderr pipe before either CRT
  // initializes.
  EXPECT_EXIT(std::exit(action() ? 0 : 1), testing::ExitedWithCode(0), matcher);
}
#else
class DiagnosticCapture {
 public:
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
};

template <class Action>
void ExpectDiagnostic(Action action,
                      std::initializer_list<const char*> tokens) {
  DiagnosticCapture capture;
  EXPECT_TRUE(action());
  const auto output = capture.Read();
  for (const auto* token : tokens)
    EXPECT_NE(output.find(token), std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
}
#endif

TEST(RimeLogPrivacyTest, InvalidKeysKeepCategoryAndHideRawInput) {
  ExpectDiagnostic(
      [] {
        rime::KeyEvent event;
        const bool modifier = event.Parse(std::string(kPrivateText) + "+a");
        const bool key = event.Parse(kPrivateText);
        return !modifier && !key;
      },
      {"key parse failed; type=unknown-modifier",
       "key parse failed; type=unknown-key"});
}

TEST(RimeLogPrivacyTest, UserDictionaryMetadataHidesTheRejectedValue) {
  ExpectDiagnostic(
      [] {
        return !rime::UserDictionary::CreateDictEntry(
            std::string("a \t") + kPrivateText,
            std::string("c=") + kPrivateText, 1);
      },
      {"metadata invalid; field=commits"});
}

TEST(RimeLogPrivacyTest, InvalidYamlKeepsLocationWithoutQuotingTheDocument) {
  ExpectDiagnostic(
      [] {
        rime::Config config;
        std::istringstream source(std::string("value: [") + kPrivateText);
        return !config.LoadFromStream(source);
      },
      {"config load failed; stage=parse line="});
}

#ifndef _WIN32
// EntryCollector is an internal, non-exported class in the Windows DLL.
class RejectingCollector : public rime::EntryCollector {
 public:
  void RejectPendingPhrase(const std::string& phrase, size_t count = 1) {
    encoder.reset(new RejectingEncoder(this));
    for (size_t i = 0; i < count; ++i)
      encode_queue.emplace(phrase, "1");
    Finish();
  }

  void RejectPreset(const std::string& filename) {
    encoder.reset(new RejectingEncoder(this));
    preset_vocabulary.reset(new rime::PresetVocabulary(filename));
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

TEST(RimeLogPrivacyTest, BulkQueueReportsOneCompletePrivateSummary) {
  DiagnosticCapture capture;
  RejectingCollector collector;
  collector.RejectPendingPhrase(kPrivateText, 150309);
  const auto output = capture.Read();
  const std::string category = "dictionary encode failed; source=queue";
  const auto position = output.find(category);
  ASSERT_NE(position, std::string::npos);
  EXPECT_EQ(output.find(category, position + category.size()),
            std::string::npos);
  EXPECT_NE(output.find("attempted=150309 failed=150309"), std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
  EXPECT_LT(output.size(), 4096u);
}

TEST(RimeLogPrivacyTest, BulkPresetReportsOneCompletePrivateSummary) {
  char filename[] = "/tmp/rime-preset-test-XXXXXX.txt";
  const int fd = mkstemps(filename, 4);
  ASSERT_GE(fd, 0);
  close(fd);
  struct RemoveFixture {
    const char* filename;
    ~RemoveFixture() { unlink(filename); }
  } cleanup{filename};
  {
    std::ofstream source(filename);
    ASSERT_TRUE(source.good());
    for (size_t i = 0; i < 150309; ++i)
      source << kPrivateText << i << "\t1\n";
    ASSERT_TRUE(source.good());
  }
  DiagnosticCapture capture;
  RejectingCollector collector;
  std::string resource(filename);
  resource.resize(resource.size() - 4);
  collector.RejectPreset(resource);
  const auto output = capture.Read();
  const std::string category = "dictionary encode failed; source=preset";
  const auto position = output.find(category);
  ASSERT_NE(position, std::string::npos);
  EXPECT_EQ(output.find(category, position + category.size()),
            std::string::npos);
  EXPECT_NE(output.find("attempted=150309 failed=150309"), std::string::npos);
  EXPECT_EQ(output.find(kPrivateText), std::string::npos);
  EXPECT_LT(output.size(), 4096u);
}

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
