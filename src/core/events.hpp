// quantd — 事件定义。事件是系统各模块间唯一的通信载体。
#pragma once

#include "core/types.hpp"

namespace quantd {

// 行情事件：一根日线完成
struct BarEvent { Bar bar; };

// 订单状态事件：受理/拒绝/撤单/成交（状态流转通知）
struct OrderEvent { Order order; };

// 成交事件
struct TradeEvent { Trade trade; };

// 交易日开盘：网关在此阶段撮合昨日收盘后挂出的订单
struct SessionOpenEvent { TradingDay day; };

// 交易日收盘：结算（T+1 解冻）、净值快照
struct SessionCloseEvent { TradingDay day; };

}  // namespace quantd
