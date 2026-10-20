// quantd — 事前风控。
// 所有订单在进入网关前必须通过此处检查。风控规则应保持保守：
// 宁可错杀，不可放过。生产版本还应加入事中(实时持仓监控)与
// 事后(当日复盘报告)风控，本模块预留扩展点。
#pragma once

#include <optional>
#include <string>

#include "core/types.hpp"
#include "portfolio/portfolio.hpp"

namespace quantd {

struct RiskParams {
  double max_order_value_cny{};     // 单笔委托金额上限（折CNY）
  double max_position_value_cny{};  // 单标的持仓市值上限（折CNY）
  double max_total_exposure{};      // 总持仓市值/总权益 上限（如 0.8）
  double max_price_deviation{};     // 限价单相对昨收最大偏离（如 0.15）
  int max_orders_per_day{};         // 单日最大委托笔数
};

class RiskManager {
 public:
  void init(const RiskParams& p) { p_ = p; }
  void set_fx(double hkd_cny, double usd_cny) {
    hkd_cny_ = hkd_cny;
    usd_cny_ = usd_cny;
  }

  // 校验订单；通过返回 nullopt，否则返回拒绝原因。
  std::optional<std::string> check(const Order& o, Price last_close,
                                   const Portfolio& pf, int orders_today) const;

 private:
  double to_cny2(double amount, Currency c) const;

  RiskParams p_{};
  double hkd_cny_ = 0.92;
  double usd_cny_ = 7.1;
};

}  // namespace quantd
