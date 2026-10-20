#!/usr/bin/env python3
"""
Futu OpenD 港美股实盘桥 — 骨架。

quantd(C++) 与本桥通过 stdin/stdout 的 JSON 行协议通信。
富途官方 SDK futu-api 为 Python 实现，接入流程：

装配步骤：
  1. 开立富途证券账户，开通 OpenAPI 权限
  2. 下载并运行 OpenD 网关程序（默认监听 127.0.0.1:11111）
  3. pip install futu-api
  4. 将 config/quantd.conf 的 gateway.hk / gateway.us 改为 futu
  5. quantd 拉起本桥并完成握手

注意事项：
  - 境内用户接入美股前请先确认券商资质、外汇额度等合规要求
  - 富途代码与 quantd 后缀映射：00700.HK -> "HK.00700"，AAPL.US -> "US.AAPL"

本文件当前为可运行的占位骨架：未检测到 futu-api 时明确报错退出。
"""
import json
import sys


def log(msg: str) -> None:
    sys.stderr.write(f"[futu_bridge] {msg}\n")
    sys.stderr.flush()


def to_futu_code(symbol: str) -> str:
    """000700.HK -> HK.00700 / AAPL.US -> US.AAPL"""
    code, _, suffix = symbol.partition(".")
    return f"{suffix}.{code}"


def main() -> int:
    try:
        import futu  # noqa: F401
    except ImportError:
        log("未安装 futu-api。请先运行 OpenD 并 pip install futu-api，"
            "详见文件头部装配步骤。")
        return 3

    log("futu-api 已加载。TODO: 实现握手/下单/撤单/持仓查询循环。")
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
