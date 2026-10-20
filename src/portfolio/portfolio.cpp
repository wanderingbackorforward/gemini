#include "portfolio/portfolio.hpp"

#include <algorithm>
#include <chrono>
#include <numeric>

#include "market/market_rules.hpp"

namespace quantd {

namespace {
inline int ccy_idx(Currency c) { return static_cast<int>(c); }
}

const Position* Portfolio::position(const std::string& symbol) const {
  auto it = positions_.find(symbol);
  return it == positions_.end() ? nullptr : &it->second;
}

double Portfolio::cash(Currency c) const {
  auto it = cash_.find(c);
  return it == cash_.end() ? 0.0 : it->second;
}

Price Portfolio::last_mark(const std::string& symbol) const {
  auto it = marks_.find(symbol);
  return it == marks_.end() ? 0.0 : it->second;
}

bool Portfolio::on_trade(const Trade& t, std::string* why) {
  const auto& rules = MarketRules::of(t.market);
  Position* pos = nullptr;
  auto it = positions_.find(t.symbol);
  if (it != positions_.end()) pos = &it->second;

  if (t.side == Side::Buy) {
    // ---- 买入：扣现金、加权摊入成本 ----
    double notional = t.price * static_cast<double>(t.qty);
    if (cash(t.ccy) < notional + t.fee) {
      if (why) *why = "现金不足";
      return false;
    }
    cash_[t.ccy] -= notional + t.fee;

    if (!pos) {
      Position p;
      p.symbol = t.symbol;
      p.market = t.market;
      p.ccy = t.ccy;
      p.total_qty = t.qty;
      p.available_qty = 0;                 // T+1 市场当日不可卖
      p.avg_price = t.price;
      positions_[t.symbol] = p;
      pos = &positions_[t.symbol];
    } else {
      double new_total = static_cast<double>(pos->total_qty + t.qty);
      pos->avg_price = (pos->avg_price * static_cast<double>(pos->total_qty) +
                        notional) / new_total;
      pos->total_qty += t.qty;
    }
    if (!rules.t_plus_one) pos->available_qty += t.qty;  // T+0 市场即时可卖
    lots_[t.symbol].push_back({t.qty, t.price, t.day});
    return true;
  }

  // ---- 卖出：检查可卖数量，FIFO 匹配实现盈亏 ----
  if (!pos || pos->available_qty < t.qty) {
    if (why) *why = pos ? "可卖数量不足（T+1 或已卖空）" : "无持仓可卖";
    return false;
  }
  double notional = t.price * static_cast<double>(t.qty);
  cash_[t.ccy] += notional - t.fee;
  pos->available_qty -= t.qty;
  pos->total_qty -= t.qty;

  auto& queue = lots_[t.symbol];
  Qty remain = t.qty;
  double realized = 0.0;
  while (remain > 0 && !queue.empty()) {
    Qty take = std::min(remain, queue.front().qty);
    realized += static_cast<double>(take) * (t.price - queue.front().price);
    RoundTrip rt;
    rt.symbol = t.symbol;
    rt.open_side = Side::Buy;
    rt.qty = take;
    rt.open_price = queue.front().price;
    rt.close_price = t.price;
    rt.realized_pnl = realized;
    rt.open_day = queue.front().day;
    rt.close_day = t.day;
    trips_.push_back(rt);
    queue.front().qty -= take;
    remain -= take;
    if (queue.front().qty == 0) queue.erase(queue.begin());
  }
  // 卖出费用冲减已实现盈亏（净额口径）
  double net = realized - t.fee;
  realized_by_ccy_[ccy_idx(t.ccy)] += net;

  if (pos->total_qty == 0) {
    positions_.erase(t.symbol);
    lots_.erase(t.symbol);
  }
  return true;
}

void Portfolio::settle() {
  for (auto& [sym, pos] : positions_) {
    if (MarketRules::of(pos.market).t_plus_one) {
      pos.available_qty = pos.total_qty;   // T+1：隔日全部解冻
    }
  }
}

double Portfolio::positions_value_cny() const {
  double v = 0.0;
  for (const auto& [sym, pos] : positions_) {
    Price px = marks_.count(sym) ? marks_.at(sym) : pos.avg_price;
    v += to_cny(px * static_cast<double>(pos.total_qty), pos.ccy, hkd_cny_, usd_cny_);
  }
  return v;
}

double Portfolio::total_equity_cny() const {
  double v = 0.0;
  for (const auto& [ccy, amt] : cash_) {
    v += to_cny(amt, ccy, hkd_cny_, usd_cny_);
  }
  return v + positions_value_cny();
}

double Portfolio::convert(double amount, Currency from, Currency to) const {
  if (from == to) return amount;
  double cny = from == Currency::HKD ? amount * hkd_cny_
               : from == Currency::USD ? amount * usd_cny_
                                       : amount;
  return to == Currency::CNY ? cny
         : to == Currency::HKD ? cny / hkd_cny_
                               : cny / usd_cny_;
}

void Portfolio::bootstrap_cash(Currency c, double amount) {
  cash_[c] = amount;
}

void Portfolio::bootstrap_position(const Position& p) {
  if (p.total_qty <= 0) {
    positions_.erase(p.symbol);
    lots_.erase(p.symbol);
    return;
  }
  positions_[p.symbol] = p;
  // 对账持仓无 FIFO 明细：以单一"均价批次"重建（真实成本以券商为准）
  auto today = std::chrono::year_month_day{
      std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
  lots_[p.symbol] = {{p.total_qty, p.avg_price, today}};
}

double Portfolio::unrealized_pnl_cny() const {
  double v = 0.0;
  for (const auto& [sym, pos] : positions_) {
    Price px = marks_.count(sym) ? marks_.at(sym) : pos.avg_price;
    v += to_cny((px - pos.avg_price) * static_cast<double>(pos.total_qty),
                pos.ccy, hkd_cny_, usd_cny_);
  }
  return v;
}

double Portfolio::total_realized_pnl_cny() const {
  return to_cny(realized_by_ccy_[ccy_idx(Currency::CNY)], Currency::CNY, hkd_cny_, usd_cny_) +
         to_cny(realized_by_ccy_[ccy_idx(Currency::HKD)], Currency::HKD, hkd_cny_, usd_cny_) +
         to_cny(realized_by_ccy_[ccy_idx(Currency::USD)], Currency::USD, hkd_cny_, usd_cny_);
}

}  // namespace quantd
