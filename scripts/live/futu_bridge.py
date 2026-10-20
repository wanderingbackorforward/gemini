#!/usr/bin/env python3
"""
Futu OpenD 港美股实盘桥 — 完整实现。

部署形态：在运行了 Futu OpenD 网关的机器上（Windows/macOS/Linux 均可）：
    python futu_bridge.py --port 17002
    # 模拟盘全真演练（强烈建议先跑）：
    python futu_bridge.py --port 17002 --env simulate

quantd(C++) 通过 TCP 连接本桥，行协议见 src/core/protocol.hpp。
支持多个引擎连接同时接入（港股网关 + 美股网关共用一个桥进程）；
委托回报按下单连接路由，避免重复入账。

依赖：
    1. 富途证券账户（牛牛开户）+ 开通 OpenAPI 权限（futu5.com 无资金门槛）
    2. 下载并运行 OpenD（OpenAPI 网关程序，默认 127.0.0.1:11111）
    3. pip install futu-api pandas

命令行参数：
    --port        引擎侧监听端口（默认 17002 = 配置 gateway.futu.port）
    --opend-host  OpenD 地址（默认 127.0.0.1）
    --opend-port  OpenD 端口（默认 11111）
    --env         real=实盘 / simulate=富途模拟盘（行情真实、成交模拟）
    --acc-id      交易账户 ID（默认取第一个）

市价单说明：
    美股直接用交易所市价单；港股无统一市价单，MKT 翻译为
    "最新对手价限价单"（快照 ask1/bid1），滑点可控。

风险提示：
    本桥会真实下单。请先在 quantd 配置 live.shadow = true 影子验收，
    再 --env simulate 模拟盘演练，最后才切 real + i_confirm_real_money。
"""
import argparse
import socket
import sys
import threading
import time

PROTOCOL_VERSION = "futu-bridge/1.0"

# 引擎格式 <-> 富途格式：00700.HK <-> HK.00700 / AAPL.US <-> US.AAPL


def to_futu(sym: str) -> str:
    code, _, mkt = sym.rpartition(".")
    return f"{mkt}.{code}"


def from_futu(sym: str) -> str:
    mkt, _, code = sym.partition(".")
    return f"{code}.{mkt}"


def log(msg: str) -> None:
    sys.stderr.write(f"[futu_bridge] {msg}\n")
    sys.stderr.flush()


def try_import_futu():
    try:
        from futu import (ModifyOrderOp, OpenQuoteContext,
                          OpenSecTradeContext, OrderType, RET_OK, TrdEnv,
                          TrdMarket, TrdSide)
        return (ModifyOrderOp, OpenQuoteContext, OpenSecTradeContext,
                OrderType, RET_OK, TrdEnv, TrdMarket, TrdSide)
    except ImportError:
        return None


