// quantd — 组合与账户管理。
// 多币种现金 + 持仓 + FIFO 平仓盈亏 + T+1 可卖数量管理。
// 所有金额按币种独立记账，汇报时经汇率折算为 CNY。
#pragma once

#include <map>
#include <string>
#include <vector>

#include "core/types.hpp"

namespace quantd {

struct RoundTrip {
  std::string symbol;
  Side open_side{};
  Qty qty{};
  Price open_price{}, close_price{};
  Price realized_pnl{};   // 本币，已扣费用（简化：费用在成交时已从现金扣除，
                          // 此处为毛差；净差见 total_realized_pnl）
  TradingDay open_day{}, close_day{};
};

class Portfolio {
 public:
  void init_capital(Currency c, double amount) { cash_[c] = amount; }

  void set_fx(double hkd_cny, double usd_cny) {
    hkd_cny_ = hkd_cny;
    usd_cny_ = usd_cny;
  }

  // 处理成交：更新持仓/现金/FIFO 队列。
  // 若违反可卖数量（T+1 或空头）返回 false 并写明原因（正常流程
  // 中网关已拦截，此处为最后防线）。
  bool on_trade(const Trade& t, std::string* why = nullptr);

  // 日终结算：T+1 市场当日买入解冻为可卖
  void settle();

  void mark(const std::string& symbol, Price last) { marks_[symbol] = last; }

  // ---- 查询 ----
  const Position* position(const std::string& symbol) const;
  double cash(Currency c) const;
  Price last_mark(const std::string& symbol) const;

  double positions_value_cny() const;      // 持仓市值折CNY
  double total_equity_cny() const;         // 总权益折CNY（现金+市值）
  double unrealized_pnl_cny() const;       // 浮动盈亏折CNY
  double total_realized_pnl_cny() const;   // FIFO 累计已实现盈亏折CNY（净）

  const std::map<std::string, Position>& positions() const { return positions_; }
  const std::vector<RoundTrip>& round_trips() const { return trips_; }

  // 跨币种金额换算（按配置汇率）
  double convert(double amount, Currency from, Currency to) const;

  // ---- 实盘对账：以券商回报为事实源重建账户 ----
  void bootstrap_cash(Currency c, double amount);   // 覆盖现金
  void bootstrap_position(const Position& p);       // 覆盖/新增持仓

 private:
  struct Lot {  // FIFO 买入批次
    Qty qty{};
    Price price{};
    TradingDay day{};
  };

  std::map<std::string, Position> positions_;
  std::map<std::string, std::vector<Lot>> lots_;
  std::map<Currency, double> cash_;
  std::map<std::string, Price> marks_;
  std::vector<RoundTrip> trips_;
  double realized_by_ccy_[3]{};   // 按 Currency 下标累计已实现盈亏
  double hkd_cny_ = 0.92;
  double usd_cny_ = 7.1;
};

}  // namespace quantd
