// quantd — 领域类型定义
// 所有跨模块传递的数据结构集中于此，保证单一事实来源。
#pragma once

#include <chrono>
#include <cstdint>
#include <format>
#include <string>

namespace quantd {

// 价格/数量。纸面阶段用 double 足够；接实盘前建议替换为定点整数
// （以最小变动单位计），彻底消除浮点误差。
using Price = double;
using Qty   = int64_t;

enum class Market : uint8_t { CN, HK, US };
enum class Side : uint8_t { Buy, Sell };
enum class OrderType : uint8_t { Market, Limit };
enum class Currency : uint8_t { CNY, HKD, USD };

enum class OrderStatus : uint8_t {
  PendingNew,      // 已提交，待网关确认
  Accepted,        // 已受理（限价单挂单中）
  Filled,          // 全部成交
  Cancelled,       // 已撤单（含日终自动撤单）
  Rejected,        // 被风控/网关拒绝
};

// 交易日期（本系统为日频低频交易）
using TradingDay = std::chrono::year_month_day;

inline std::string to_string(TradingDay d) {
  return std::format("{}-{:02d}-{:02d}", int(d.year()),
                     unsigned(d.month()), unsigned(d.day()));
}

inline std::string_view to_string(Side s) {
  return s == Side::Buy ? "BUY" : "SELL";
}
inline std::string_view to_string(OrderType t) {
  return t == OrderType::Market ? "MKT" : "LMT";
}
inline std::string_view to_string(Market m) {
  switch (m) {
    case Market::CN: return "CN";
    case Market::HK: return "HK";
    case Market::US: return "US";
  }
  return "?";
}
inline std::string_view to_string(Currency c) {
  switch (c) {
    case Currency::CNY: return "CNY";
    case Currency::HKD: return "HKD";
    case Currency::USD: return "USD";
  }
  return "?";
}

// 订单请求（策略 → 引擎）
struct OrderRequest {
  std::string symbol;
  Side side{};
  OrderType type{};
  Qty qty{};
  Price limit_price{};   // 限价单有效
};

// 订单（引擎内唯一编号，含状态）
struct Order {
  uint64_t id{};
  std::string symbol;
  Market market{};
  Side side{};
  OrderType type{};
  Qty qty{};
  Price limit_price{};
  OrderStatus status{OrderStatus::PendingNew};
  std::string note;      // 拒绝原因/备注
  TradingDay day{};      // 下单交易日
};

// 成交回报
struct Trade {
  uint64_t id{};
  uint64_t order_id{};
  std::string symbol;
  Market market{};
  Side side{};
  Qty qty{};
  Price price{};
  Price fee{};           // 该笔全费用
  Currency ccy{};
  TradingDay day{};
};

// 日线 K 线
struct Bar {
  std::string symbol;
  Market market{};
  TradingDay day{};
  Price open{}, high{}, low{}, close{};
  double volume{};       // 成交量（股）
};

// 持仓
struct Position {
  std::string symbol;
  Market market{};
  Currency ccy{};
  Qty total_qty{};       // 总持仓
  Qty available_qty{};   // 可卖数量（T+1 市场当日买入不可卖）
  Price avg_price{};     // 平均成本（FIFO 剩余加权）
};

}  // namespace quantd
