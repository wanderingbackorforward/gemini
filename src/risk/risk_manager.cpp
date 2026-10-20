#include "risk/risk_manager.hpp"

#include <cmath>
#include <format>

#include "market/market_rules.hpp"

namespace quantd {

double RiskManager::to_cny2(double amount, Currency c) const {
  switch (c) {
    case Currency::CNY: return amount;
    case Currency::HKD: return amount * hkd_cny_;
    case Currency::USD: return amount * usd_cny_;
  }
  return amount;
}

std::optional<std::string> RiskManager::check(const Order& o, Price last_close,
                                              const Portfolio& pf,
                                              int orders_today) const {
  if (o.qty <= 0) return "委托数量必须为正";
  if (o.type == OrderType::Limit && o.limit_price <= 0) return "限价单价格非法";
  if (orders_today >= p_.max_orders_per_day) {
    return std::format("超过单日最大委托笔数 {}", p_.max_orders_per_day);
  }

  const auto& rules = MarketRules::of(o.market);
  const int lot = MarketRules::lot_of(o.symbol);

  // 整手检查：A股买入须整手（卖出允许零股，处理历史拆股尾差）
  if (rules.market == Market::CN && o.side == Side::Buy && o.qty % lot != 0) {
    return std::format("A股买入须为{}股整数倍", lot);
  }
  // 港股买卖均须整手
  if (rules.market == Market::HK && o.qty % lot != 0) {
    return std::format("港股须为{}股整手", lot);
  }

  // 卖出可卖数量检查（T+1 / 空头防线）
  if (o.side == Side::Sell) {
    const Position* pos = pf.position(o.symbol);
    if (!pos || pos->available_qty < o.qty) {
      return "可卖数量不足（T+1 当日买入或无持仓）";
    }
  }

  // 委托金额上限
  double px = o.type == OrderType::Limit ? o.limit_price : last_close;
  if (px <= 0) return "无参考价格，拒绝市价单（首个交易日只观察不下单）";
  double order_value_cny = to_cny2(px * static_cast<double>(o.qty), rules.ccy);
  if (order_value_cny > p_.max_order_value_cny) {
    return std::format("单笔金额 {:.0f} CNY 超上限 {:.0f}",
                       order_value_cny, p_.max_order_value_cny);
  }

  // 单标的持仓市值上限（按委托价预估）
  if (const Position* pos = pf.position(o.symbol)) {
    Price mark = pf.last_mark(o.symbol);
    if (mark <= 0) mark = pos->avg_price;
    double cur = to_cny2(static_cast<double>(pos->total_qty) * mark, rules.ccy);
    double delta = o.side == Side::Buy ? order_value_cny : -order_value_cny;
    if (cur + delta > p_.max_position_value_cny) {
      return std::format("标的 {} 持仓市值超上限", o.symbol);
    }
  }

  // 总仓位上限（仅约束买入）
  if (o.side == Side::Buy) {
    double equity = pf.total_equity_cny();
    double exposure = pf.positions_value_cny() + order_value_cny;
    if (equity > 0 && exposure / equity > p_.max_total_exposure) {
      return std::format("总仓位 {:.1f}% 超上限 {:.1f}%",
                         100.0 * exposure / equity, 100.0 * p_.max_total_exposure);
    }
  }

  // 限价单价格偏离保护
  if (o.type == OrderType::Limit && last_close > 0) {
    double dev = std::fabs(o.limit_price / last_close - 1.0);
    if (dev > p_.max_price_deviation) {
      return std::format("限价偏离昨收 {:.1f}% 超阈值", 100.0 * dev);
    }
  }

  // A股涨跌停板价格检查
  if (rules.price_limit_pct > 0 && last_close > 0) {
    Price upper = last_close * (1 + rules.price_limit_pct);
    Price lower = last_close * (1 - rules.price_limit_pct);
    if (o.type == OrderType::Limit &&
        (o.limit_price > upper + 1e-9 || o.limit_price < lower - 1e-9)) {
      return "委托价超出涨跌停范围";
    }
  }
  return std::nullopt;
}

}  // namespace quantd
