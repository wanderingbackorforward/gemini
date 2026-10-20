// quantd — 示例策略：双均线交叉（低频）。
// 快线上穿慢线买入、下穿卖出。信号在 bar 收盘确认，
// 由网关于次日开盘撮合，无前视偏差。
// 这是教学级基线策略，目的在于打通全链路，不构成投资建议。
#pragma once

#include <deque>
#include <map>
#include <string>

#include "strategy/strategy.hpp"

namespace quantd {

class DualMAStrategy final : public Strategy {
 public:
  void init(int fast, int slow, double position_ratio) {
    fast_ = fast;
    slow_ = slow;
    ratio_ = position_ratio;
  }

  std::string name() const override { return "DualMA"; }

  void on_bar(OrderAgent& agent, const Bar& bar) override;
  void on_trade(OrderAgent& agent, const Trade& t) override;

 private:
  static double ma(const std::deque<Price>& closes, int n);

  int fast_ = 5;
  int slow_ = 20;
  double ratio_ = 0.2;                        // 每标的目标仓位占总权益比
  std::map<std::string, std::deque<Price>> closes_;
  std::map<std::string, bool> holding_;
};

}  // namespace quantd
