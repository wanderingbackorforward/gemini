// quantd — 核心模块单元测试（零依赖轻量框架）。
// 运行: ctest --test-dir build（或直接执行 ./build/quantd_tests）
#include <cmath>
#include <cstdio>
#include <string>

#include "core/event_bus.hpp"
#include "gateway/paper_gateway.hpp"
#include "market/calendar.hpp"
#include "market/market_rules.hpp"
#include "portfolio/portfolio.hpp"
#include "risk/risk_manager.hpp"

using namespace quantd;

namespace {
int g_failed = 0, g_total = 0;

void check(bool ok, const std::string& what) {
  ++g_total;
  if (!ok) {
    ++g_failed;
    std::printf("  [FAIL] %s\n", what.c_str());
  } else {
    std::printf("  [ ok ] %s\n", what.c_str());
  }
}
}  // namespace

// ---------- 市场规则 ----------
static void test_market_rules() {
  std::printf("[market_rules]\n");
  check(MarketRules::market_of("600519.SH") == Market::CN, "600519.SH -> CN");
  check(MarketRules::market_of("000001.SZ") == Market::CN, "000001.SZ -> CN");
  check(MarketRules::market_of("00700.HK") == Market::HK, "00700.HK -> HK");
  check(MarketRules::market_of("AAPL.US") == Market::US, "AAPL.US -> US");
  check(!MarketRules::market_of("AAPL"), "无后缀 -> 未知");

  const auto& cn = MarketRules::of(Market::CN);
  check(cn.t_plus_one, "A股 T+1");
  check(cn.price_limit_pct == 0.10, "A股 ±10% 涨跌停");
  check(cn.default_lot == 100, "A股 100股/手");
  check(cn.ccy == Currency::CNY, "A股 CNY");

  const auto& hk = MarketRules::of(Market::HK);
  check(!hk.t_plus_one, "港股 T+0");
  check(MarketRules::lot_of("00700.HK") == 100, "腾讯 100股/手");
  check(std::fabs(hk.tick_size(50.0) - 0.05) < 1e-12, "港股 50元 tick=0.05");
  check(hk.ccy == Currency::HKD, "港股 HKD");

  const auto& us = MarketRules::of(Market::US);
  check(us.default_lot == 1 && us.ccy == Currency::USD, "美股 1股/手 USD");

  // A股费用: 买入 10万 → 佣金 max(25, 5)=25 + 过户 1 = 26
  double fee_buy = cn.trade_fee(Side::Buy, 100000.0);
  check(std::fabs(fee_buy - 26.0) < 1e-6, "A股买入费用 10万 -> 26元");
  // 卖出另加印花税 50
  double fee_sell = cn.trade_fee(Side::Sell, 100000.0);
  check(std::fabs(fee_sell - 76.0) < 1e-6, "A股卖出费用 10万 -> 76元");

  // 最低佣金: 买入 1000元 -> 佣金 min 5 + 过户 0.01 ≈ 5.01
  double fee_min = cn.trade_fee(Side::Buy, 1000.0);
  check(fee_min >= 5.0 && fee_min < 5.1, "A股最低佣金 5元");
}

// ---------- 交易日历 ----------
static void test_calendar() {
  std::printf("[calendar]\n");
  using namespace std::chrono;
  // 2026-10-20 是周二，应为交易日
  auto tue = year{2026} / 10 / 20;
  check(is_trading_day(Market::CN, tue), "2026-10-20 A股交易日");
  check(is_trading_day(Market::HK, tue), "2026-10-20 港股交易日");
  // 2026-10-24 周六
  auto sat = year{2026} / 10 / 24;
  check(!is_trading_day(Market::CN, sat), "周六休市");
  // 国庆 2026-10-01
  auto nd = year{2026} / 10 / 1;
  check(!is_trading_day(Market::CN, nd), "2026-10-01 A股休市");
  check(!is_trading_day(Market::HK, nd), "2026-10-01 港股休市");
  // 下一交易日：10-20 -> 10-21
  auto nxt = next_trading_day(Market::CN, tue);
  check(to_string(nxt) == "2026-10-21", "next_trading_day 10-20 -> 10-21");
}