class FutuBridge:
    def __init__(self, opend_host: str, opend_port: int, env: str,
                 acc_id: int):
        f = try_import_futu()
        if f is None:
            raise RuntimeError(
                "未安装 futu-api（pip install futu-api）。"
                "本桥拒绝启动，防止被误当作实盘通道。")
        (self.ModifyOrderOp, self.OpenQuoteContext, self.OpenSecTradeContext,
         self.OrderType, self.RET_OK, self.TrdEnv, self.TrdMarket,
         self.TrdSide) = f
        self.opend_host = opend_host
        self.opend_port = opend_port
        self.trd_env = (self.TrdEnv.SIMULATE if env == "simulate"
                        else self.TrdEnv.REAL)
        self.env_name = env
        self.acc_id = acc_id
        self.trd = None
        self.quote = None
        self.lock = threading.Lock()
        self.trade_seq = 0
        # futu_order_id -> {"oid": 引擎单号, "conn": 下单连接, "qty":, "price":, "acked":}
        self.orders = {}
        self.filled_seen = set()          # 成交去重键
        self.last_status = {}             # futu_order_id -> 已推送状态
        self.conns = set()                # 活跃引擎连接

    # ---------------- 连接券商（OpenD） ----------------
    def connect_broker(self) -> bool:
        try:
            self.quote = self.OpenQuoteContext(self.opend_host, self.opend_port)
            self.trd = self.OpenSecTradeContext(
                host=self.opend_host, port=self.opend_port)
        except Exception as e:  # noqa: BLE001
            log(f"连接 OpenD 失败: {e}（请确认 OpenD 正在运行且已扫码登录）")
            return False
        ret, acc = self.trd.accinfo(trd_env=self.trd_env)
        if ret != self.RET_OK:
            log(f"accinfo 失败: {acc}")
            return False
        if self.acc_id == 0:
            self.acc_id = int(acc["acc_id"].iloc[0])
        env_tag = "模拟盘" if self.trd_env == self.TrdEnv.SIMULATE else "实盘"
        log(f"已连接 OpenD {self.opend_host}:{self.opend_port}，"
            f"账户 {self.acc_id}（{env_tag}，{PROTOCOL_VERSION}）")
        return True

    # ---------------- 工具 ----------------
    def _push(self, conn, line: str):
        try:
            conn.sendall((line + "\n").encode("utf-8"))
        except OSError:
            log(f"推送失败: {line}")

    def _push_all(self, line: str):
        with self.lock:
            conns = list(self.conns)
        for c in conns:
            self._push(c, line)

    def _fee_est(self, symbol: str, price: float, qty: int) -> float:
        """成交费用估算（以券商结算为准，富途 T+2 出账单）。
        HK: 佣金 max(0.03%,3) + 平台费 15 + 印花 0.1% + 交易征费 ~0.0077%
        US: 佣金 max(0.0049*qty,1) + 平台费 1"""
        notional = price * qty
        if symbol.endswith(".HK"):
            return (max(notional * 3 / 10000.0, 3.0) + 15.0
                    + max(notional * 1 / 1000.0, 1.0)
                    + notional * 0.77 / 10000.0)
        return max(qty * 0.0049, 1.0) + 1.0

    def _opposite_price(self, code: str, side: str, ref: float) -> float:
        """港股对手价（MKT→限价翻译）；取不到退回引擎参考价"""
        try:
            ret, snap = self.quote.get_market_snapshot([code])
            if ret == self.RET_OK and len(snap) > 0:
                asks = list(snap["ask_price"].iloc[0])
                bids = list(snap["bid_price"].iloc[0])
                if side == "BUY" and asks and asks[0] > 0:
                    return float(asks[0])
                if side == "SELL" and bids and bids[0] > 0:
                    return float(bids[0])
        except Exception as e:  # noqa: BLE001
            log(f"取快照失败({code}): {e}，退回参考价 {ref}")
        return ref

    # ---------------- 对账 ----------------
    def query_asset(self, conn) -> None:
        ret, acc = self.trd.accinfo(trd_env=self.trd_env, acc_id=self.acc_id)
        if ret == self.RET_OK and len(acc) > 0:
            row = acc.iloc[0]
            ccy = str(row.get("currency", "HKD"))
            if ccy not in ("HKD", "USD", "CNY"):
                ccy = "HKD"
            self._push(conn, f"cash ccy={ccy} amount={float(row['cash']):.2f}")
        # 持仓：港美市场分别查，失败容忍（某市场未开通则跳过）
        for tm in (self.TrdMarket.HK, self.TrdMarket.US):
            try:
                ret, pos = self.trd.position_list_query(
                    code="", trd_env=self.trd_env, acc_id=self.acc_id,
                    trd_market=tm)
                if ret != self.RET_OK or pos is None or len(pos) == 0:
                    continue
                for _, p in pos.iterrows():
                    qty = int(p["qty"])
                    if qty <= 0:
                        continue
                    self._push(conn,
                               f"pos symbol={from_futu(p['code'])} qty={qty}"
                               f" available={int(p['can_sell_qty'])}"
                               f" avg={float(p['cost_price']):.4f}")
            except Exception as e:  # noqa: BLE001
                log(f"查询持仓失败({tm}): {e}")

    # ---------------- 下单 ----------------
    def place_order(self, conn, kv: dict) -> None:
        oid = kv["id"]
        symbol = kv["symbol"]              # 引擎格式 00700.HK / AAPL.US
        code = to_futu(symbol)
        side = kv["side"]
        qty = int(kv["qty"])
        otype = kv.get("type", "MKT")
        price = float(kv.get("price", "0") or 0)
        ref = float(kv.get("ref", "0") or 0)

        trd_side = (self.TrdSide.BUY if side == "BUY" else self.TrdSide.SELL)
        trd_market = (self.TrdMarket.US if code.startswith("US.")
                      else self.TrdMarket.HK)
        if otype == "LMT" and price > 0:
            order_type, px = self.OrderType.LIMIT, price
        elif code.startswith("US."):
            order_type, px = self.OrderType.MARKET, 0.0
        else:
            # 港股无统一市价单 → 对手价限价单
            px = self._opposite_price(code, side, ref)
            if px <= 0:
                self._push(conn, f"ack id={oid} status=rejected reason=无参考价")
                return
            order_type = self.OrderType.LIMIT

        ret, data = self.trd.place_order(
            price=px, qty=qty, code=code, trd_side=trd_side,
            order_type=order_type, trd_env=self.trd_env,
            acc_id=self.acc_id, trd_market=trd_market)
        if ret != self.RET_OK:
            reason = str(data).replace(" ", "_")[:80]
            self._push(conn, f"ack id={oid} status=rejected reason={reason}")
            return
        fid = str(data["order_id"].iloc[0])
        with self.lock:
            self.orders[fid] = {"oid": oid, "conn": conn, "qty": qty,
                                "price": px, "acked": False}
        log(f"已下委托 引擎#{oid} 富途[{fid}] {side} {code} x{qty} @{'市价' if px == 0 else f'{px:.4f}'}")

    def cancel_order(self, conn, kv: dict) -> None:
        oid = kv["id"]
        with self.lock:
            fid = next((k for k, v in self.orders.items() if v["oid"] == oid),
                       None)
            info = self.orders.get(fid)
        if fid is None or info is None:
            return
        self.trd.modify_order(self.ModifyOrderOp.CANCEL, fid,
                              info["qty"], info["price"],
                              trd_env=self.trd_env, acc_id=self.acc_id)

    # ---------------- 回报轮询（不依赖 OpenD 推送权限） ----------------
    def poll_loop(self) -> None:
        while True:
            time.sleep(2)
            try:
                self._poll_orders()
                self._poll_fills()
            except Exception as e:  # noqa: BLE001
                log(f"轮询异常: {e}")

    def _poll_orders(self) -> None:
        ret, orders = self.trd.list_today_orders(
            trd_env=self.trd_env, acc_id=self.acc_id)
        if ret != self.RET_OK or orders is None or len(orders) == 0:
            return
        for _, o in orders.iterrows():
            fid = str(o["order_id"])
            with self.lock:
                info = self.orders.get(fid)
            if info is None or info["conn"] is None:
                continue          # 非本桥下的单（人工/其他终端）不处理
            st = str(o["order_status"])
            status = ("rejected" if st in ("FAILED", "DISABLED")
                      else "cancelled" if st.startswith("CANCELLED")
                      else "accepted")
            with self.lock:
                if self.last_status.get(fid) == status:
                    continue
                self.last_status[fid] = status
            self._push(info["conn"], f"ack id={info['oid']} status={status}")

    def _poll_fills(self) -> None:
        ret, fills = self.trd.list_today_fills(
            trd_env=self.trd_env, acc_id=self.acc_id)
        if ret != self.RET_OK or fills is None or len(fills) == 0:
            return
        for _, f in fills.iterrows():
            fid = str(f["order_id"])
            key = (fid, str(f.get("create_time", "")),
                   float(f["price"]), int(f["qty"]))
            with self.lock:
                info = self.orders.get(fid)
                if info is None or info["conn"] is None:
                    continue
                if key in self.filled_seen:
                    continue
                self.filled_seen.add(key)
                self.trade_seq += 1
                seq = self.trade_seq
                oid = info["oid"]
                conn = info["conn"]
                symbol = from_futu(f["code"])
                price = float(f["price"])
                qty = int(f["qty"])
            fee = self._fee_est(symbol, price, qty)
            self._push(conn,
                       f"trade id={seq} order_id={oid} symbol={symbol}"
                       f" price={price:.4f} qty={qty} fee={fee:.2f}")
            log(f"成交回报 引擎#{oid} {symbol} x{qty} @ {price:.4f}")

    # ---------------- 引擎连接处理 ----------------
    def handle_line(self, conn, line: str) -> bool:
        """返回 False 表示该连接应关闭。"""
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
            self.query_asset(conn)
            self._push(conn, "log msg=对账完成(futu)")
        elif op == "order":
            self.place_order(conn, kv)
        elif op == "cancel":
            self.cancel_order(conn, kv)
        elif op == "shutdown":
            log("收到 shutdown，退出")
            self._push_all("log msg=桥已退出")
            return False
        else:
            log(f"未知 op: {op}")
        return True


