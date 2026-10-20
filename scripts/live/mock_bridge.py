#!/usr/bin/env python3
"""
Mock 桥 — 模拟 QMT 桥行为，用于无券商环境下的通道联调。

作用：在部署真 QMT 之前，验证 quantd 引擎 ↔ 桥 的完整链路：
TCP 连接、ping/pong 握手、query_asset 对账、order 下单、
ack/trade 回报、影子模式拦截。跑通本桥 = 通道代码就绪，
之后换 qmt_bridge.py 即接真券商。

模拟行为：
    - 账户现金 200,000 CNY，持仓 600519.SH x100（可卖100，均价1400）
    - 收到订单：立即回 ack accepted + 模拟成交（按参考价+0.1%滑点，
      费用按 A股规则估算）
    - 用 --reject-rate N 可随机拒单 N% 用于测试拒单路径

用法：
    python3 mock_bridge.py --port 17001
    python3 mock_bridge.py --port 17001 --reject-rate 100   # 全拒单测试
"""
import argparse
import random
import socket
import sys
import threading
import time

VERSION = "mock-bridge/1.0"


def log(msg: str) -> None:
    sys.stderr.write(f"[mock_bridge] {msg}\n")
    sys.stderr.flush()


def est_fee(side: str, price: float, qty: int) -> float:
    notional = price * qty
    fee = max(notional * 2.5 / 10000.0, 5.0) + notional * 0.1 / 10000.0
    if side == "SELL":
        fee += notional * 5.0 / 10000.0
    return round(fee, 2)


class MockBridge:
    def __init__(self, reject_rate: int = 0):
        self.reject_rate = reject_rate
        self.cash = 200000.0
        self.positions = {"600519.SH": (100, 100, 1400.0)}  # (qty, available, avg)
        self.trade_seq = 0
        self.lock = threading.Lock()

    def handle(self, conn, line: str, push) -> None:
        parts = line.split()
        op = parts[0] if parts else ""
        kv = {}
        for p in parts[1:]:
            if "=" in p:
                k, v = p.split("=", 1)
                kv[k] = v

        if op == "ping":
            conn.sendall(b"pong\n")

        elif op == "query_asset":
            push(f"cash ccy=CNY amount={self.cash:.2f}")
            for sym, (qty, avail, avg) in self.positions.items():
                if qty > 0:
                    push(f"pos symbol={sym} qty={qty} available={avail} avg={avg:.2f}")
            push("log msg=对账完成(mock)")

        elif op == "order":
            oid = kv["id"]
            symbol = kv["symbol"]
            side = kv["side"]
            qty = int(kv["qty"])
            otype = kv.get("type", "MKT")
            price = float(kv.get("price", "0") or 0)
            ref = float(kv.get("ref", "0") or 0)

            # 随机拒单测试路径
            if random.randint(1, 100) <= self.reject_rate:
                push(f"ack id={oid} status=rejected reason=mock随机拒单")
                return

            # 模拟成交价：限价单用限价，市价单用参考价 + 0.1% 滑点
            if otype == "LMT" and price > 0:
                fill_px = price
            else:
                fill_px = round(ref * (1.001 if side == "BUY" else 0.999), 2)
            if fill_px <= 0:
                push(f"ack id={oid} status=rejected reason=无参考价")
                return

            # 买入现金检查 / 卖出持仓检查（模拟券商风控）
            fee = est_fee(side, fill_px, qty)
            with self.lock:
                if side == "BUY":
                    if fill_px * qty + fee > self.cash:
                        push(f"ack id={oid} status=rejected reason=资金不足(mock)")
                        return
                    self.cash -= fill_px * qty + fee
                    q, a, avg = self.positions.get(symbol, (0, 0, 0.0))
                    new_qty = q + qty
                    avg = ((avg * q) + fill_px * qty) / new_qty if new_qty else 0
                    # mock 简化为 T+0（买入即可卖），便于联调完整买卖回合；
                    # 真实 A股 T+1 由券商侧把关、引擎侧 Portfolio 同步管理
                    self.positions[symbol] = (new_qty, a + qty, avg)
                else:
                    q, a, avg = self.positions.get(symbol, (0, 0, 0.0))
                    if a < qty:
                        push(f"ack id={oid} status=rejected reason=可卖不足(mock)")
                        return
                    self.positions[symbol] = (q - qty, a - qty, avg)
                    self.cash += fill_px * qty - fee

            push(f"ack id={oid} status=accepted")
            self.trade_seq += 1
            push(f"trade id={self.trade_seq} order_id={oid} symbol={symbol}"
                 f" side={side} price={fill_px} qty={qty} fee={fee}")
            log(f"模拟成交: {side} {symbol} x{qty} @ {fill_px} 费{fee}"
                f" | 现金余 {self.cash:.2f}")

        elif op == "cancel":
            push(f"ack id={kv.get('id', '?')} status=cancelled")

        else:
            log(f"未知 op: {op}")


def main() -> int:
    ap = argparse.ArgumentParser(description="quantd 通道联调 mock 桥")
    ap.add_argument("--port", type=int, default=17001)
    ap.add_argument("--reject-rate", type=int, default=0,
                    help="随机拒单百分比（0-100），测试拒单路径")
    args = ap.parse_args()

    bridge = MockBridge(args.reject_rate)
    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    log(f"{VERSION} 就绪 127.0.0.1:{args.port}"
        f"（现金 {bridge.cash:.0f} CNY, 持仓 600519.SH x100）")

    conn, _ = srv.accept()
    log("引擎已连接")

    def push(line: str) -> None:
        with bridge.lock:
            try:
                conn.sendall((line + "\n").encode("utf-8"))
            except OSError:
                pass

    buf = b""
    try:
        while True:
            data = conn.recv(4096)
            if not data:
                log("引擎断开连接")
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                text = line.decode("utf-8", errors="replace").strip()
                if text:
                    log(f"<< {text}")
                    bridge.handle(conn, text, push)
    except KeyboardInterrupt:
        pass
    finally:
        conn.close()
        srv.close()
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
