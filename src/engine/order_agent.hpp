// quantd — 下单代理。
// 策略面向 OrderAgent 编程（而非直接面对引擎），便于：
//   1. 单元测试中用 MockAgent 替换
//   2. 未来多引擎/远程引擎部署时只换此实现
#pragma once

#include <string>

#include "core/types.hpp"
#include "portfolio/portfolio.hpp"

namespace quantd {

class Engine;

class OrderAgent {
 public:
  explicit OrderAgent(Engine& e) : engine_(e) {}

  // 下市价单（次日开盘撮合）。返回订单号；被风控拒绝返回订单号但状态为 Rejected。
  uint64_t market_order(const std::string& symbol, Side side, Qty qty);
  // 下限价单
  uint64_t limit_order(const std::string& symbol, Side side, Qty qty, Price px);

  // ---- 策略常用查询 ----
  const Position* position(const std::string& symbol) const;
  double equity_cny() const;                       // 总权益（折CNY）
  Price last_close(const std::string& symbol) const;
  int lot_of(const std::string& symbol) const;     // 每手股数
  double cash_for(const std::string& symbol) const;  // 该标的币种可用现金
  double cny_to(const std::string& symbol, double cny_amount) const;  // CNY→标的币种

 private:
  Engine& engine_;
};

}  // namespace quantd
