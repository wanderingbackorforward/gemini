// quantd — 市场规则抽象。
// 把三个市场所有微观结构差异收敛到这一个模块，其余代码只面向
// MarketRules 编程，新增市场（如期货期权）只需扩展此处的表。
#pragma once

#include <optional>
#include <string>

#include "core/types.hpp"

namespace quantd {

struct FeeSchedule {
  double commission_bps{};   // 佣金（双边，万分比）
  double min_commission{};   // 最低佣金（本币）
  double stamp_sell_bps{};   // 印花税（仅卖出，万分比）
  double transfer_bps{};     // 过户费/交易征费等杂费（双边，万分比）
};

struct MarketRules {
  Market market{};
  std::string name;          // "A股" / "港股" / "美股"
  Currency ccy{};
  bool t_plus_one{};         // A股 T+1；港美股 T+0
  double price_limit_pct{};  // 涨跌停幅度（0 表示无限制）
  int default_lot{};         // 默认每手股数
  FeeSchedule fees{};

  // 价格最小变动单位。港股按价位表分档（此处为简化分档，
  // 生产环境应以交易所官方价位表为准）；A股/美股统一 0.01。
  Price tick_size(Price p) const;

  // 按名义金额计算全费用（本币）
  Price trade_fee(Side side, Price notional) const;

  // 标的后缀 → 市场：.SH/.SZ/.BJ → CN；.HK → HK；.US → US
  static std::optional<Market> market_of(const std::string& symbol);
  static std::optional<std::string_view> market_suffix_error(const std::string& symbol);

  // 某标的的每手股数（港股因股票而异，内置常见标的表，其余取默认值）
  static int lot_of(const std::string& symbol);

  static const MarketRules& of(Market m);
};

// 将名义金额折算为 CNY（汇率来自配置，生产应接实时汇率源）
double to_cny(double amount, Currency ccy, double hkd_cny, double usd_cny);

}  // namespace quantd
