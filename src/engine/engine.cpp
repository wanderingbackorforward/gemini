#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <format>
#include <iomanip>
#include <sstream>

#include "core/log.hpp"
#include "engine/order_agent.hpp"
#include "gateway/live_gateways.hpp"
#include "gateway/paper_gateway.hpp"
#include "market/calendar.hpp"
#include "portfolio/portfolio.hpp"
#include "strategy/dual_ma.hpp"

namespace quantd {

namespace {

std::string to_iso(TradingDay d) {
  return std::format("{}-{:02d}-{:02d}", int(d.year()), unsigned(d.month()), unsigned(d.day()));
}

}  // namespace

bool Engine::init() {
  // ---- 1) 市场/网关装配：每市场一个网关 ----
  const std::string output_dir = cfg_.str("report.dir", "output");
  std::filesystem::create_directories(output_dir);

  const double hkd_cny = cfg_.num("fx.hkd_cny", 0.92);
  const double usd_cny = cfg_.num("fx.usd_cny", 7.10);
  pf_.set_fx(hkd_cny, usd_cny);
  risk_.set_fx(hkd_cny, usd_cny);

  struct MarketCfg {
    Market m;
    const char* gateway_key;
    const char* enable_key;
  } market_cfgs[] = {
      {Market::CN, "gateway.cn", "market.cn.enable"},
      {Market::HK, "gateway.hk", "market.hk.enable"},
      {Market::US, "gateway.us", "market.us.enable"},
  };

  for (const auto& mc : market_cfgs) {
    if (!cfg_.flag(mc.enable_key, true)) {
      QD_INFO("市场 {} 未启用，跳过", to_string(mc.m));
      continue;
    }
    const std::string g = cfg_.str(mc.gateway_key, "paper");
    if (g == "paper") {
      auto gw = std::make_unique<PaperGateway>(bus_, pf_);
      gw->set_slippage_bps(cfg_.num("paper.slippage_bps", 5.0));
      gateways_[mc.m] = std::move(gw);
      QD_INFO("市场 {} -> 纸面网关(模拟撮合)", to_string(mc.m));
    } else if (g == "qmt") {
      auto gw = std::make_unique<QmtGateway>(bus_, cfg_.str("gateway.qmt.bridge", "python3 scripts/live/qmt_bridge.py"));
      if (!gw->connect()) {
        QD_ERROR("QMT 网关连接失败：请先启动 QMT 客户端并装配桥进程（见 README）");
        return false;
      }
      gateways_[mc.m] = std::move(gw);
    } else if (g == "futu") {
      auto gw = std::make_unique<FutuGateway>(bus_, cfg_.str("gateway.futu.bridge", "python3 scripts/live/futu_bridge.py"));
      if (!gw->connect()) {
        QD_ERROR("Futu 网关连接失败：请先启动 OpenD 并装配桥进程（见 README）");
        return false;
      }
      gateways_[mc.m] = std::move(gw);
    } else if (g == "longport") {
      auto gw = std::make_unique<LongportGateway>(bus_, "longport");
      if (!gw->connect()) {
        QD_ERROR("LongPort 网关未装配：需引入官方 C SDK 后编译（见 README）");
        return false;
      }
      gateways_[mc.m] = std::move(gw);
    } else {
      QD_ERROR("未知网关类型: {} (支持 paper/qmt/futu/longport)", g);
      return false;
    }
  }
  if (gateways_.empty()) {
    QD_ERROR("没有任何启用的市场网关");
    return false;
  }

  // ---- 2) 资金（分币种独立记账，折CNY 合并计算权益）----
  const double capital = cfg_.num("account.capital_cny", 1'000'000.0);
  pf_.init_capital(Currency::CNY, capital);
  if (double hkd = cfg_.num("account.capital_hkd", 0.0); hkd > 0) {
    pf_.init_capital(Currency::HKD, hkd);
  }
  if (double usd = cfg_.num("account.capital_usd", 0.0); usd > 0) {
    pf_.init_capital(Currency::USD, usd);
  }
  initial_equity_cny_ = pf_.total_equity_cny();

  // ---- 3) 风控 ----
  RiskParams rp;
  rp.max_order_value_cny = cfg_.num("risk.max_order_value_cny", 300'000.0);
  rp.max_position_value_cny = cfg_.num("risk.max_position_value_cny", 500'000.0);
  rp.max_total_exposure = cfg_.num("risk.max_total_exposure", 0.8);
  rp.max_price_deviation = cfg_.num("risk.max_price_deviation", 0.15);
  rp.max_orders_per_day = static_cast<int>(cfg_.integer("risk.max_orders_per_day", 20));
  risk_.init(rp);

  // ---- 4) 行情源 ----
  std::vector<std::string> universe = cfg_.list("universe");
  // 过滤未启用市场的标的
  std::erase_if(universe, [&](const std::string& s) {
    auto m = MarketRules::market_of(s);
    if (!m) {
      QD_ERROR("标的后缀非法，已剔除: {}", s);
      return true;
    }
    return !gateways_.contains(*m);
  });
  if (universe.empty()) {
    QD_ERROR("交易标的池为空");
    return false;
  }
  feed_.init(cfg_, universe);

  // ---- 5) 策略装配（按 strategy.* 段）----
  const std::string strat = cfg_.str("strategy", "dual_ma");
  if (strat == "dual_ma") {
    auto s = std::make_unique<DualMAStrategy>();
    s->init(static_cast<int>(cfg_.integer("strategy.fast", 5)),
            static_cast<int>(cfg_.integer("strategy.slow", 20)),
            cfg_.num("strategy.position_ratio", 0.2));
    strategies_.push_back(std::move(s));
  } else {
    QD_ERROR("未知策略: {}", strat);
    return false;
  }

  // ---- 6) 事件订阅 ----
  auto* strat0 = strategies_[0].get();   // 单策略；多策略时可在此注册多份
  bus_.bars().subscribe([this, strat0](const BarEvent& e) {
    OrderAgent agent(*this);
    for (auto& s : strategies_) s->on_bar(agent, e.bar);
  });
  bus_.trades().subscribe([this](const TradeEvent& e) {
    all_trades_.push_back(e.trade);
    OrderAgent agent(*this);
    for (auto& s : strategies_) s->on_trade(agent, e.trade);
    QD_INFO("成交 #{} {} {} {} x{} @{} 费{} {}",
            e.trade.id, e.trade.symbol, to_string(e.trade.side),
            to_string(e.trade.market), e.trade.qty, e.trade.price,
            e.trade.fee, to_string(e.trade.ccy));
  });
  bus_.orders().subscribe([this](const OrderEvent& e) {
    all_orders_.push_back(e.order);
    if (e.order.status == OrderStatus::Rejected) {
      QD_WARN("订单#{} {} 被拒绝: {}", e.order.id, e.order.symbol, e.order.note);
    }
  });
  bus_.closes().subscribe([this, strat0](const SessionCloseEvent& e) {
    OrderAgent agent(*this);
    for (auto& s : strategies_) s->on_session_close(agent, e.day);
  });

  QD_INFO("引擎装配完成: 初始权益 {:.0f} CNY (CNY {:.0f} / HKD {:.0f} / USD {:.0f}), 标的 {} 个, 模拟 {} 个交易日",
          initial_equity_cny_, capital, cfg_.num("account.capital_hkd", 0.0),
          cfg_.num("account.capital_usd", 0.0), universe.size(),
          cfg_.integer("sim.days", 60));
  return true;
}

double Engine::cash_for(const std::string& symbol) const {
  auto m = MarketRules::market_of(symbol);
  if (!m) return 0.0;
  return pf_.cash(MarketRules::of(*m).ccy);
}

double Engine::cny_to(const std::string& symbol, double cny_amount) const {
  auto m = MarketRules::market_of(symbol);
  if (!m) return cny_amount;
  return pf_.convert(cny_amount, Currency::CNY, MarketRules::of(*m).ccy);
}

uint64_t Engine::submit_order(const OrderRequest& req) {
  auto m = MarketRules::market_of(req.symbol);
  Order o;
  o.id = next_order_id_++;
  o.symbol = req.symbol;
  o.side = req.side;
  o.type = req.type;
  o.qty = req.qty;
  o.limit_price = req.limit_price;
  o.day = cur_day_;
  if (!m) {
    o.status = OrderStatus::Rejected;
    o.note = "无法识别市场后缀";
    bus_.orders().publish(OrderEvent{o});
    return o.id;
  }
  o.market = *m;
  if (!gateways_.contains(*m)) {
    o.status = OrderStatus::Rejected;
    o.note = "该市场未启用";
    bus_.orders().publish(OrderEvent{o});
    return o.id;
  }

  Price ref = feed_.last_close().count(req.symbol) ? feed_.last_close().at(req.symbol) : 0.0;
  if (auto reason = risk_.check(o, ref, pf_, orders_today_)) {
    o.status = OrderStatus::Rejected;
    o.note = *reason;
    bus_.orders().publish(OrderEvent{o});
    return o.id;
  }
  ++orders_today_;
  gateways_.at(*m)->place(o, ref);
  return o.id;
}

int Engine::run() {
  QD_INFO("==================== 交易开始 ====================");

  while (auto bars = feed_.next_day()) {
    cur_day_ = feed_.current_day();
    orders_today_ = 0;

    for (auto& [m, gw] : gateways_) gw->on_session_open(cur_day_);
    bus_.opens().publish(SessionOpenEvent{cur_day_});

    // 先撮合昨日挂单（按今日 bar 的开盘/高低），再向策略推送行情
    for (const auto& bar : *bars) {
      for (auto& [m, gw] : gateways_) gw->on_bar(bar);
      pf_.mark(bar.symbol, bar.close);
      bus_.bars().publish(BarEvent{bar});
    }

    for (auto& [m, gw] : gateways_) gw->on_session_close(cur_day_);
    pf_.settle();   // T+1 解冻
    bus_.closes().publish(SessionCloseEvent{cur_day_});
    record_equity();

    if (feed_.days_elapsed() % 10 == 0) {
      QD_INFO("[{}] 净值 {:.2f} CNY (第 {}/{} 个交易日)", to_iso(cur_day_),
              pf_.total_equity_cny(), feed_.days_elapsed(),
              cfg_.integer("sim.days", 60));
    }
  }

  QD_INFO("==================== 交易结束 ====================");
  write_reports();
  return 0;
}

void Engine::record_equity() {
  equity_curve_.push_back({
      cur_day_,
      pf_.total_equity_cny(),
      pf_.total_equity_cny() - pf_.positions_value_cny(),
      pf_.positions_value_cny(),
      pf_.unrealized_pnl_cny(),
      pf_.total_realized_pnl_cny(),
  });
}

void Engine::write_reports() {
  const std::string dir = cfg_.str("report.dir", "output");

  // ---- 净值曲线 ----
  {
    std::ofstream f(dir + "/equity.csv");
    f << "date,equity_cny,cash_cny,positions_cny,unrealized_pnl_cny,realized_pnl_cny\n";
    for (const auto& p : equity_curve_) {
      f << to_iso(p.day) << ',' << p.equity << ',' << p.cash << ',' << p.positions
        << ',' << p.unrealized << ',' << p.realized << '\n';
    }
  }

  // ---- 成交明细 ----
  {
    std::ofstream f(dir + "/trades.csv");
    f << "trade_id,order_id,day,symbol,market,side,qty,price,fee,ccy\n";
    for (const auto& t : all_trades_) {
      f << t.id << ',' << t.order_id << ',' << to_iso(t.day) << ',' << t.symbol
        << ',' << to_string(t.market) << ',' << to_string(t.side) << ',' << t.qty
        << ',' << t.price << ',' << t.fee << ',' << to_string(t.ccy) << '\n';
    }
  }

  // ---- 持仓 ----
  {
    std::ofstream f(dir + "/positions.csv");
    f << "symbol,market,ccy,total_qty,available_qty,avg_price,last_price,market_value_ccy,unrealized_pnl_ccy\n";
    for (const auto& [sym, pos] : pf_.positions()) {
      Price last = pf_.last_mark(sym);
      f << sym << ',' << to_string(pos.market) << ',' << to_string(pos.ccy) << ','
        << pos.total_qty << ',' << pos.available_qty << ',' << pos.avg_price << ','
        << last << ',' << last * pos.total_qty << ','
        << (last - pos.avg_price) * pos.total_qty << '\n';
    }
  }

  // ---- 业绩摘要 ----
  double final_eq = pf_.total_equity_cny();
  double ret = final_eq / initial_equity_cny_ - 1.0;
  double peak = 0.0, max_dd = 0.0;
  for (const auto& p : equity_curve_) {
    peak = std::max(peak, p.equity);
    max_dd = std::max(max_dd, (peak - p.equity) / peak);
  }
  int wins = 0;
  for (const auto& rt : pf_.round_trips()) {
    if (rt.close_price - rt.open_price > 0) ++wins;
  }
  int total_rt = static_cast<int>(pf_.round_trips().size());

  std::ostringstream summary;
  summary << std::fixed << std::setprecision(2);
  summary << "================ 业绩摘要 ================\n";
  summary << "初始资金:   " << initial_equity_cny_ << " CNY\n";
  summary << "期末权益:   " << final_eq << " CNY\n";
  summary << "总收益率:   " << std::setprecision(4) << ret * 100 << "%\n";
  summary << "最大回撤:   " << max_dd * 100 << "%\n";
  summary << "已实现盈亏: " << std::setprecision(2) << pf_.total_realized_pnl_cny() << " CNY\n";
  summary << "浮动盈亏:   " << pf_.unrealized_pnl_cny() << " CNY\n";
  summary << "成交笔数:   " << all_trades_.size() << "\n";
  summary << "委托笔数:   " << all_orders_.size() << "\n";
  summary << "平仓回合:   " << total_rt << " (盈利 " << wins << ", 胜率 "
          << (total_rt ? 100.0 * wins / total_rt : 0.0) << "%)\n";
  summary << "==========================================\n";

  const std::string text = summary.str();
  std::cout << '\n' << text;
  { std::ofstream f(dir + "/summary.txt"); f << text; }
  QD_INFO("报告已写入 {}/ (equity.csv, trades.csv, positions.csv, summary.txt)", dir);
}

}  // namespace quantd
