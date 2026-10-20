#!/usr/bin/env python3
"""
QMT(迅投) A股实盘桥 — 完整实现。

部署形态：在登录了 QMT 客户端(极简模式/miniQMT)的 Windows 机器上运行：
    python qmt_bridge.py --port 17001 --account <资金账号> --qmt-path "<QMT安装目录>"

quantd(C++) 通过 TCP 连接本桥，行协议见 src/core/protocol.hpp。

依赖：
    pip install xtquant
    QMT 客户端已登录，且开启了极简模式(miniQMT)。

命令行参数：
    --port      监听端口（默认 17001，与 quantd 配置 gateway.qmt.port 对应）
    --account   资金账号（QMT 客户端登录的账号）
    --qmt-path  QMT 安装目录（用于定位 userdata_mini；部分版本可自动探测）

市价单说明：
    A股没有交易所级市价单的统一实现，本桥将 MKT 翻译为
    "最新对手价限价单"（查 tick 的 ask1/bid1），滑点可控。
    如券商支持市价委托类型，可改 xtconstant 中的市价常量。

风险提示：
    本桥会真实下单。请先在 quantd 配置 live.shadow = true 完成影子验收，
    再开 live.i_confirm_real_money = true。
"""
import argparse
import socket
import sys
import threading
import time

PROTOCOL_VERSION = "qmt-bridge/1.0"


def log(msg: str) -> None:
    sys.stderr.write(f"[qmt_bridge] {msg}\n")
    sys.stderr.flush()


def try_import_xtquant():
    try:
        from xtquant import xtconstant, xttrader, xtdata
        from xtquant.xttype import StockAccount
        return xtconstant, xttrader, xtdata, StockAccount
    except ImportError:
        return None


