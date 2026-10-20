// quantd — 交易日历。
// 内置 2025-2026 年三市场节假日（依据历年交易所安排整理，
// 生产环境应以交易所正式公告或专业日历数据源为准）。
#pragma once

#include "core/types.hpp"

namespace quantd {

// 该市场该日是否交易（周末 + 节假日判断）
bool is_trading_day(Market m, TradingDay day);

// 从 start（含）起第 n 个交易日（n 从 1 开始）
TradingDay nth_trading_day(Market m, TradingDay start, int n);

// 推进到下一个交易日
TradingDay next_trading_day(Market m, TradingDay day);

}  // namespace quantd