def main() -> int:
    ap = argparse.ArgumentParser(description="quantd Futu 实盘桥")
    ap.add_argument("--port", type=int, default=17002)
    ap.add_argument("--opend-host", default="127.0.0.1")
    ap.add_argument("--opend-port", type=int, default=11111)
    ap.add_argument("--env", choices=["real", "simulate"], default="real",
                    help="real=实盘 simulate=富途模拟盘")
    ap.add_argument("--acc-id", type=int, default=0)
    args = ap.parse_args()

    try:
        bridge = FutuBridge(args.opend_host, args.opend_port, args.env,
                            args.acc_id)
    except RuntimeError as e:
        log(str(e))
        return 3
    if not bridge.connect_broker():
        return 3
    if args.env == "simulate":
        log(">>> 当前为富途【模拟盘】模式：行情真实、成交模拟、不动真钱 <<<")

    threading.Thread(target=bridge.poll_loop, daemon=True).start()

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", args.port))
    srv.listen(4)
    log(f"桥就绪，等待引擎连接 127.0.0.1:{args.port}")

    def serve_conn(conn):
        with bridge.lock:
            bridge.conns.add(conn)
        bridge._push(conn, f"log msg={PROTOCOL_VERSION} 已连接券商")
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
                    if text and not bridge.handle_line(conn, text):
                        return
        except OSError:
            pass
        finally:
            with bridge.lock:
                bridge.conns.discard(conn)
                for info in bridge.orders.values():
                    if info["conn"] is conn:
                        info["conn"] = None   # 连接断开，回报转为丢弃
            conn.close()

    try:
        while True:
            conn, _ = srv.accept()
            log("引擎已连接")
            threading.Thread(target=serve_conn, args=(conn,),
                             daemon=True).start()
    except KeyboardInterrupt:
        log("手动停止")
    finally:
        srv.close()
        time.sleep(0.2)
    return 0


if __name__ == "__main__":
    sys.exit(main())
