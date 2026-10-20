// quantd — 实盘网关骨架（A股 QMT / 港美股 Futu、Longport）。
//
// ┌─────────────────────────────────────────────────────────────┐
// │ 实盘接入路线图（代码结构已就绪，装配后即可切换）：          │
// │                                                             │
// │ 1) A股 — QMT(迅投): 券商提供 xtquant(Python)。推荐以子进程  │
// │    方式运行 scripts/live/qmt_bridge.py，本类通过 stdin/     │
// │    stdout JSON 行协议与之通信。也可改用 XTP(C++) 直连。     │
// │ 2) 港美股 — Futu OpenD: 官方 futu-api(Python/协议开放)，    │
// │    同样走子进程桥 scripts/live/futu_bridge.py。             │
// │ 3) 港美股 — LongPort: 提供官方 C SDK，可直接编入本工程。    │
// │                                                             │
// │ 境内合规提示：美股交易请先确认券商资质与外汇额度。          │
// └─────────────────────────────────────────────────────────────┘
//
// 这些骨架当前处于"占位可编译"状态：place() 会明确拒绝并说明
// 原因，避免误把模拟逻辑当成实盘。
#pragma once

#include <string>

#include "gateway/gateway.hpp"

namespace quantd {

class LiveGatewayBase : public IGateway {
 public:
  LiveGatewayBase(EventBus& bus, std::string bridge_cmd)
      : bus_(bus), bridge_cmd_(std::move(bridge_cmd)) {}

  bool connect() override {
    // TODO(实盘): popen 启动桥进程，握手 {"op":"ping"}。
    return false;  // 未装配前一律连接失败，防止误当实盘
  }

  void place(Order& o, Price) override {
    o.status = OrderStatus::Rejected;
    o.note = "实盘网关未装配: " + bridge_cmd_;
    bus_.orders().publish(OrderEvent{o});
  }

  void cancel(uint64_t) override {}
  void on_session_open(TradingDay) override {}
  void on_bar(const Bar&) override {}
  void on_session_close(TradingDay) override {}

 protected:
  EventBus& bus_;
  std::string bridge_cmd_;
};

// A股实盘（QMT / 迅投 xtquant）
class QmtGateway final : public LiveGatewayBase {
 public:
  QmtGateway(EventBus& bus, const std::string& cmd)
      : LiveGatewayBase(bus, cmd) {}
  std::string name() const override { return "live_qmt(A股)"; }
};

// 港美股实盘（富途 OpenD）
class FutuGateway final : public LiveGatewayBase {
 public:
  FutuGateway(EventBus& bus, const std::string& cmd)
      : LiveGatewayBase(bus, cmd) {}
  std::string name() const override { return "live_futu(港美股)"; }
};

// 港美股实盘（长桥 LongPort，可直连官方 C SDK）
class LongportGateway final : public LiveGatewayBase {
 public:
  LongportGateway(EventBus& bus, const std::string& cmd)
      : LiveGatewayBase(bus, cmd) {}
  std::string name() const override { return "live_longport(港美股)"; }
};

}  // namespace quantd
