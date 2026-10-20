#!/usr/bin/env python3
"""
QMT(迅投) A股实盘桥 — 骨架。

quantd(C++) 与本桥通过 stdin/stdout 的 JSON 行协议通信（每行一个
JSON 对象），桥负责调用 xtquant 与 QMT 客户端交互。

装配步骤：
  1. 在券商开通 QMT 极简/量化权限，安装并登录 QMT 客户端
  2. pip install xtquant
  3. 将 config/quantd.conf 的 gateway.cn 改为 qmt，
     gateway.qmt.bridge 指向本脚本
  4. quantd 会以子进程方式拉起本桥，connect() 阶段完成握手

协议（草案）：
  -> {"op":"ping"}                          心跳
  <- {"op":"pong"}
  -> {"op":"order","id":1,"symbol":"600519.SH","side":"BUY",
       "type":"MKT","qty":100,"limit":0.0}   下单
  <- {"op":"order_ack","id":1,"xt_id":"..."}
  -> {"op":"cancel","xt_id":"..."}           撤单
  <- {"op":"trade","id":1,"xt_id":"...","price":1450.0,"qty":100}
  <- {"op":"position","symbol":"600519.SH","qty":100,"available":100}

本文件当前为可运行的占位骨架：未检测到 xtquant 时会明确报错退出，
防止被误当作实盘通道。
"""
import json
import sys


def log(msg: str) -> None:
    sys.stderr.write(f"[qmt_bridge] {msg}\n")
    sys.stderr.flush()


def main() -> int:
    try:
        from xtquant import xttrader  # noqa: F401
        from xtquant.xttype import StockAccount  # noqa: F401
    except ImportError:
        log("未安装 xtquant（需 QMT 客户端环境）。本桥拒绝启动，"
            "详见文件头部装配步骤。")
        return 3

    log("xtquant 已加载。TODO: 实现握手/下单/撤单/持仓查询循环。")
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            req = json.loads(line)
        except json.JSONDecodeError:
            log(f"非法 JSON: {line[:100]}")
            continue
        op = req.get("op")
        if op == "ping":
            sys.stdout.write(json.dumps({"op": "pong"}) + "\n")
            sys.stdout.flush()
        else:
            log(f"TODO 处理: {op}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
