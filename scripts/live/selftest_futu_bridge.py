#!/usr/bin/env python3
"""
futu_bridge.py 自测 — 无需富途环境/真实 OpenD。

注入一个假 futu 模块（模拟账户：150,000 HKD + 00700.HK x200 均价350，
下单后 0.8 秒自动成交 @对手价），启动 FutuBridge 服务线程，
再用 socket 扮演 quantd 引擎，走完整协议：

  ping → query_asset(现金/持仓对账) → order(MKT 港股对手价翻译)
  → ack accepted → trade 成交回报 → 拒单路径 → 撤单路径

用法： python3 scripts/live/selftest_futu_bridge.py
"""
import socket
import sys
import threading
import time
import types

PORT = 17099

passed, failed = [], []


def check(name: str, ok: bool, detail: str = ""):
    (passed if ok else failed).append(name)
    print(f"  [{' ok ' if ok else 'FAIL'}] {name}{(' — ' + detail) if detail else ''}")


# ---------------- 假 futu SDK ----------------
def make_fake_futu():
    import pandas as pd

    class _NS:
        BUY, SELL = "BUY", "SELL"
    TrdSide = _NS()

    class _OT:
        LIMIT, MARKET = "LIMIT", "MARKET"
    OrderType = _OT()

    class _TM:
        HK, US = "HK", "US"
    TrdMarket = _TM()

    class _TE:
        REAL, SIMULATE = "REAL", "SIMULATE"
    TrdEnv = _TE()

    class _MO:
        CANCEL = "CANCEL"
    ModifyOrderOp = _MO()

    RET_OK = 0

    class FakeTrd:
        """模拟富途交易接口：港股对手价成交，支持拒单/撤单"""
        def __init__(self):
            self.orders = {}
            self.fills = []
            self.seq = 1000
            self.reject_next = False
            self.auto_fill = True
            self.lock = threading.Lock()

        def accinfo(self, trd_env=None, acc_id=0):
            if acc_id in (0, 888001):
                return RET_OK, pd.DataFrame([{
                    "acc_id": 888001, "cash": 150000.0,
                    "currency": "HKD"}])
            return -1, "账号不存在"

        def _fill(self, fid):
            with self.lock:
                o = self.orders[fid]
                o["order_status"] = "FILLED_ALL"
                self.fills.append({
                    "order_id": fid, "code": o["code"],
                    "price": o["price"], "qty": o["qty"],
                    "create_time": "2026-10-20 10:00:00"})

        def place_order(self, price, qty, code, trd_side, order_type,
                        trd_env, acc_id, trd_market):
            if self.reject_next:
                self.reject_next = False
                return -1, "Mock 拒单: 资金不足"
            self.seq += 1
            fid = f"F{self.seq}"
            with self.lock:
                self.orders[fid] = {
                    "order_id": fid, "code": code, "trd_side": trd_side,
                    "order_status": "SUBMITTED", "qty": qty, "price": price}
            if self.auto_fill:
                threading.Timer(0.8, self._fill, args=(fid,)).start()
            return RET_OK, pd.DataFrame([{"order_id": fid}])

        def modify_order(self, op, order_id, qty, price, trd_env, acc_id):
            with self.lock:
                o = self.orders.get(order_id)
                if o and o["order_status"] == "SUBMITTED":
                    o["order_status"] = "CANCELLED_ALL"
            return RET_OK, pd.DataFrame([{"order_id": order_id}])

        def position_list_query(self, code="", trd_env=None, acc_id=0,
                                trd_market=None):
            if trd_market == TrdMarket.HK:
                return RET_OK, pd.DataFrame([{
                    "code": "HK.00700", "qty": 200, "can_sell_qty": 200,
                    "cost_price": 350.0}])
            return RET_OK, pd.DataFrame(
                columns=["code", "qty", "can_sell_qty", "cost_price"])

        def list_today_orders(self, trd_env=None, acc_id=0):
            with self.lock:
                rows = list(self.orders.values())
            return RET_OK, pd.DataFrame(rows) if rows else pd.DataFrame(
                columns=["order_id", "order_status"])

        def list_today_fills(self, trd_env=None, acc_id=0):
            with self.lock:
                rows = list(self.fills)
            return RET_OK, pd.DataFrame(rows) if rows else pd.DataFrame(
                columns=["order_id", "price", "qty"])

    class FakeQuote:
        def __init__(self, host=None, port=None):
            pass
        def get_market_snapshot(self, codes):
            return RET_OK, pd.DataFrame([{
                "code": codes[0], "ask_price": [352.0, 352.4],
                "bid_price": [351.0, 350.6]}])

    class FakeSecCtx(FakeTrd):
        """交易上下文 = 交易接口本体（桥直接调 ctx.xxx）"""
        def __init__(self, host=None, port=None):
            super().__init__()

    fake = types.ModuleType("futu")
    fake.ModifyOrderOp, fake.OrderType, fake.RET_OK = ModifyOrderOp, OrderType, RET_OK
    fake.TrdEnv, fake.TrdMarket, fake.TrdSide = TrdEnv, TrdMarket, TrdSide
    fake.OpenQuoteContext = FakeQuote
    fake.OpenSecTradeContext = FakeSecCtx
    return fake, FakeSecCtx