// ---------- 组合/盈亏 ----------
static void test_portfolio() {
  std::printf("[portfolio]\n");
  Portfolio pf;
  pf.set_fx(0.92, 7.1);
  pf.init_capital(Currency::CNY, 100000.0);

  Trade buy1{1, 1, "600519.SH", Market::CN, Side::Buy, 100, 100.0, 26.0,
             Currency::CNY, std::chrono::year{2026} / 10 / 20};
  check(pf.on_trade(buy1), "买入成交");
  check(std::fabs(pf.cash(Currency::CNY) - (100000.0 - 10000.0 - 26.0)) < 1e-9,
        "买入后现金 = 100000 - 10000 - 26");
  const Position* p = pf.position("600519.SH");
  check(p && p->total_qty == 100, "持仓 100 股");
  check(p && p->available_qty == 0, "A股 T+1: 当日买入不可卖");

  // 当日卖出应被拒
  Trade sell_same_day{2, 2, "600519.SH", Market::CN, Side::Sell, 100, 101.0, 0,
                      Currency::CNY, std::chrono::year{2026} / 10 / 20};
  check(!pf.on_trade(sell_same_day), "T+1 当日卖出被拒");

  pf.settle();  // 日终解冻
  check(pf.position("600519.SH")->available_qty == 100, "解冻后可卖 100");

  Trade sell1{3, 3, "600519.SH", Market::CN, Side::Sell, 100, 110.0, 76.0,
              Currency::CNY, std::chrono::year{2026} / 10 / 21};
  check(pf.on_trade(sell1), "次日卖出成交");
  // 已实现 = (110-100)*100 - 76 = 924
  check(std::fabs(pf.total_realized_pnl_cny() - 924.0) < 1e-6,
        "FIFO 已实现盈亏 = 924");
  check(!pf.position("600519.SH"), "清仓后持仓移除");

  // 港币现金记账
  Trade buy_hk{4, 4, "00700.HK", Market::HK, Side::Buy, 100, 400.0, 250.0,
               Currency::HKD, std::chrono::year{2026} / 10 / 21};
  // 港股账户现金为 0，应因现金不足被拒
  check(!pf.on_trade(buy_hk), "港币现金不足被拒（分币种记账）");
}

// ---------- 风控 ----------
static void test_risk() {
  std::printf("[risk]\n");
  Portfolio pf;
  pf.set_fx(0.92, 7.1);
  pf.init_capital(Currency::CNY, 1'000'000.0);
  RiskManager rm;
  rm.set_fx(0.92, 7.1);
  RiskParams rp{300000, 500000, 0.8, 0.15, 20};
  rm.init(rp);

  Order o1{};
  o1.symbol = "600519.SH";
  o1.market = Market::CN;
  o1.side = Side::Buy;
  o1.type = OrderType::Market;
  o1.qty = 150;   // 非整手
  check(rm.check(o1, 100.0, pf, 0).has_value(), "A股非整手买入被拒");

  o1.qty = 100;
  check(!rm.check(o1, 100.0, pf, 0), "整手买入通过");

  Order o2{};
  o2.symbol = "600519.SH";
  o2.market = Market::CN;
  o2.side = Side::Buy;
  o2.type = OrderType::Limit;
  o2.qty = 100;
  o2.limit_price = 120.0;  // 偏离 20% > 15%
  check(rm.check(o2, 100.0, pf, 0).has_value(), "限价偏离超阈值被拒");

  o2.limit_price = 111.0;  // 超涨跌停 (100*1.1=110)
  check(rm.check(o2, 100.0, pf, 0).has_value(), "超涨跌停被拒");

  Order o3{};
  o3.symbol = "600519.SH";
  o3.market = Market::CN;
  o3.side = Side::Sell;
  o3.type = OrderType::Market;
  o3.qty = 100;
  check(rm.check(o3, 100.0, pf, 0).has_value(), "无持仓卖出被拒");

  Order o4{};
  o4.symbol = "00700.HK";
  o4.market = Market::HK;
  o4.side = Side::Buy;
  o4.type = OrderType::Market;
  o4.qty = 150;  // 港股非整手
  check(rm.check(o4, 400.0, pf, 0).has_value(), "港股非整手被拒");
}

