#ifndef RIME_POSIX_LOG_WRITER_H_
#define RIME_POSIX_LOG_WRITER_H_

#include <glog/logging.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <cstdint>
#include <deque>
#include <mutex>
#include <memory>
#include <typeinfo>
#include <string>

namespace rime {

struct PosixLogRegistration {
  google::base::Logger* original = nullptr;
  const std::type_info* stock_type = nullptr;
  std::atomic<google::base::Logger*> installed{nullptr};
};

struct PosixLogRegistrations {
  std::array<PosixLogRegistration, google::NUM_SEVERITIES> entries;
  ~PosixLogRegistrations() { Release(); }
  void Release() {
    for (size_t i = 0; i < entries.size(); ++i) {
      const auto severity = static_cast<google::LogSeverity>(i);
      auto& entry = entries[i];
      if (auto* installed = entry.installed.load()) {
        if (google::base::GetLogger(severity) == installed)
          google::base::SetLogger(severity, entry.original);
        // glog 0.6 replaces without deleting; 0.7 destroys the old writer and
        // clears this registration. Support both ownership implementations.
        if (entry.installed.load() == installed)
          delete installed;
      }
    }
  }
};

// Keep glog's formatting and severity routing, but avoid its second-resolution
// O_EXCL filenames. Frontends archive closed files; never rename an open file.
class PosixLogWriter final : public google::base::Logger {
 public:
  static constexpr size_t kLimit = 4 * 1024 * 1024;
  static constexpr size_t kGenerations = 2;

  PosixLogWriter(int directory,
                 std::string app,
                 google::LogSeverity severity,
                 PosixLogRegistration* registration = nullptr)
      : registration_(registration),
        directory_(directory),
        app_(std::move(app)),
        severity_(severity) {}
  ~PosixLogWriter() override {
    // Explicit glog shutdown or replacement invalidates the saved registration.
    if (registration_) {
      google::base::Logger* expected = this;
      registration_->installed.compare_exchange_strong(expected, nullptr);
    }
    Close();
    if (directory_ >= 0)
      close(directory_);
  }

