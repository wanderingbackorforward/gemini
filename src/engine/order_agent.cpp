#include "engine/order_agent.hpp"

#include "engine/engine.hpp"

namespace quantd {

uint64_t OrderAgent::market_order(const std::string& symbol, Side side, Qty qty) {
  return engine_.submit_order({symbol, side, OrderType::Market, qty, 0.0});
}

uint64_t OrderAgent::limit_order(const std::string& symbol, Side side, Qty qty, Price px) {
  return engine_.submit_order({symbol, side, OrderType::Limit, qty, px});
}

const Position* OrderAgent::position(const std::string& symbol) const {
  return engine_.portfolio().position(symbol);
}

double OrderAgent::equity_cny() const {
  return engine_.portfolio().total_equity_cny();
}

Price OrderAgent::last_close(const std::string& symbol) const {
  return engine_.last_close(symbol);
}

int OrderAgent::lot_of(const std::string& symbol) const {
  return engine_.lot_of(symbol);
}

double OrderAgent::cash_for(const std::string& symbol) const {
  return engine_.cash_for(symbol);
}

double OrderAgent::cny_to(const std::string& symbol, double cny_amount) const {
  return engine_.cny_to(symbol, cny_amount);
}

}  // namespace quantd
