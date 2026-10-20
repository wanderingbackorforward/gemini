// quantd — 交易网关抽象。
// 每个市场绑定一个网关（Paper 模拟 / QMT / Futu / Longport 实盘）。
// 网关只负责"订单 ↔ 成交回报"的通道与撮合，不持有任何策略逻辑。
#pragma once

#include "core/event_bus.hpp"
#include "core/types.hpp"

namespace quantd {

class IGateway {
 public:
  virtual ~IGateway() = default;
  virtual std::string name() const = 0;

  // 握手/登录。纸面网关恒成功；实盘网关在此建立连接。
  virtual bool connect() = 0;

  // 受理订单：网关内部撮合（Paper）或转发券商（Live），
  // 状态变化通过 EventBus 的 orders()/trades() 通道发布。
  virtual void place(Order& o, Price ref_price) = 0;

  // 撤单
  virtual void cancel(uint64_t order_id) = 0;

  // ---- 回调（由引擎在事件循环中驱动）----

  // 交易日开盘：撮合昨日收盘后挂出的市价/限价单
  virtual void on_session_open(TradingDay day) = 0;

  // 日内每根 bar 到来：检查限价单是否可成交
  virtual void on_bar(const Bar& bar) = 0;

  // 交易日收盘：撤销所有未成交挂单（GFD 当日有效）
  virtual void on_session_close(TradingDay day) = 0;
};

}  // namespace quantd
