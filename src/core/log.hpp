// quantd — 分级日志。低频场景同步输出到 stdout 足够；
// 接实盘时可扩展为异步双缓冲 + 按天滚动文件。
#pragma once

#include <cstdint>
#include <format>
#include <iostream>
#include <string_view>

namespace quantd {

enum class LogLevel : uint8_t { Debug, Info, Warn, Error };

inline LogLevel g_log_level = LogLevel::Info;

inline bool log_enabled(LogLevel lv) { return lv >= g_log_level; }

inline void log_write(LogLevel lv, std::string_view msg) {
  static constexpr std::string_view kTags[] = {"DEBUG", "INFO ", "WARN ", "ERROR"};
  std::cout << '[' << kTags[static_cast<uint8_t>(lv)] << "] " << msg << '\n';
}

}  // namespace quantd

// 用法: QD_INFO("成交 {} {}股 @{}", symbol, qty, price);
#define QD_LOG(lv, ...)                        \
  do {                                         \
    if (quantd::log_enabled(lv))               \
      quantd::log_write(lv, std::format(__VA_ARGS__)); \
  } while (0)

#define QD_DEBUG(...) QD_LOG(quantd::LogLevel::Debug, __VA_ARGS__)
#define QD_INFO(...)  QD_LOG(quantd::LogLevel::Info,  __VA_ARGS__)
#define QD_WARN(...)  QD_LOG(quantd::LogLevel::Warn,  __VA_ARGS__)
#define QD_ERROR(...) QD_LOG(quantd::LogLevel::Error, __VA_ARGS__)
