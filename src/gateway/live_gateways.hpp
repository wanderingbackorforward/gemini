// quantd — 实盘网关：TCP 通道 + 行协议。
//
// 架构：每个市场一个网关实例，通过 TCP(默认 127.0.0.1) 连接桥进程
// （如 scripts/live/qmt_bridge.py）。桥负责与券商 SDK 通信，本类只做
// 协议收发与状态映射。桥独立进程的好处：崩溃隔离 + 可单独手动调试
// （telnet 127.0.0.1 <port> 即可肉眼排查）。
//
// 线程模型：后台读线程只负责收行入队；引擎主循环调用 poll() 消费
// 队列并发布事件 —— EventBus 保持单线程语义，无锁竞争。
//
// 影子模式（shadow）：连接、对账、回报处理全部真实执行，唯独
// place() 拦截订单并标记"影子拦截"。用于真钱上线前的零成本验收。
#pragma once

#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "gateway/gateway.hpp"
#include "portfolio/portfolio.hpp"

namespace quantd {

// 跨平台 socket 句柄
#ifdef _WIN32
using sock_t = void*;
#else
using sock_t = int;
#endif

class LiveGatewayBase : public IGateway {
 public:
  // shadow=true 时拦截所有真实委托（验收模式）
  LiveGatewayBase(EventBus& bus, Portfolio& pf, std::string host, int port,
                  bool shadow);
  ~LiveGatewayBase() override;

  bool connect() override;
  void place(Order& o, Price ref_price) override;
  void cancel(uint64_t order_id) override;
  Qty pending_qty(const std::string& symbol, Side side) const override;

  // live 模式下时点由引擎调度，bar 来自桥/行情源推送
  void on_session_open(TradingDay) override {}
  void on_bar(const Bar&) override {}
  void on_session_close(TradingDay) override {}

  // 引擎主循环驱动：消费桥消息（ack/trade/对账快照）
  void poll();

  bool is_shadow() const { return shadow_; }
  bool connected() const { return sock_ok_; }

 protected:
  // 子类钩子：处理已收到的一行协议（默认实现覆盖 ack/trade/pos/cash）
  virtual void handle_message(const std::map<std::string, std::string>& msg);
  // 子类钩子：连接建立后发送的初始请求（默认握手+对账）
  virtual void on_connected();

  bool send_line(const std::string& line);
  void close_socket();

  EventBus& bus_;
  Portfolio& pf_;
  const std::string host_;
  const int port_;
  const bool shadow_;

  sock_t sock_{};
  bool sock_ok_ = false;
  std::thread reader_;
  std::mutex mq_;
  std::deque<std::string> queue_;

  // 引擎订单号 → 订单快照（ack/trade 回报时补发 OrderEvent）
  std::map<uint64_t, Order> live_orders_;
};

// ---- A股实盘（QMT / 迅投 xtquant，桥: scripts/live/qmt_bridge.py）----
class QmtGateway final : public LiveGatewayBase {
 public:
  using LiveGatewayBase::LiveGatewayBase;
  std::string name() const override { return shadow_ ? "live_qmt(A股·影子)" : "live_qmt(A股·实盘)"; }
};

// ---- 港美股实盘（富途 OpenD，桥: scripts/live/futu_bridge.py）----
class FutuGateway final : public LiveGatewayBase {
 public:
  using LiveGatewayBase::LiveGatewayBase;
  std::string name() const override { return shadow_ ? "live_futu(港美·影子)" : "live_futu(港美·实盘)"; }
};

}  // namespace quantd
