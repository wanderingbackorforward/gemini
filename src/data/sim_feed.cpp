#include "data/sim_feed.hpp"

#include <algorithm>
#include <cmath>

#include "market/calendar.hpp"
#include "market/market_rules.hpp"

namespace quantd {

void SimFeed::init(const Config& cfg, const std::vector<std::string>& universe) {
  total_days_ = static_cast<int>(cfg.integer("sim.days", 60));
  const uint64_t seed = static_cast<uint64_t>(cfg.integer("sim.seed", 42));
  int seq = 0;

  for (const auto& sym : universe) {
    auto m = MarketRules::market_of(sym);
    if (!m) continue;
    if (std::find(markets_.begin(), markets_.end(), *m) == markets_.end()) {
      markets_.push_back(*m);
    }

    // 每个标的独立子随机流（种子 = 全局种子 + 序号），可复现且互不干扰
    State st;
    st.params.init_price = cfg.num("sim." + sym + ".price", 100.0);
    st.params.annual_vol = cfg.num("sim." + sym + ".vol", 0.25);
    st.params.annual_drift = cfg.num("sim." + sym + ".drift", 0.05);
    st.params.daily_volume = cfg.num("sim." + sym + ".volume", 1e6);
    st.prev_close = st.params.init_price;
    st.rng.seed(seed + static_cast<uint64_t>(seq));
    ++seq;
    states_[sym] = std::move(st);
  }

  // 解析起始日期（默认今天）
  cur_day_ = std::chrono::year_month_day{
      std::chrono::year{static_cast<int>(cfg.integer("sim.start.year", 2026))},
      std::chrono::month{static_cast<unsigned>(cfg.integer("sim.start.month", 10))},
      std::chrono::day{static_cast<unsigned>(cfg.integer("sim.start.day", 20))}};
  // 起始日须对所有市场都是交易日（模拟三市场共用一个交易日轴）
  bool all_open = !markets_.empty();
  for (auto m : markets_) all_open = all_open && is_trading_day(m, cur_day_);
  if (!all_open) {
    // 逐日后推直至所有市场同开
    for (;;) {
      cur_day_ = next_trading_day(markets_[0], cur_day_);
      all_open = true;
      for (auto m : markets_) all_open = all_open && is_trading_day(m, cur_day_);
      if (all_open) break;
    }
  }
}

std::optional<std::vector<Bar>> SimFeed::next_day() {
  if (days_done_ >= total_days_ || states_.empty()) return std::nullopt;
  if (days_done_ > 0) {
    // 推进到下一个"三市场共同交易日"
    for (;;) {
      cur_day_ = next_trading_day(markets_[0], cur_day_);
      bool all_open = true;
      for (auto m : markets_) all_open = all_open && is_trading_day(m, cur_day_);
      if (all_open) break;
    }
  }
  ++days_done_;

  // 日线粒度 → 年化参数折算
  constexpr double kDaysPerYear = 252.0;
  std::vector<Bar> bars;
  bars.reserve(states_.size());
  for (auto& [sym, st] : states_) {
    auto m = MarketRules::market_of(sym);
    Bar bar;
    bar.symbol = sym;
    bar.market = *m;
    bar.day = cur_day_;
    bar.open = st.prev_close;

    double dt = 1.0 / kDaysPerYear;
    std::normal_distribution<double> nd(
        st.params.annual_drift * dt - 0.5 * st.params.annual_vol * st.params.annual_vol * dt,
        st.params.annual_vol * std::sqrt(dt));
    double ret = nd(st.rng);
    bar.close = std::max(st.prev_close * std::exp(ret), 0.01);

    // 日内高低：以收盘为锚的随机振幅
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    double span = std::abs(bar.close - bar.open) + bar.close * std::abs(ret) + 1e-9;
    bar.high = std::max(bar.open, bar.close) + span * ud(st.rng) * 0.5;
    bar.low  = std::min(bar.open, bar.close) - span * ud(st.rng) * 0.5;
    bar.low  = std::max(bar.low, 0.005);
    bar.volume = st.params.daily_volume * (0.6 + 0.8 * ud(st.rng));

    st.prev_close = bar.close;
    last_close_[sym] = bar.close;
    bars.push_back(bar);
  }
  return bars;
}

}  // namespace quantd
