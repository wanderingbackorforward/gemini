#include "gateway/live_gateways.hpp"

#include <chrono>
#include <cstring>

#include "core/log.hpp"
#include "core/protocol.hpp"
#include "market/market_rules.hpp"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
using raw_sock_t = SOCKET;
constexpr raw_sock_t kBadSock = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using raw_sock_t = int;
constexpr raw_sock_t kBadSock = -1;
#endif

namespace quantd {

namespace {

// Winsock 一次性初始化（Windows 需要，Linux 空操作）
struct SockInit {
#ifdef _WIN32
  WSADATA wsa{};
  SockInit() { WSAStartup(MAKEWORD(2, 2), &wsa); }
  ~SockInit() { WSACleanup(); }
#else
  SockInit() = default;
#endif
};
SockInit& sock_init() {
  static SockInit init;
  return init;
}

int sock_send(raw_sock_t s, const char* buf, int len) {
#ifdef _WIN32
  return static_cast<int>(send(s, buf, static_cast<int>(len), 0));
#else
  return static_cast<int>(::send(s, buf, static_cast<size_t>(len), 0));
#endif
}

void sock_close(raw_sock_t s) {
#ifdef _WIN32
  closesocket(s);
#else
  ::close(s);
#endif
}

}  // namespace

LiveGatewayBase::LiveGatewayBase(EventBus& bus, Portfolio& pf, std::string host,
                                 int port, bool shadow)
    : bus_(bus), pf_(pf), host_(std::move(host)), port_(port), shadow_(shadow) {}

LiveGatewayBase::~LiveGatewayBase() {
  sock_ok_ = false;
  if (reader_.joinable()) reader_.join();
  if (connected()) close_socket();
}

bool LiveGatewayBase::connect() {
  sock_init();
  raw_sock_t s = ::socket(AF_INET, SOCK_STREAM, 0);
  if (s == kBadSock) {
    QD_ERROR("[{}] 创建 socket 失败", name());
    return false;
  }
  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(static_cast<uint16_t>(port_));
  if (inet_pton(AF_INET, host_.c_str(), &addr.sin_addr) != 1) {
    QD_ERROR("[{}] 桥地址非法: {}", name(), host_);
    sock_close(s);
    return false;
  }
  if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
    QD_ERROR("[{}] 无法连接桥 {}:{}（请先启动桥进程，见 README）",
             name(), host_, port_);
    sock_close(s);
    return false;
  }
  sock_ = reinterpret_cast<sock_t>(s);
  sock_ok_ = true;

  // 1) 先启动读线程：所有入站数据（含 pong）由它收行入队
  reader_ = std::thread([this] {
    char buf[4096];
    std::string carry;
    while (sock_ok_) {
      int r = static_cast<int>(recv(reinterpret_cast<raw_sock_t>(sock_),
                                    buf, sizeof(buf), 0));
      if (r <= 0) {
        if (sock_ok_) QD_ERROR("[{}] 与桥的连接断开", name());
        sock_ok_ = false;
        break;
      }
      carry.append(buf, static_cast<size_t>(r));
      size_t nl;
      while ((nl = carry.find('\n')) != std::string::npos) {
        std::string line2 = carry.substr(0, nl);
        carry.erase(0, nl + 1);
        if (!line2.empty() && line2.back() == '\r') line2.pop_back();
        if (line2.empty()) continue;
        std::lock_guard<std::mutex> lk(mq_);
        queue_.push_back(std::move(line2));
      }
    }
  });

  // 2) 握手：ping/pong（3秒超时）
  if (!send_line("ping")) {
    QD_ERROR("[{}] 握手发送失败", name());
    sock_ok_ = false;
    reader_.join();
    close_socket();
    return false;
  }
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::string line;
    {
      std::lock_guard<std::mutex> lk(mq_);
      if (!queue_.empty()) {
        line = queue_.front();
        queue_.pop_front();
      }
    }
    if (!line.empty()) {
      auto msg = parse_line(line);
      if (msg["op"] == "pong") {
        on_connected();
        QD_INFO("[{}] 已连接桥 {}:{} {}", name(), host_, port_,
                shadow_ ? "（影子模式：委托将被拦截）" : "（实盘模式）");
        return true;
      }
    }
  }
  QD_ERROR("[{}] 握手超时（桥未按协议回应 pong）", name());
  sock_ok_ = false;
  reader_.join();
  close_socket();
  return false;
}

void LiveGatewayBase::on_connected() {
  // 握手成功后立即对账：以券商侧为事实源重建现金/持仓
  send_line("query_asset");
}

