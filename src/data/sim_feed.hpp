// quantd — 模拟行情源。
// 用几何布朗运动(GBM)生成日线 bar，固定随机种子保证可复现。
// 接口设计为"逐日推进"，替换成真实行情（如读取历史 CSV 回放、
// 或接实时行情推送）时只需重写本类，引擎与策略零改动。
#pragma once

#include <map>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "core/config.hpp"
#include "core/types.hpp"

namespace quantd {

struct SimParams {
  Price init_price{};
  double annual_vol{};    // 年化波动率
  double annual_drift{};  // 年化漂移
  double daily_volume{};  // 日均成交量（股）
};

class SimFeed {
 public:
  // universe: 交易标的列表；market 决定交易日历推进
  void init(const Config& cfg, const std::vector<std::string>& universe);

  // 推进一个交易日：找到对"所有已启用市场"都是交易日的下一天。
  // 返回当日全部 bar（按 symbol 字典序），模拟期结束返回 nullopt。
  std::optional<std::vector<Bar>> next_day();

  // 上一交易日各标的收盘价（供风控做价格偏离检查）
  const std::map<std::string, Price>& last_close() const { return last_close_; }

  TradingDay current_day() const { return cur_day_; }
  int days_elapsed() const { return days_done_; }

 private:
  struct State {
    SimParams params;
    Price prev_close{};
    std::mt19937_64 rng;
  };

  std::map<std::string, State> states_;
  std::map<std::string, Price> last_close_;
  TradingDay cur_day_{};
  int days_done_{};
  int total_days_{};
  std::vector<Market> markets_{};
};

}  // namespace quantd
