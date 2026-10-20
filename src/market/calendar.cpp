#include "market/calendar.hpp"

#include <set>

namespace quantd {

namespace {

// "MM-DD" 或节假日集合，按市场分年份收录。
// 说明：2026 年安排按历年规律推定（春节/清明等），若与交易所
// 正式公告有出入，请以公告为准并更新此表。
const std::set<std::string> kHolidaysCN = {
    // 2025
    "2025-01-01",
    "2025-01-28", "2025-01-29", "2025-01-30", "2025-01-31", "2025-02-03", "2025-02-04",
    "2025-04-04",
    "2025-05-01", "2025-05-02", "2025-05-05",
    "2025-05-31", "2025-06-02",
    "2025-10-01", "2025-10-02", "2025-10-03", "2025-10-06", "2025-10-07", "2025-10-08",
    // 2026
    "2026-01-01", "2026-01-02",
    "2026-02-16", "2026-02-17", "2026-02-18", "2026-02-19", "2026-02-20", "2026-02-23", "2026-02-24",
    "2026-04-06",
    "2026-05-01", "2026-05-04", "2026-05-05",
    "2026-06-19",
    "2026-09-25",
    "2026-10-01", "2026-10-02", "2026-10-05", "2026-10-06", "2026-10-07", "2026-10-08",
};

const std::set<std::string> kHolidaysHK = {
    // 2025
    "2025-01-01",
    "2025-01-29", "2025-01-30", "2025-01-31",
    "2025-04-04", "2025-04-07", "2025-04-18", "2025-04-21",
    "2025-05-01", "2025-05-06",
    "2025-06-02",
    "2025-07-01",
    "2025-10-01", "2025-10-02", "2025-10-29",
    "2025-12-25", "2025-12-26",
    // 2026
    "2026-01-01",
    "2026-02-16", "2026-02-17", "2026-02-18", "2026-02-19",
    "2026-04-03", "2026-04-06", "2026-04-07",
    "2026-05-01", "2026-05-25",
    "2026-06-19",
    "2026-07-01",
    "2026-10-01",
    "2026-12-25",
};

const std::set<std::string> kHolidaysUS = {
    // 2025
    "2025-01-01", "2025-01-09", "2025-01-20",
    "2025-02-17",
    "2025-04-18",
    "2025-05-26",
    "2025-06-19",
    "2025-07-04",
    "2025-09-01",
    "2025-11-27",
    "2025-12-25",
    // 2026
    "2026-01-01", "2026-01-19",
    "2026-02-16",
    "2026-04-03",
    "2026-05-25",
    "2026-06-19",
    "2026-07-03",
    "2026-09-07",
    "2026-11-26",
    "2026-12-25",
};

bool is_weekend(TradingDay d) {
  auto wd = std::chrono::weekday(d);
  return wd == std::chrono::Sunday || wd == std::chrono::Saturday;
}

}  // namespace

bool is_trading_day(Market m, TradingDay day) {
  if (is_weekend(day)) return false;
  const std::set<std::string>* table = nullptr;
  switch (m) {
    case Market::CN: table = &kHolidaysCN; break;
    case Market::HK: table = &kHolidaysHK; break;
    case Market::US: table = &kHolidaysUS; break;
  }
  return !table->contains(to_string(day));
}

TradingDay next_trading_day(Market m, TradingDay day) {
  auto d = std::chrono::year_month_day{std::chrono::sys_days{day} + std::chrono::days{1}};
  while (!is_trading_day(m, d)) {
    d = std::chrono::year_month_day{std::chrono::sys_days{d} + std::chrono::days{1}};
  }
  return d;
}

TradingDay nth_trading_day(Market m, TradingDay start, int n) {
  auto d = start;
  // 先对齐到 start 本身（若 start 是交易日则从 start 起算）
  int cnt = is_trading_day(m, d) ? 1 : 0;
  while (cnt < n) {
    d = next_trading_day(m, d);
    ++cnt;
  }
  return d;
}

}  // namespace quantd
