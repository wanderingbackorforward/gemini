// quantd — 主引擎。
// 单线程事件循环（LMAX 模型）：行情 → 策略 → 风控 → 网关 → 组合。
// 事件顺序确定，天然可复现（回测/实盘同构，实盘只需替换
// SimFeed 为实时行情、PaperGateway 为券商网关）。
#pragma once

#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/event_bus.hpp"
#include "data/sim_feed.hpp"
#include "gateway/gateway.hpp"
#include "market/market_rules.hpp"
#include "portfolio/portfolio.hpp"
#include "risk/risk_manager.hpp"
#include "strategy/strategy.hpp"

namespace quantd {

class OrderAgent;

class Engine {
 public:
  explicit Engine(Config cfg) : cfg_(std::move(cfg)) {}

  // 装配所有组件；失败返回 false
  bool init();
  // 运行完整模拟/交易周期；返回进程退出码
  int run();

  // ---- OrderAgent 后端（策略经此提交订单）----
  uint64_t submit_order(const OrderRequest& req);

  // ---- 供 OrderAgent 查询 ----
  const Portfolio& portfolio() const { return pf_; }
  Price last_close(const std::string& symbol) const { return feed_.last_close().count(symbol) ? feed_.last_close().at(symbol) : 0.0; }
  int lot_of(const std::string& symbol) const { return MarketRules::lot_of(symbol); }
  // 该标的结算币种的可用现金（多币种账户约束）
  double cash_for(const std::string& symbol) const;
  // 该标的该方向在途订单量（已提交未终结，跨网关汇总）
  Qty pending_qty(const std::string& symbol, Side side) const;
  // CNY 金额折算为该标的结算币种
  double cny_to(const std::string& symbol, double cny_amount) const;

 private:
  friend class OrderAgent;

  void record_equity();
  void write_reports();
  // 消费 live 网关消息；settle_ms>0 时等待对账类消息到位
  void poll_live(int settle_ms = 0);

  Config cfg_;
  EventBus bus_;
  Portfolio pf_;
  RiskManager risk_;
  SimFeed feed_;
  std::vector<std::unique_ptr<Strategy>> strategies_;
  std::map<Market, std::unique_ptr<IGateway>> gateways_;
  std::vector<std::unique_ptr<std::ofstream>> report_files_;

  TradingDay cur_day_{};
  uint64_t next_order_id_ = 1;
  int orders_today_ = 0;
  double initial_equity_cny_ = 0.0;

  // ---- 实盘模式状态 ----
  bool live_mode_ = false;                 // 任一网关为 live 即置位
  double live_max_order_value_cny_ = 5000; // 单笔硬保险丝（折CNY）
  double live_daily_loss_limit_cny_ = 0;   // 当日亏损熔断（0=关闭）
  double day_start_equity_cny_ = 0;        // 当日开盘权益（熔断基准）

  // 报告数据
  struct EquityPoint {
    TradingDay day;
    double equity, cash, positions, unrealized, realized;
  };
  std::vector<EquityPoint> equity_curve_;
  std::vector<Trade> all_trades_;
  std::vector<Order> all_orders_;
};

}  // namespace quantd
