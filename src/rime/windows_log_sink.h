#ifndef RIME_WINDOWS_LOG_SINK_H_
#define RIME_WINDOWS_LOG_SINK_H_

#include <glog/logging.h>
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <mutex>
#include <string>
#include <system_error>

namespace rime {

// glog's narrow CRT paths cannot represent every Windows profile directory.
// Closed generations are compressed by the frontend's log maintenance helper.
class WindowsLogSink final : public google::LogSink {
 public:
  ~WindowsLogSink() override {
    if (registered_)
      google::RemoveLogSink(this);
    Close();
  }

  bool Configure(const char* directory, const char* app_name) noexcept {
    try {
      std::lock_guard<std::mutex> lock(mutex_);
      const auto root = std::filesystem::u8path(directory);
      std::error_code error;
      std::filesystem::create_directories(root, error);
      if (error) {
        ReportFailure(L"create-directory", DWORD(error.value()));
        return false;
      }
      const DWORD attributes = GetFileAttributesW(root.c_str());
      if (attributes == INVALID_FILE_ATTRIBUTES ||
          !(attributes & FILE_ATTRIBUTE_DIRECTORY) ||
          (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) {
        ReportFailure(L"validate-directory", ERROR_ACCESS_DENIED);
        return false;
      }
      std::string name = app_name ? app_name : "rime";
      for (char& character : name) {
        const bool safe = (character >= 'a' && character <= 'z') ||
                          (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') ||
                          character == '.' || character == '-' ||
                          character == '_';
        if (!safe)
          character = '_';
      }
      name.resize((std::min)(name.size(), size_t(80)));
      path_ = root / std::filesystem::u8path(
                         name + "-engine-" +
                         std::to_string(GetCurrentProcessId()) + ".log");
      return Open();
    } catch (...) {
      ReportFailure(L"configure", ERROR_INVALID_DATA);
      return false;
    }
  }

  void Register() noexcept {
    try {
      google::AddLogSink(this);
      registered_ = true;
    } catch (...) {
      ReportFailure(L"register", ERROR_NOT_ENOUGH_MEMORY);
    }
  }

  void send(google::LogSeverity severity,
            const char*,
            const char* base_filename,
            int line,
            const google::LogMessageTime& time,
            const char* message,
            size_t message_length) override {
    // A sink runs under glog's locks: never call LOG or CHECK from here.
    try {
      auto record = google::LogSink::ToString(
          severity, base_filename, line, time, message,
          (std::min)(message_length, kRecordLimit));
      record += '\n';
      if (record.size() > kRecordLimit) {
        constexpr char suffix[] = " [truncated]\n";
        size_t end = kRecordLimit - sizeof(suffix);
        while (end && (static_cast<unsigned char>(record[end]) & 0xc0) == 0x80)
          --end;
        record.resize(end);
        record += suffix;
      }
      std::lock_guard<std::mutex> lock(mutex_);
      if (file_ == INVALID_HANDLE_VALUE && !Open())
        return;
      if (size_ + record.size() > kFileLimit && !Rotate())
        return;
      size_t offset = 0;
      while (offset < record.size()) {
        DWORD written = 0;
        const bool wrote = WriteFile(file_, record.data() + offset,
                                     DWORD(record.size() - offset), &written,
                                     nullptr) != FALSE;
        if (!wrote || written == 0) {
          const DWORD error = wrote ? ERROR_WRITE_FAULT : GetLastError();
          Close();
          ReportFailure(L"write", error);
          return;
        }
        offset += written;
        size_ += written;
      }
      if (severity >= google::GLOG_ERROR && !FlushFileBuffers(file_))
        ReportFailure(L"flush", GetLastError());
    } catch (...) {
      ReportFailure(L"format", ERROR_NOT_ENOUGH_MEMORY);
    }
  }

 private:
  static constexpr size_t kFileLimit = 4 * 1024 * 1024;
  static constexpr size_t kRecordLimit = 64 * 1024;

  bool Open() {
    file_ = CreateFileW(path_.c_str(), GENERIC_WRITE | FILE_READ_ATTRIBUTES,
                        FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                        nullptr);
    if (file_ == INVALID_HANDLE_VALUE) {
      ReportFailure(L"open", GetLastError());
      return false;
    }
    BY_HANDLE_FILE_INFORMATION info{};
    LARGE_INTEGER length{};
    if (!GetFileInformationByHandle(file_, &info) ||
        !GetFileSizeEx(file_, &length)) {
      const DWORD error = GetLastError();
      Close();
      ReportFailure(L"inspect-file", error);
      return false;
    }
    if ((info.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) ||
        info.nNumberOfLinks != 1 || GetFileType(file_) != FILE_TYPE_DISK) {
      Close();
      ReportFailure(L"validate-file", ERROR_ACCESS_DENIED);
      return false;
    }
    LARGE_INTEGER end{};
    if (!SetFilePointerEx(file_, end, nullptr, FILE_END)) {
      const DWORD error = GetLastError();
      Close();
      ReportFailure(L"seek", error);
      return false;
    }
    size_ = static_cast<unsigned long long>(length.QuadPart);
    return true;
  }

  bool Rotate() {
    Close();
    for (int index = 3; index >= 1; --index) {
      const auto from = index == 1 ? path_ : Previous(index - 1);
      const auto to = Previous(index);
      if (!MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND) {
          ReportFailure(L"rotate", error);
          return false;
        }
      }
    }
    return Open();
  }

  std::filesystem::path Previous(int index) const {
    auto previous = path_;
    previous += L".previous." + std::to_wstring(index);
    return previous;
  }

  void Close() noexcept {
    if (file_ != INVALID_HANDLE_VALUE) {
      if (!CloseHandle(file_))
        ReportFailure(L"close", GetLastError());
      file_ = INVALID_HANDLE_VALUE;
    }
  }

  static void ReportFailure(const wchar_t* stage, DWORD error) noexcept {
    wchar_t message[160];
    swprintf_s(message, L"Rime log storage: %ls failed (error=%lu)\n", stage,
               static_cast<unsigned long>(error));
    OutputDebugStringW(message);
    std::fprintf(stderr, "Rime log storage failed (error=%lu)\n",
                 static_cast<unsigned long>(error));
  }

  std::mutex mutex_;
  std::filesystem::path path_;
  HANDLE file_ = INVALID_HANDLE_VALUE;
  unsigned long long size_ = 0;
  bool registered_ = false;
};

}  // namespace rime

#endif  // RIME_WINDOWS_LOG_SINK_H_
