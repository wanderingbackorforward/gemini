# quantd — 多市场量化交易系统 (C++20)

事件驱动的多市场量化交易框架。当前以**纸面交易（模拟撮合）**模式
打通全链路：行情 → 策略 → 风控 → 网关 → 组合 → 报告；A股与港股
已启用，美股预留；实盘网关（QMT / 富途 / 长桥）骨架就绪。

## 快速开始

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/quantd_tests        # 单元测试（46 项）
./build/quantd              # 跑 60 个交易日的模拟（config/quantd.conf）
```

运行后查看 `output/`：`equity.csv`（净值曲线）、`trades.csv`（成交
明细）、`positions.csv`（期末持仓）、`summary.txt`（业绩摘要）。

## 架构

```
                 ┌──────────────────────────────────────────┐
                 │              Engine (单线程事件循环)      │
                 └──────────────────────────────────────────┘
  SimFeed ──bar──▶ EventBus ──▶ Strategy ──订单──▶ RiskManager
  (GBM模拟)         │  ▲                        │
                    │  └────── 成交/回报 ◀── Gateway (每市场一个)
                    ▼                                │
                 Portfolio ◀─────────────────────────┘
              (多币种/FIFO/T+1)
```

| 模块 | 职责 |
|---|---|
| `src/core` | 领域类型、事件总线、配置、日志 |
| `src/market` | 市场规则（T+1/整手/涨跌停/价位/费用）、交易日历 |
| `src/data` | 模拟行情源（可替换为历史回放或实时行情） |
| `src/strategy` | 策略接口 + 双均线示例（金叉买/死叉卖） |
| `src/risk` | 事前风控：限额/频率/价格偏离/涨跌停/可卖数量 |
| `src/gateway` | 交易网关：Paper 模拟撮合 + QMT/Futu/LongPort 骨架 |
| `src/portfolio` | 多币种现金、FIFO 平仓盈亏、T+1 解冻 |
| `src/engine` | 主循环装配、报告输出 |

### 设计要点

- **无前视偏差**：bar 日收盘信号 → 次日开盘价撮合（限价单按次日
  high/low 判断触及，开盘更优则按开盘成交）
- **市场规则集中**：A股（T+1、±10%、100股整手、印花税0.05%）、
  港股（T+0、每手因股票而异、简化价位表）、美股（T+0、1股）差异
  全部收敛在 `market_rules.cpp`
- **多币种分账**：CNY/HKD/USD 现金独立记账，权益按汇率折 CNY
- **回测/实盘同构**：实盘只替换行情源与网关，引擎与策略零改动

## 配置

编辑 `config/quantd.conf`（注释齐全）。要点：

- `market.*.enable` / `gateway.*`：市场开关与网关选择
  （`paper` 模拟 / `qmt` / `futu` / `longport` 实盘）
- `universe`：标的池，后缀 `.SH/.SZ/.BJ`=A股、`.HK`=港股、`.US`=美股
- `account.capital_*`：各币种初始资金（分账管理）
- `strategy.*`：策略参数（快/慢均线、单标的仓位比）
- `sim.*`：模拟行情参数（起始日、天数、各标的波动率，种子固定可复现）

## 实盘接入（路线图）

> ⚠️ 实盘前务必：券商 API 权限开通、合规确认、**小资金灰度验证**。

**A股 — QMT（迅投）**
1. 券商开通 QMT 量化权限并登录客户端
2. `pip install xtquant`
3. 补全 `scripts/live/qmt_bridge.py`（JSON 行协议骨架已就位）
4. `gateway.cn = qmt`，重启 quantd

**港美股 — 富途 OpenD**
1. 富途账户开通 OpenAPI，运行 OpenD
2. `pip install futu-api`，补全 `scripts/live/futu_bridge.py`
3. `gateway.hk = futu`（美股市场同理，`market.us.enable = true`）

**港美股 — 长桥 LongPort**
- 官方提供 C SDK，可直接编入本工程实现 `LongportGateway`（接口已定义）

境内接入美股前请确认券商资质与外汇额度等合规要求。

## 当前状态与路线

- [x] 事件驱动引擎 + 纸面撮合（A股/港股）
- [x] 多币种账户、FIFO 盈亏、T+1、整手、费用模型
- [x] 事前风控 + 46 项单元测试
- [ ] 历史行情 CSV 回放源
- [ ] QMT / Futu 实盘桥落地（骨架就绪）
- [ ] 事中风控（实时持仓监控）与自动换汇
- [ ] tick 级数据与更高频策略

## 免责声明

本项目用于量化交易技术研究。示例策略（双均线）为教学基线，不构成
任何投资建议；接入实盘产生的盈亏由使用者自行承担。