class QmtBridge:
    def __init__(self, account: str, qmt_path: str):
        x = try_import_xtquant()
        if x is None:
            raise RuntimeError(
                "未安装 xtquant（需要 QMT 客户端环境）。"
                "本桥拒绝启动，防止被误当作实盘通道。")
        self.xtconstant, self.xttrader, self.xtdata, self.StockAccount = x
        self.account_id = account
        self.qmt_path = qmt_path
        self.trader = None
        self.account = None
        self.trade_seq = 0
        self.lock = threading.Lock()

    # ---------------- 连接券商 ----------------
    def connect_broker(self) -> bool:
        import random
        session_id = random.randint(100000, 999999)
        userdata = self.qmt_path.rstrip("\\/") + "\\userdata_mini"
        self.trader = self.xttrader.XtQuantTrader(userdata, session_id)
        # 注册回报回调（成交/委托状态由券商推送）
        self.trader.register_callback(self._make_callbacks())
        if self.trader.start() != 0:
            log("XtQuantTrader.start() 失败：请确认 QMT 客户端已登录且为极简模式")
            return False
        if not self.trader.connect():
            log("连接 miniQMT 失败：请确认 QMT 客户端正在运行")
            return False
        self.account = self.StockAccount(self.account_id)
        if not self.trader.subscribe(self.account):
            log("订阅账户失败：请核对资金账号")
            return False
        log(f"已连接 miniQMT，账号 {self.account_id}（{PROTOCOL_VERSION}）")
        return True

    def _make_callbacks(self):
        xttrader = self.xttrader
        bridge = self
        sock_ref = {"conn": None}  # 由 serve() 注入，用于向引擎推送

        class CB(xttrader.XtQuantTraderCallback):
            def on_disconnected(self):
                log("与 miniQMT 断开连接！")

            def on_stock_order(self, order):
                # 委托状态变化。order.order_status: 48=已报 49=部成
                # 50=全成 51=部撤 52=已撤 53=未报 55=拒单...
                try:
                    status = {
                        48: "accepted", 49: "accepted", 50: "accepted",
                        53: "accepted",
                        51: "cancelled", 52: "cancelled",
                        55: "rejected",
                    }.get(order.order_status, "accepted")
                    reason = ""
                    if status == "rejected":
                        reason = f" status_reason={order.status_msg}"
                    bridge._push(sock_ref,
                                 f"ack id={order.order_remark} status={status}{reason}")
                except Exception as e:  # noqa: BLE001
                    log(f"on_stock_order 异常: {e}")

            def on_stock_trade(self, trade):
                # 逐笔成交回报（一单可能多笔）
                try:
                    bridge.trade_seq += 1
                    fee = bridge._est_fee(trade)
                    bridge._push(
                        sock_ref,
                        f"trade id={bridge.trade_seq} order_id={trade.order_remark}"
                        f" symbol={trade.stock_code} price={trade.traded_price:.2f}"
                        f" qty={trade.traded_volume} fee={fee:.2f}")
                except Exception as e:  # noqa: BLE001
                    log(f"on_stock_trade 异常: {e}")

            def on_order_error(self, order_error):
                try:
                    bridge._push(sock_ref,
                                 f"ack id={order_error.order_remark} "
                                 f"status=rejected reason={order_error.error_msg}")
                except Exception as e:  # noqa: BLE001
                    log(f"on_order_error 异常: {e}")

        return CB()

    # ---------------- 工具 ----------------
    def _push(self, sock_ref, line: str):
        conn = sock_ref.get("conn")
        if conn is None:
            log(f"(未连接引擎，丢弃) {line}")
            return
        with self.lock:
            try:
                conn.sendall((line + "\n").encode("utf-8"))
            except OSError:
                log(f"推送失败: {line}")

    def _est_fee(self, trade) -> float:
        """成交费用估算（佣金万2.5最低5元 + 过户万0.1 + 卖印花万5）。
        真实费用以券商结算为准；xtquant 回报不含费用字段。"""
        notional = trade.traded_price * trade.traded_volume
        fee = max(notional * 2.5 / 10000.0, 5.0) + notional * 0.1 / 10000.0
        # trade_offset: 0=开仓(买) 1=平仓(卖)（xttype 常量）
        if getattr(trade, "trade_type", 0) in (57,):  # SELL
            fee += notional * 5.0 / 10000.0
        return fee

    def _opposite_price(self, symbol: str, side: str, ref: float) -> float:
        """取最新对手价（MKT→限价翻译）；取不到时退回引擎给的参考价"""
        try:
            tick = self.xtdata.get_full_tick([symbol])[symbol]
            if side == "BUY" and tick.get("askPrice") and tick["askPrice"][0] > 0:
                return tick["askPrice"][0]
            if side == "SELL" and tick.get("bidPrice") and tick["bidPrice"][0] > 0:
                return tick["bidPrice"][0]
        except Exception as e:  # noqa: BLE001
            log(f"取 tick 失败({symbol}): {e}，退回参考价 {ref}")
        return ref

    # ---------------- 协议操作 ----------------
    def handle_line(self, conn, line: str, sock_ref) -> None:
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
            # 现金快照（A股市值/可用由 pos 行补充）
            asset = self.trader.query_stock_asset(self.account)
            if asset is not None:
                self._push(sock_ref, f"cash ccy=CNY amount={asset.cash:.2f}")
            positions = self.trader.query_stock_positions(self.account)
            if positions:
                for p in positions:
                    if p.volume > 0:
                        self._push(sock_ref,
                                   f"pos symbol={p.stock_code} qty={p.volume}"
                                   f" available={p.can_use_volume}"
                                   f" avg={p.open_price:.3f}")

        elif op == "order":
            oid = kv["id"]
            symbol = kv["symbol"]
            side = kv["side"]
            qty = int(kv["qty"])
            otype = kv.get("type", "MKT")
            price = float(kv.get("price", "0") or 0)
            ref = float(kv.get("ref", "0") or 0)

            xt_side = (self.xtconstant.STOCK_BUY if side == "BUY"
                       else self.xtconstant.STOCK_SELL)
            if otype == "MKT":
                # A股无统一市价单 → 对手价限价单
                price = self._opposite_price(symbol, side, ref)
                if price <= 0:
                    self._push(sock_ref,
                               f"ack id={oid} status=rejected reason=无参考价")
                    return
                price_type = self.xtconstant.FIX_PRICE
            else:
                price_type = self.xtconstant.FIX_PRICE

            # order_remark 携带引擎订单号，回报据此关联
            err = self.trader.order_stock(
                self.account, symbol, xt_side, qty, price_type, price,
                strategy_name="quantd", order_remark=oid)
            if err != 0:
                self._push(sock_ref,
                           f"ack id={oid} status=rejected reason=下单返回{err}")
            # 成功路径的 ack 由 on_stock_order 回调推送

        elif op == "cancel":
            oid = kv["id"]
            # 需要将引擎订单号映射到券商委托号：v1 通过今日委托查询
            orders = self.trader.query_stock_orders(self.account)
            if orders:
                for o in orders:
                    if o.order_remark == oid and o.order_status in (48, 49, 53):
                        self.trader.cancel_order_stock(self.account, o.stock_order)

        elif op == "shutdown":
            log("收到 shutdown，退出")
            conn.close()
            sys.exit(0)

        else:
            log(f"未知 op: {op}")


def main() -> int:
    ap = argparse.ArgumentParser(description="quantd QMT 实盘桥")
    ap.add_argument("--port", type=int, default=17001)
    ap.add_argument("--account", required=True, help="资金账号")
    ap.add_argument("--qmt-path", required=True, help="QMT 安装目录")
    args = ap.parse_args()

    bridge = QmtBridge(args.account, args.qmt_path)
    if not bridge.connect_broker():
        return 3

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(1)
    log(f"桥就绪，等待引擎连接 127.0.0.1:{args.port}")

    conn, _ = srv.accept()
    log("引擎已连接")
    # 注入推送通道（回调线程 -> 引擎）
    sock_ref = {"conn": conn}
    bridge._push(sock_ref, f"log msg={PROTOCOL_VERSION} 已连接券商")

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
                    bridge.handle_line(conn, text, sock_ref)
    except KeyboardInterrupt:
        log("手动停止")
    finally:
        conn.close()
        srv.close()
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
