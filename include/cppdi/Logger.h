#pragma once

#include <cstddef>
#include <iostream>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace cppdi {

enum class LogLevel {
    Info,
    Warn,
    Error,
};

/// Stable, human readable label for a `LogLevel`.
inline const char *logLevelName(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Info:
        return "INFO";
    case LogLevel::Warn:
        return "WARN";
    case LogLevel::Error:
        return "ERROR";
    }
    return "?";
}

/// Logging abstraction.
///
/// Like every dependency it is injected through `Dependencies`, so tests can
/// plug in `TestLogger` (which records messages) and production uses
/// `ConsoleLogger`.
class ILogger {
public:
    virtual ~ILogger() = default;

    /// Write a structurally-tagged message.
    virtual void log(LogLevel level, const std::string &message) = 0;

    void info(const std::string &message) { log(LogLevel::Info, message); }
    void warn(const std::string &message) { log(LogLevel::Warn, message); }
    void error(const std::string &message) { log(LogLevel::Error, message); }
};

/// Production logger that writes `[LEVEL] message` to `std::cout`.
/// Mutex-guarded, so interleaved writers never corrupt a line.
class ConsoleLogger final : public ILogger {
public:
    void log(LogLevel level, const std::string &message) override {
        std::lock_guard<std::mutex> lock(mutex);
        std::cout << '[' << logLevelName(level) << "] " << message << '\n';
    }

private:
    std::mutex mutex;
};

/// Test logger that records messages and offers thread-safe accessors, so
/// assertions can inspect exactly what the code under test logged.
class TestLogger final : public ILogger {
public:
    void log(LogLevel level, const std::string &message) override {
        std::ostringstream line;
        line << '[' << logLevelName(level) << "] " << message;

        std::lock_guard<std::mutex> lock(mutex);
        entries.push_back(line.str());
    }

    /// Snapshot of the recorded lines (thread-safe copy).
    [[nodiscard]] std::vector<std::string> copyLogs() const {
        std::lock_guard<std::mutex> lock(mutex);
        return entries;
    }

    /// Number of recorded lines.
    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mutex);
        return entries.size();
    }

    void clear() {
        std::lock_guard<std::mutex> lock(mutex);
        entries.clear();
    }

private:
    mutable std::mutex mutex;
    std::vector<std::string> entries;
};

/// Discards everything; useful to silence noisy components in tests.
class NullLogger final : public ILogger {
public:
    void log(LogLevel, const std::string &) override {}
};

} // namespace cppdi