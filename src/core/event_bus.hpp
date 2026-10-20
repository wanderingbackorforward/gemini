// quantd — 单线程同步事件总线。
//
// 低频交易（日线级）不需要无锁队列与多线程派发；单线程同步派发
// 天然免锁、事件顺序确定、便于回放与调试。若未来升级高频，
// 将 Channel 内部替换为 SPSC 环形队列即可，接口不变。
#pragma once

#include <functional>
#include <utility>
#include <vector>

#include "core/events.hpp"

namespace quantd {

template <typename E>
class Channel {
 public:
  void subscribe(std::function<void(const E&)> fn) {
    subs_.push_back(std::move(fn));
  }
  void publish(const E& e) {
    for (auto& f : subs_) f(e);
  }

 private:
  std::vector<std::function<void(const E&)>> subs_;
};

class EventBus {
 public:
  Channel<BarEvent>&          bars()   { return bars_; }
  Channel<OrderEvent>&        orders() { return orders_; }
  Channel<TradeEvent>&        trades() { return trades_; }
  Channel<SessionOpenEvent>&  opens()  { return opens_; }
  Channel<SessionCloseEvent>& closes() { return closes_; }

 private:
  Channel<BarEvent> bars_;
  Channel<OrderEvent> orders_;
  Channel<TradeEvent> trades_;
  Channel<SessionOpenEvent> opens_;
  Channel<SessionCloseEvent> closes_;
};

}  // namespace quantd
