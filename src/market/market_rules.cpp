#include "market/market_rules.hpp"

#include <algorithm>
#include <cmath>
#include <map>

namespace quantd {

namespace {

// 港股常见标的每手表（董事会Lot Size因股票而异；未收录的按配置默认值）
const std::map<std::string, int> kHkLots = {
    {"00700.HK", 100},   // 腾讯控股
    {"00005.HK", 100},   // 汇丰控股
    {"00388.HK", 100},   // 香港交易所
    {"09988.HK", 100},   // 阿里巴巴
    {"03690.HK", 100},   // 美团
    {"00941.HK", 500},   // 中国移动
    {"01299.HK", 200},   // 友邦保险? (以交易所为准)
    {"01810.HK", 200},   // 小米集团
};

const MarketRules kRules[] = {
    // ---- A股：T+1、±10%涨跌停（主板示例）、100股整手买入 ----
    {.market = Market::CN,
     .name = "A股",
     .ccy = Currency::CNY,
     .t_plus_one = true,
     .price_limit_pct = 0.10,
     .default_lot = 100,
     .fees = {.commission_bps = 2.5,    // 万2.5佣金
              .min_commission = 5.0,    // 最低5元
              .stamp_sell_bps = 5.0,    // 印花税 0.05%（2023-08减半后）
              .transfer_bps = 0.1}},    // 过户费 万0.1
    // ---- 港股：T+0、无涨跌停、每手因股票而异 ----
    {.market = Market::HK,
     .name = "港股",
     .ccy = Currency::HKD,
     .t_plus_one = false,
     .price_limit_pct = 0.0,
     .default_lot = 100,
     .fees = {.commission_bps = 3.0,    // 万3佣金
              .min_commission = 3.0,    // 最低3港元
              .stamp_sell_bps = 10.0,   // 印花税 0.1%
              .transfer_bps = 1.3}},    // 交易征费/交易费等（合并简化）
    // ---- 美股：T+0、无涨跌停、1股即可交易 ----
    {.market = Market::US,
     .name = "美股",
     .ccy = Currency::USD,
     .t_plus_one = false,
     .price_limit_pct = 0.0,
     .default_lot = 1,
     .fees = {.commission_bps = 0.0,    // 主流券商零佣金
              .min_commission = 0.0,
              .stamp_sell_bps = 0.0,    // 无印花税（SEC费率极低，忽略）
              .transfer_bps = 0.0}},
};

}  // namespace

Price MarketRules::tick_size(Price p) const {
  if (market != Market::HK) return 0.01;
  // 港股价位表（简化分档，生产请以 HKEX 官方位价表为准）
  if (p < 1.0) return 0.001;
  if (p < 10.0) return 0.01;
  if (p < 20.0) return 0.02;
  if (p < 100.0) return 0.05;
  if (p < 200.0) return 0.1;
  if (p < 500.0) return 0.2;
  if (p < 1000.0) return 0.5;
  return 1.0;
}

Price MarketRules::trade_fee(Side side, Price notional) const {
  if (notional <= 0) return 0.0;
  Price fee = notional * fees.commission_bps / 10000.0;
  fee = std::max(fee, fees.min_commission);
  fee += notional * fees.transfer_bps / 10000.0;
  if (side == Side::Sell) fee += notional * fees.stamp_sell_bps / 10000.0;
  // 四舍五入到分
  return std::round(fee * 100.0) / 100.0;
}

std::optional<std::string_view> MarketRules::market_suffix_error(
    const std::string& symbol) {
  if (!market_of(symbol)) {
    return "标的必须带市场后缀: .SH/.SZ/.BJ(A股) .HK(港股) .US(美股)";
  }
  return std::nullopt;
}

std::optional<Market> MarketRules::market_of(const std::string& symbol) {
  auto ends = [&](const char* sfx) {
    size_t n = std::char_traits<char>::length(sfx);
    return symbol.size() > n && symbol.compare(symbol.size() - n, n, sfx) == 0;
  };
  if (ends(".SH") || ends(".SZ") || ends(".BJ")) return Market::CN;
  if (ends(".HK")) return Market::HK;
  if (ends(".US")) return Market::US;
  return std::nullopt;
}

int MarketRules::lot_of(const std::string& symbol) {
  auto it = kHkLots.find(symbol);
  if (it != kHkLots.end()) return it->second;
  auto m = market_of(symbol);
  if (!m) return 1;
  return of(*m).default_lot;
}

const MarketRules& MarketRules::of(Market m) {
  return kRules[static_cast<uint8_t>(m)];
}

double to_cny(double amount, Currency ccy, double hkd_cny, double usd_cny) {
  switch (ccy) {
    case Currency::CNY: return amount;
    case Currency::HKD: return amount * hkd_cny;
    case Currency::USD: return amount * usd_cny;
  }
  return amount;
}

}  // namespace quantd
