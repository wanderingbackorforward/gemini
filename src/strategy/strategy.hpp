// quantd — 策略接口。
// 策略只允许通过 OrderAgent 下单、只接收事件回调，不允许触碰
// 引擎/网关/组合内部，保证关注点分离与可回测性。
#pragma once

#include "core/types.hpp"

namespace quantd {

class OrderAgent;  // 前置声明（见 engine/order_agent.hpp）

class Strategy {
 public:
  virtual ~Strategy() = default;
  virtual std::string name() const = 0;

  virtual void on_start(OrderAgent&) {}
  // 收到一根日线。注意：为避免前视偏差，本框架约定
  // bar 日产生的订单在【次日开盘】撮合（见 PaperGateway）。
  virtual void on_bar(OrderAgent&, const Bar&) {}
  virtual void on_trade(OrderAgent&, const Trade&) {}
  virtual void on_session_close(OrderAgent&, TradingDay) {}
};

}  // namespace quantd