bool LiveGatewayBase::send_line(const std::string& line) {
  if (!sock_ok_) return false;
  std::string data = line + "\n";
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    int n = sock_send(reinterpret_cast<raw_sock_t>(sock_), p, static_cast<int>(left));
    if (n <= 0) {
      sock_ok_ = false;
      QD_ERROR("[{}] 发送失败，连接已断开", name());
      return false;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

void LiveGatewayBase::close_socket() {
  if (reinterpret_cast<raw_sock_t>(sock_) != kBadSock) {
    sock_close(reinterpret_cast<raw_sock_t>(sock_));
  }
}

void LiveGatewayBase::place(Order& o, Price ref_price) {
  if (shadow_) {
    // 影子模式：不发送，标记拦截（验收期零成本验证）
    o.status = OrderStatus::Cancelled;
    o.note = "影子模式拦截（未发送任何委托）";
    bus_.orders().publish(OrderEvent{o});
    QD_INFO("[{}] 影子拦截: #{} {} {} x{} @{}",
            name(), o.id, o.symbol, to_string(o.side), o.qty,
            o.type == OrderType::Limit ? o.limit_price : ref_price);
    return;
  }
  live_orders_[o.id] = o;   // 快照，回报时补发事件
  if (!send_line(make_line("order", {
          {"id", std::to_string(o.id)},
          {"symbol", o.symbol},
          {"side", std::string(to_string(o.side))},
          {"type", std::string(to_string(o.type))},
          {"qty", std::to_string(o.qty)},
          {"price", o.type == OrderType::Limit ? fmt_amount(o.limit_price) : "0"},
          {"ref", fmt_amount(ref_price)},
      }))) {
    o.status = OrderStatus::Rejected;
    o.note = "桥连接断开";
    bus_.orders().publish(OrderEvent{o});
    live_orders_.erase(o.id);
  }
  // 正常路径：等待桥回 ack（poll 中处理）
}

Qty LiveGatewayBase::pending_qty(const std::string& symbol, Side side) const {
  Qty n = 0;
  for (const auto& [id, o] : live_orders_) {
    if ((o.status == OrderStatus::PendingNew ||
         o.status == OrderStatus::Accepted) &&
        o.symbol == symbol && o.side == side) {
      n += o.qty;
    }
  }
  return n;
}

void LiveGatewayBase::cancel(uint64_t order_id) {
  if (!shadow_) send_line(make_line("cancel", {{"id", std::to_string(order_id)}}));
}

void LiveGatewayBase::poll() {
  for (;;) {
    std::string line;
    {
      std::lock_guard<std::mutex> lk(mq_);
      if (queue_.empty()) break;
      line = queue_.front();
      queue_.pop_front();
    }
    auto msg = parse_line(line);
    if (msg.empty()) continue;
    handle_message(msg);
  }
}

void LiveGatewayBase::handle_message(const std::map<std::string, std::string>& msg) {
  const std::string& op = msg.at("op");

  if (op == "trade") {
    // 桥回报成交：更新订单状态 + 走组合记账
    Trade t{};
    t.id = std::stoull(msg.at("id"));
    t.order_id = std::stoull(msg.at("order_id"));
    t.symbol = msg.at("symbol");
    auto m = MarketRules::market_of(t.symbol);
    if (!m) {
      QD_ERROR("[{}] 成交回报标的非法: {}", name(), t.symbol);
      return;
    }
    t.market = *m;
    // 成交方向：优先协议回传，否则按本地订单快照
    auto lit = live_orders_.find(t.order_id);
    if (msg.count("side")) {
      t.side = msg.at("side") == "SELL" ? Side::Sell : Side::Buy;
    } else if (lit != live_orders_.end()) {
      t.side = lit->second.side;
    } else {
      t.side = Side::Buy;
    }
    t.qty = std::stoll(msg.at("qty"));
    t.price = std::stod(msg.at("price"));
    t.fee = msg.count("fee") ? std::stod(msg.at("fee")) : 0.0;
    t.ccy = MarketRules::of(*m).ccy;
    t.day = std::chrono::year_month_day{
        std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};

    std::string why;
    if (!pf_.on_trade(t, &why)) {
      QD_ERROR("[{}] 券商回报无法入账: {}（{}）— 请人工核对持仓!",
               name(), t.symbol, why);
      return;
    }
    if (lit != live_orders_.end()) {
      Order o = lit->second;
      o.status = OrderStatus::Filled;
      bus_.orders().publish(OrderEvent{o});
      live_orders_.erase(lit);
    }
    bus_.trades().publish(TradeEvent{t});
    return;
  }

  if (op == "ack") {
    const uint64_t id = std::stoull(msg.at("id"));
    const std::string& status = msg.at("status");
    auto lit = live_orders_.find(id);
    if (lit != live_orders_.end()) {
      Order o = lit->second;
      if (status == "rejected") {
        o.status = OrderStatus::Rejected;
        o.note = msg.count("reason") ? msg.at("reason") : "券商拒单";
        live_orders_.erase(lit);
        bus_.orders().publish(OrderEvent{o});
        QD_WARN("[{}] 券商拒单: 订单#{} 原因: {}", name(), id, o.note);
      } else {
        o.status = OrderStatus::Accepted;
        bus_.orders().publish(OrderEvent{o});
        QD_INFO("[{}] 券商已受理订单#{}", name(), id);
      }
    }
    return;
  }

  if (op == "cash") {
    Currency c = msg.at("ccy") == "HKD" ? Currency::HKD
                : msg.at("ccy") == "USD" ? Currency::USD
                                         : Currency::CNY;
    double amt = std::stod(msg.at("amount"));
    pf_.bootstrap_cash(c, amt);
    QD_INFO("[{}] 对账: 券商现金 {} {:.2f}", name(), to_string(c), amt);
    return;
  }

  if (op == "pos") {
    Position p{};
    p.symbol = msg.at("symbol");
    auto m = MarketRules::market_of(p.symbol);
    if (!m) return;
    p.market = *m;
    p.ccy = MarketRules::of(*m).ccy;
    p.total_qty = std::stoll(msg.at("qty"));
    p.available_qty = std::stoll(msg.at("available"));
    p.avg_price = std::stod(msg.at("avg"));
    pf_.bootstrap_position(p);
    QD_INFO("[{}] 对账: 持仓 {} x{} (可卖 {}) 均价 {}",
            name(), p.symbol, p.total_qty, p.available_qty, p.avg_price);
    return;
  }

  if (op == "log") {
    QD_INFO("[{}|桥] {}", name(), msg.count("msg") ? msg.at("msg") : "");
    return;
  }
}

}  // namespace quantd
