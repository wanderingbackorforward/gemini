#include "gateway/paper_gateway.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>

#include "core/log.hpp"
#include "market/market_rules.hpp"

namespace quantd {

namespace {
// 成交号全局唯一：多个网关实例（多市场）共存时也不冲突。
// 实盘接入后成交号以券商回报为准，此计数器仅纸面阶段使用。
std::atomic<uint64_t> g_trade_id{1};
}  // namespace

void PaperGateway::place(Order& o, Price /*ref_price*/) {
  // 引擎已完成风控；网关受理订单，等待次日开盘撮合。
  o.status = OrderStatus::Accepted;
  pending_.push_back(o);
  bus_.orders().publish(OrderEvent{o});
}

void PaperGateway::cancel(uint64_t order_id) {
  for (auto& o : pending_) {
    if (o.id == order_id && o.status == OrderStatus::Accepted) {
      o.status = OrderStatus::Cancelled;
      bus_.orders().publish(OrderEvent{o});
    }
  }
  std::erase_if(pending_, [](const Order& o) {
    return o.status != OrderStatus::Accepted;
  });
}

void PaperGateway::on_session_open(TradingDay /*day*/) {
  opens_.clear();
  // 注意：开盘价由随后的 on_bar(bar) 提供（bar.open），
  // 因此市价单实际在"当日第一根 bar 到达时"撮合，见 on_bar。
}

void PaperGateway::on_bar(const Bar& bar) {
  const std::string& sym = bar.symbol;

  // ---- 1) 市价单：开盘价 ± 滑点成交（无前视：bar 日信号，次日开盘成交）----
  std::vector<Order> still_pending;
  for (auto& o : pending_) {
    if (o.symbol != sym) { still_pending.push_back(o); continue; }
    if (o.status != OrderStatus::Accepted) { continue; }

    const auto& rules = MarketRules::of(o.market);
    Price px = bar.open;
    if (o.type == OrderType::Market) {
      double slip = slip_bps_ / 10000.0 *
                    (1.0 + 0.2 * std::uniform_real_distribution<double>(-1.0, 1.0)(jitter_));
      px = o.side == Side::Buy ? bar.open * (1 + slip) : bar.open * (1 - slip);
      px = std::round(px / rules.tick_size(px)) * rules.tick_size(px);
      fill(o, px, bar.day);
      still_pending.push_back(o);
    } else {
      // ---- 2) 限价单：开盘优于限价按开盘成交，否则等日内 low/high ----
      if (o.side == Side::Buy && bar.open <= o.limit_price) {
        fill(o, bar.open, bar.day);
        still_pending.push_back(o);
      } else if (o.side == Side::Sell && bar.open >= o.limit_price) {
        fill(o, bar.open, bar.day);
        still_pending.push_back(o);
      } else if (o.side == Side::Buy && bar.low <= o.limit_price) {
        fill(o, o.limit_price, bar.day);
        still_pending.push_back(o);
      } else if (o.side == Side::Sell && bar.high >= o.limit_price) {
        fill(o, o.limit_price, bar.day);
        still_pending.push_back(o);
      } else {
        still_pending.push_back(o);   // 继续挂单
      }
    }
  }
  pending_.swap(still_pending);
}

void PaperGateway::on_session_close(TradingDay /*day*/) {
  // GFD（当日有效）：日终撤销未成交的限价单。
  // 市价单不撤——其语义是"次日开盘执行"（信号日收盘决策 →
  // 次日开盘撮合，见 on_bar）。若标的停牌多日无 bar，订单会
  // 继续挂至下一可成交开盘。
  for (auto& o : pending_) {
    if (o.status == OrderStatus::Accepted && o.type == OrderType::Limit) {
      o.status = OrderStatus::Cancelled;
      o.note = "日终自动撤单(GFD)";
      bus_.orders().publish(OrderEvent{o});
    }
  }
  std::erase_if(pending_, [](const Order& o) {
    return o.status != OrderStatus::Accepted;
  });
}

void PaperGateway::fill(Order& o, Price px, TradingDay day) {
  const auto& rules = MarketRules::of(o.market);
  double notional = px * static_cast<double>(o.qty);
  Price fee = rules.trade_fee(o.side, notional);

  Trade t;
  t.id = g_trade_id.fetch_add(1);
  t.order_id = o.id;
  t.symbol = o.symbol;
  t.market = o.market;
  t.side = o.side;
  t.qty = o.qty;
  t.price = px;
  t.fee = fee;
  t.ccy = rules.ccy;
  t.day = day;

  // 最后防线：可卖数量 / 现金校验（正常流程风控已拦截）
  std::string why;
  bool ok = (o.side == Side::Sell)
                ? (!pf_.position(o.symbol) ||
                   pf_.position(o.symbol)->available_qty >= o.qty)
                : true;
  if (ok) ok = pf_.on_trade(t, &why);
  if (!ok) {
    o.status = OrderStatus::Rejected;
    o.note = "网关拒单: " + why;
    bus_.orders().publish(OrderEvent{o});
    return;
  }

  o.status = OrderStatus::Filled;
  bus_.trades().publish(TradeEvent{t});
  bus_.orders().publish(OrderEvent{o});
}

}  // namespace quantd