  // The time_t overload is the public interface on glog 0.6. Modern glog
  // delegates to it too, preserving compatibility without version macros.
  void Write(bool force_flush,
             time_t timestamp,
             const char* data,
             size_t length) override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pid_ != getpid()) {
      Close();
      generations_.clear();
      pid_ = getpid();
    }
    // Keep ordinary records intact for the frontend's line-based archive.
    // Only an individual oversized record needs splitting to enforce the cap.
    if (fd_ >= 0 && length <= kLimit && length > kLimit - size_)
      Close();
    while (length) {
      if (fd_ < 0 && !Open(timestamp)) {
        Fallback(data, length);
        return;
      }
      if (!SafeActive()) {
        Report("active-identity", EACCES);
        Close();
        Fallback(data, length);
        return;
      }
      const auto amount = std::min(length, kLimit - size_);
      const auto written = write(fd_, data, amount);
      if (written < 0) {
        if (errno == EINTR)
          continue;
        Report("write", errno);
        Fallback(data, length);
        return;
      }
      if (!written) {
        Report("write", EIO);
        Fallback(data, length);
        return;
      }
      data += written;
      length -= written;
      size_ += written;
      if (size_ == kLimit)
        Close();
    }
    if (force_flush && fd_ >= 0 && fsync(fd_) < 0)
      Report("flush", errno);
  }

  void Flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    if (fd_ >= 0 && fsync(fd_) < 0)
      Report("flush", errno);
  }
  google::uint32 LogSize() override {
    std::lock_guard<std::mutex> lock(mutex_);
    return static_cast<google::uint32>(size_);
  }

  static bool Install(const char* directory, const char* app) {
    // Destroy this registry before unloading the engine, while shared glog is
    // still alive. Its stock Logger objects belong to glog, not this module.
    static PosixLogRegistrations registrations;
    // SetupLogging installs only after initializing glog. Repeat installation
    // may replace our own writers, but must not adopt another owner's custom
    // logger as a stock object that glog could delete on replacement.
    for (int i = 0; i < google::NUM_SEVERITIES; ++i) {
      auto& entry = registrations.entries[i];
      auto* current =
          google::base::GetLogger(static_cast<google::LogSeverity>(i));
      if (current != entry.installed.load() && entry.stock_type &&
          typeid(*current) != *entry.stock_type) {
        Report("foreign-logger", EBUSY);
        registrations.Release();
        return false;
      }
    }
    std::string selected = directory ? directory : FLAGS_log_dir;
    if (selected.empty()) {
      selected = "/tmp/rime-logs-" + std::to_string(geteuid());
      if (mkdir(selected.c_str(), 0700) < 0 && errno != EEXIST) {
        Report("create-directory", errno);
        return false;
      }
    }
    int root =
        open(selected.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (root < 0 || fstat(root, &st) < 0 || st.st_uid != geteuid() ||
        (st.st_mode & 0022)) {
      if (root >= 0)
        close(root);
      Report("directory", EACCES);
      return false;
    }
    std::string name = app ? app : "rime";
    for (char& c : name)
      if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_'))
        c = '_';
    name.resize(std::min(name.size(), size_t(80)));
    std::array<std::unique_ptr<PosixLogWriter>, google::NUM_SEVERITIES> writers;
    for (int severity = 0; severity < google::NUM_SEVERITIES; ++severity) {
      const int copy = fcntl(root, F_DUPFD_CLOEXEC, 0);
      if (copy < 0) {
        Report("duplicate-directory", errno);
        close(root);
        return false;
      }
      const auto level = static_cast<google::LogSeverity>(severity);
      writers[severity] = std::make_unique<PosixLogWriter>(
          copy, name, level, &registrations.entries[severity]);
    }
    // Allocate every descriptor before changing the process-wide logger set.
    registrations.Release();
    // Keep deployment cleanup and directory discovery on the actual backend.
    FLAGS_log_dir = selected;
    for (int severity = 0; severity < google::NUM_SEVERITIES; ++severity) {
      const auto level = static_cast<google::LogSeverity>(severity);
      auto& registration = registrations.entries[severity];
      registration.original = google::base::GetLogger(level);
      registration.stock_type = &typeid(*registration.original);
      auto* writer = writers[severity].release();
      registration.installed.store(writer);
      google::base::SetLogger(level, writer);
    }
    close(root);
    return true;
  }

 private:
  struct Generation {
    std::string name;
    dev_t device;
    ino_t inode;
  };
  static void Report(const char* stage, int code) {
    static std::atomic<unsigned long long> failures{0};
    std::fprintf(stderr, "rime logging failed; stage=%s code=%d count=%llu\n",
                 stage, code, ++failures);
  }
  static void Fallback(const char* data, size_t length) {
    while (length) {
      const auto n = write(STDERR_FILENO, data, length);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0)
        return;
      data += n;
      length -= n;
    }
  }
  bool SafeActive() const {
    struct stat st;
    return fstat(fd_, &st) == 0 && S_ISREG(st.st_mode) &&
           st.st_uid == geteuid() && st.st_nlink == 1 &&
           st.st_dev == active_.device && st.st_ino == active_.inode &&
           st.st_size == static_cast<off_t>(size_);
  }
  bool Open(time_t timestamp) {
    char date[32];
    struct tm local;
    localtime_r(&timestamp, &local);
    strftime(date, sizeof(date), "%Y%m%d-%H%M%S", &local);
    for (int tries = 0; tries < 128; ++tries) {
      const std::string name = app_ + "." + std::to_string(sequence_++) + "." +
                               google::GetLogSeverityName(severity_) + "." +
                               date + "." + std::to_string(getpid()) + ".log";
      fd_ = openat(directory_, name.c_str(),
                   O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (fd_ < 0) {
        if (errno == EEXIST)
          continue;
        Report("open", errno);
        return false;
      }
      struct stat st;
      if (fstat(fd_, &st) < 0 || !S_ISREG(st.st_mode) ||
          st.st_uid != geteuid() || st.st_nlink != 1) {
        Report("new-identity", EACCES);
        Close();
        return false;
      }
      if (flock(fd_, LOCK_EX | LOCK_NB) < 0) {
        Report("lock-new", errno);
        Close();
        return false;
      }
      active_ = {name, st.st_dev, st.st_ino};
      size_ = 0;
      generations_.push_back(active_);
      // Share the frontend's maintenance lock without waiting under glog's
      // global mutex. Busy maintenance is a counted retention exception.
#ifdef __APPLE__
      const char* lock_name = ".archive.lock";
#else
      const char* lock_name = "maintenance.lock";
#endif
      int maintenance = -1;
      if (generations_.size() > kGenerations) {
        maintenance = openat(directory_, lock_name,
                             O_WRONLY | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
        struct stat lock_info;
        if (maintenance < 0 || fstat(maintenance, &lock_info) < 0 ||
            !S_ISREG(lock_info.st_mode) || lock_info.st_uid != geteuid() ||
            lock_info.st_nlink != 1 ||
            flock(maintenance, LOCK_EX | LOCK_NB) < 0) {
          if (maintenance >= 0)
            close(maintenance);
          Report("retain-maintenance-busy-or-unsafe", EACCES);
          generations_.pop_front();
          return true;
        }
      }
      while (generations_.size() > kGenerations) {
        const auto old = generations_.front();
        generations_.pop_front();
        const int old_fd = openat(directory_, old.name.c_str(),
                                  O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        if (old_fd < 0) {
          if (errno != ENOENT)
            Report("retain-uninspectable-generation", errno);
          continue;
        }
        struct stat current, named;
        if (fstat(old_fd, &current) == 0 && S_ISREG(current.st_mode) &&
            current.st_uid == geteuid() && current.st_nlink == 1 &&
            current.st_dev == old.device && current.st_ino == old.inode &&
            flock(old_fd, LOCK_EX | LOCK_NB) == 0 &&
            fstatat(directory_, old.name.c_str(), &named,
                    AT_SYMLINK_NOFOLLOW) == 0 &&
            named.st_dev == current.st_dev && named.st_ino == current.st_ino &&
            named.st_nlink == 1) {
          if (unlinkat(directory_, old.name.c_str(), 0) < 0 && errno != ENOENT)
            Report("expire-generation", errno);
        } else {
          Report("retain-unsafe-or-busy-generation", EACCES);
        }
        close(old_fd);
      }
      if (maintenance >= 0)
        close(maintenance);
      return true;
    }
    Report("unique-name", EEXIST);
    return false;
  }
  void Close() {
    if (fd_ >= 0) {
      if (fsync(fd_) < 0)
        Report("close-flush", errno);
      if (close(fd_) < 0)
        Report("close", errno);
      fd_ = -1;
    }
  }

  PosixLogRegistration* registration_;
  int directory_;
  std::string app_;
  google::LogSeverity severity_;
  pid_t pid_ = getpid();
  int fd_ = -1;
  size_t size_ = 0;
  uint64_t sequence_ = 0;
  Generation active_;
  std::deque<Generation> generations_;
  std::mutex mutex_;
};

}  // namespace rime
#endif  // RIME_POSIX_LOG_WRITER_H_
