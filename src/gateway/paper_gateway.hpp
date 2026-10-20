// quantd — 纸面交易网关（模拟撮合）。
// 撮合模型（对低频日线系统足够严谨，且无前视偏差）：
//   1. bar 日产生的市价单 → 次日开盘价 ± 滑点成交
//   2. 限价单 → 次日起逐日检查：开盘优于限价按开盘成交，
//      否则日内 high/low 触及限价按限价成交
//   3. 所有挂单 GFD（当日收盘自动撤销）
// 成交前会做可卖数量（T+1）与现金余额的最终防线校验。
#pragma once

#include <map>
#include <random>
#include <vector>

#include "gateway/gateway.hpp"
#include "portfolio/portfolio.hpp"

namespace quantd {

class PaperGateway : public IGateway {
 public:
  PaperGateway(EventBus& bus, Portfolio& pf)
      : bus_(bus), pf_(pf) {}

  std::string name() const override { return "paper"; }
  bool connect() override { return true; }

  void place(Order& o, Price ref_price) override;
  void cancel(uint64_t order_id) override;
  Qty pending_qty(const std::string& symbol, Side side) const override;

  void on_session_open(TradingDay day) override;
  void on_bar(const Bar& bar) override;
  void on_session_close(TradingDay day) override;

  // 滑点（按基点设置，如 5 = 0.05%）
  void set_slippage_bps(double bps) { slip_bps_ = bps; }

 private:
  void fill(Order& o, Price px, TradingDay day);

  EventBus& bus_;
  Portfolio& pf_;
  std::vector<Order> pending_;          // 待撮合（次日开盘执行）
  std::map<std::string, Price> opens_;  // 当日开盘价
  double slip_bps_ = 5.0;
  std::mt19937_64 jitter_{7};           // 滑点抖动源
};

}  // namespace quantd