# ---------------- 引擎侧 socket 客户端 ----------------
class EngineClient:
    def __init__(self, port):
        self.sock = socket.create_connection(("127.0.0.1", port), timeout=10)
        self.buf = b""

    def send(self, line):
        self.sock.sendall((line + "\n").encode())

    def recv_line(self, timeout=8.0):
        self.sock.settimeout(timeout)
        while b"\n" not in self.buf:
            data = self.sock.recv(4096)
            if not data:
                raise ConnectionError("桥断开")
            self.buf += data
        line, self.buf = self.buf.split(b"\n", 1)
        return line.decode().strip()

    def recv_until(self, pred, timeout=8.0):
        """收行直到 pred(line) 为真，返回命中的行列表"""
        got = []
        deadline = time.time() + timeout
        while time.time() < deadline:
            line = self.recv_line(timeout=max(0.1, deadline - time.time()))
            got.append(line)
            if pred(line):
                return got
        return got


def main() -> int:
    sys.path.insert(0, "scripts/live")
    fake, fake_sec_cls = make_fake_futu()
    sys.modules["futu"] = fake
    import futu_bridge as fb

    # 启动桥（服务循环）
    bridge = fb.FutuBridge("127.0.0.1", 11111, "real", 0)
    if not bridge.connect_broker():
        print("connect_broker 失败")
        return 1
    threading.Thread(target=bridge.poll_loop, daemon=True).start()

    import socket as sk
    srv = sk.socket(sk.AF_INET, sk.SOCK_STREAM)
    srv.setsockopt(sk.SOL_SOCKET, sk.SO_REUSEADDR, 1)
    srv.bind(("127.0.0.1", PORT))
    srv.listen(1)

    def serve():
        conn, _ = srv.accept()
        with bridge.lock:
            bridge.conns.add(conn)
        buf = b""
        try:
            while True:
                data = conn.recv(4096)
                if not data:
                    break
                buf += data
                while b"\n" in buf:
                    line, buf = buf.split(b"\n", 1)
                    text = line.decode().strip()
                    if text and not bridge.handle_line(conn, text):
                        return
        except OSError:
            pass

    threading.Thread(target=serve, daemon=True).start()
    time.sleep(0.2)

    fake_trd = bridge.trd
    print("=== futu_bridge 协议自测（假富途环境） ===")

    # 1) 握手
    ec = EngineClient(PORT)
    ec.send("ping")
    check("握手 ping/pong", ec.recv_line() == "pong")

    # 2) 对账
    ec.send("query_asset")
    lines = ec.recv_until(lambda l: l.startswith("log msg="))
    cash = next((l for l in lines if l.startswith("cash ")), "")
    pos = next((l for l in lines if l.startswith("pos ")), "")
    check("对账现金", cash == "cash ccy=HKD amount=150000.00", cash)
    check("对账持仓(含代码映射 HK.00700→00700.HK)",
          "pos symbol=00700.HK qty=200 available=200 avg=350.0000" == pos, pos)

    # 3) 港股市价单 → 对手价限价翻译（BUY 应取 ask1=352.0）
    ec.send("order id=1 symbol=00700.HK side=BUY type=MKT qty=100 price=0 ref=350")
    ack = ec.recv_until(lambda l: l.startswith("ack id=1"))[-1]
    check("委托受理 ack", ack == "ack id=1 status=accepted", ack)
    placed_price = list(fake_trd.orders.values())[-1]["price"]
    check("MKT→对手价限价翻译(BUY取ask1=352)", placed_price == 352.0,
          f"实际委托价 {placed_price}")

    # 4) 成交回报（轮询 2s 一次，等 6s）
    trade = ec.recv_until(lambda l: l.startswith("trade "), timeout=6)[-1]
    parts = dict(p.split("=", 1) for p in trade.split()[1:])
    check("成交回报", parts.get("symbol") == "00700.HK"
          and parts.get("order_id") == "1"
          and float(parts["price"]) == 352.0
          and int(parts["qty"]) == 100
          and float(parts["fee"]) > 0, trade)

    # 5) 拒单路径
    fake_trd.reject_next = True
    ec.send("order id=2 symbol=00700.HK side=SELL type=LMT qty=100 price=380")
    ack2 = ec.recv_until(lambda l: l.startswith("ack id=2"))[-1]
    check("拒单路径", "status=rejected" in ack2, ack2)

    # 6) 撤单路径（自动成交关掉，保证单子还在挂）
    fake_trd.auto_fill = False
    ec.send("order id=3 symbol=00700.HK side=BUY type=LMT qty=100 price=300")
    ec.recv_until(lambda l: l.startswith("ack id=3"))
    ec.send("cancel id=3")
    ack3 = ec.recv_until(lambda l: "id=3 status=cancelled" in l, timeout=8)[-1]
    check("撤单路径", "id=3 status=cancelled" in ack3, ack3)

    ec.send("shutdown")

    print(f"\n===== 自测结果: {len(passed)}/{len(passed)+len(failed)} 通过 =====")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
