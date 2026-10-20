#include "strategy/dual_ma.hpp"

#include <algorithm>
#include <cmath>
#include <format>

#include "core/log.hpp"
#include "engine/order_agent.hpp"

namespace quantd {

double DualMAStrategy::ma(const std::deque<Price>& closes, int n) {
  int cnt = std::min(n, static_cast<int>(closes.size()));
  double sum = 0.0;
  for (int i = 0; i < cnt; ++i) sum += closes[closes.size() - 1 - i];
  return sum / cnt;
}

void DualMAStrategy::on_bar(OrderAgent& agent, const Bar& bar) {
  auto& series = closes_[bar.symbol];
  series.push_back(bar.close);
  if (static_cast<int>(series.size()) > slow_ + 1) series.pop_front();  // 滑窗
  if (static_cast<int>(series.size()) < slow_ + 1) return;  // 历史不足，先观察

  double fast = ma(series, fast_);
  double slow = ma(series, slow_);
  // 前一根的均线（用去掉最新一根的窗口）
  auto prev_series = series;
  prev_series.pop_back();
  double prev_fast = ma(prev_series, fast_);
  double prev_slow = ma(prev_series, slow_);

  bool golden = prev_fast <= prev_slow && fast > slow;   // 金叉
  bool dead   = prev_fast >= prev_slow && fast < slow;   // 死叉
  if (!golden && !dead) return;

  const Position* pos = agent.position(bar.symbol);
  const int lot = agent.lot_of(bar.symbol);

  if (golden && !pos) {
    // 在途买单未落地时不重复买（券商回报延迟窗口）
    if (agent.pending_qty(bar.symbol, Side::Buy) > 0) return;
    // 目标金额 = min(权益×仓位比, 该币种账户可用现金×0.98)
    // （多币种账户分账管理，避免跨市场抢资金）
    double budget_ccy = std::min(
        agent.cny_to(bar.symbol, agent.equity_cny() * ratio_),
        agent.cash_for(bar.symbol) * 0.98);
    Price px = agent.last_close(bar.symbol);
    if (px <= 0 || lot <= 0 || budget_ccy <= 0) return;
    Qty qty = static_cast<Qty>(std::floor(budget_ccy / px / lot)) * lot;
    if (qty < lot) return;
    auto id = agent.market_order(bar.symbol, Side::Buy, qty);
    QD_INFO("[{}] 金叉信号: BUY {} x{} @~{:.2f} (单#{})", name(), bar.symbol, qty, px, id);
  } else if (dead && pos) {
    // 卖出全部可卖（T+1 当日买入部分次日再卖；
    // 在途卖单占用量一并扣除，防止回报延迟窗口内重复卖出）
    Qty pend = agent.pending_qty(bar.symbol, Side::Sell);
    Qty avail = pos->available_qty > pend ? pos->available_qty - pend : 0;
    if (avail > 0) {
      auto id = agent.market_order(bar.symbol, Side::Sell, avail);
      QD_INFO("[{}] 死叉信号: SELL {} x{} (单#{})", name(), bar.symbol, avail, id);
    }
  }
}

void DualMAStrategy::on_trade(OrderAgent& /*agent*/, const Trade& t) {
  holding_[t.symbol] = (t.side == Side::Buy);
}

}  // namespace quantd