// ---------- 纸面网关 ----------
static void test_paper_gateway() {
  std::printf("[paper_gateway]\n");
  EventBus bus;
  Portfolio pf;
  pf.set_fx(0.92, 7.1);
  pf.init_capital(Currency::CNY, 1'000'000.0);
  PaperGateway gw(bus, pf);
  gw.set_slippage_bps(0);
  check(gw.connect(), "纸面网关连接");

  Trade received{};
  bus.trades().subscribe([&](const TradeEvent& e) { received = e.trade; });
  Order last_order_update{};
  bus.orders().subscribe([&](const OrderEvent& e) { last_order_update = e.order; });

  // D1 收盘信号 -> 市价单
  Order o{};
  o.id = 1;
  o.symbol = "600519.SH";
  o.market = Market::CN;
  o.side = Side::Buy;
  o.type = OrderType::Market;
  o.qty = 100;
  gw.place(o, 100.0);
  check(o.status == OrderStatus::Accepted, "订单受理进入挂单");

  // D2 开盘 bar 触发撮合（开盘 101）
  Bar d2{};
  d2.symbol = "600519.SH";
  d2.market = Market::CN;
  d2.day = std::chrono::year{2026} / 10 / 21;
  d2.open = 101.0; d2.high = 103.0; d2.low = 99.5; d2.close = 102.0;
  gw.on_session_open(d2.day);
  gw.on_bar(d2);

  check(received.id == 1, "次日开盘成交回报");
  check(std::fabs(received.price - 101.0) < 1e-9, "市价单按开盘价 101 成交");
  check(received.fee > 0, "成交含费用");
  check(pf.position("600519.SH") != nullptr, "持仓已入账");

  // 限价单挂单未触及 → 日终撤单
  Order l{};
  l.id = 2;
  l.symbol = "600519.SH";
  l.market = Market::CN;
  l.side = Side::Buy;
  l.type = OrderType::Limit;
  l.qty = 100;
  l.limit_price = 90.0;  // 远低于日内 low
  gw.place(l, 102.0);
  check(gw.pending_qty("600519.SH", Side::Buy) == 100,
        "挂单期间在途占用=100");
  check(gw.pending_qty("600519.SH", Side::Sell) == 0,
        "反方向在途=0");
  Bar d3{};
  d3.symbol = "600519.SH";
  d3.market = Market::CN;
  d3.day = std::chrono::year{2026} / 10 / 22;
  d3.open = 102.0; d3.high = 105.0; d3.low = 100.0; d3.close = 104.0;
  gw.on_session_open(d3.day);
  gw.on_bar(d3);
  check(gw.pending_qty("600519.SH", Side::Buy) == 100,
        "未成交期间占用保持");
  gw.on_session_close(d3.day);
  check(gw.pending_qty("600519.SH", Side::Buy) == 0,
        "日终撤单后占用归零");
  check(last_order_update.id == 2 &&
            last_order_update.status == OrderStatus::Cancelled,
        "未触及限价单日终自动撤单(GFD)");
}

// 在途占用风控：回报延迟窗口内不可重复卖出
static void test_pending_occupancy_risk() {
  std::printf("[pending_occupancy]\n");
  EventBus bus;
  Portfolio pf;
  pf.set_fx(0.92, 7.1);
  pf.init_capital(Currency::CNY, 100'000.0);
  PaperGateway gw(bus, pf);
  RiskManager rm;
  RiskParams rp;
  rp.max_order_value_cny = 300'000;
  rp.max_position_value_cny = 500'000;
  rp.max_total_exposure = 0.8;
  rp.max_price_deviation = 0.15;
  rp.max_orders_per_day = 20;
  rm.init(rp);
  rm.set_fx(0.92, 7.1);

  // 注入持仓 600519.SH x100，可卖 100
  Position p{};
  p.symbol = "600519.SH";
  p.market = Market::CN;
  p.ccy = Currency::CNY;
  p.total_qty = 100;
  p.available_qty = 100;
  p.avg_price = 100.0;
  pf.bootstrap_position(p);

  // 第一笔卖出委托挂单中（占用 100 股）
  Order o1{};
  o1.id = 1;
  o1.symbol = "600519.SH";
  o1.market = Market::CN;
  o1.side = Side::Sell;
  o1.type = OrderType::Market;
  o1.qty = 100;
  gw.place(o1, 100.0);
  check(gw.pending_qty("600519.SH", Side::Sell) == 100, "卖出在途=100");

  // 回报未到、持仓视角未变时，第二笔同标的卖出应被风控拦截
  Order o2 = o1;
  o2.id = 2;
  o2.qty = 100;
  auto reason = rm.check(o2, 100.0, pf, 1, gw.pending_qty("600519.SH", Side::Sell));
  check(reason.has_value(), "在途占用期间重复卖出被风控拦截");
}

int main() {
  std::printf("===== quantd 单元测试 =====\n");
  test_market_rules();
  test_calendar();
  test_portfolio();
  test_risk();
  test_paper_gateway();
  test_pending_occupancy_risk();
  std::printf("===== 结果: %d/%d 通过 =====\n", g_total - g_failed, g_total);
  return g_failed == 0 ? 0 : 1;
}
