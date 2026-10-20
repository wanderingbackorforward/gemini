# quantd — 多市场量化交易系统 (C++20)

事件驱动的多市场量化交易框架。全链路：行情 → 策略 → 风控 → 网关 →
组合 → 报告；A股（QMT）与港美股（富途 OpenD）**实盘通道已完整实现**，
美股预留。实盘安全内置三道防线：影子模式、单笔硬上限、当日亏损熔断。

## 快速开始

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
./build/quantd_tests        # 单元测试（52 项）
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
| `src/risk` | 事前风控：限额/频率/价格偏离/涨跌停/可卖数量/在途占用 |
| `src/gateway` | 交易网关：Paper 模拟撮合 + QMT/Futu 实盘（TCP 桥架构） |
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

## 实盘接入（部署指南）

> ⚠️ 实盘前务必：券商 API 权限开通、合规确认、**小资金灰度验证**。
> 本系统所有实盘通道都经过三层开闸验证（见下），不要跳级。

### 实盘安全模型（已内置，无需配置）

| 防线 | 机制 | 触发条件 |
|---|---|---|
| 1. 影子模式 | 委托被拦截不发券商，通道/对账/回报全真实执行 | `live.i_confirm_real_money` 非 true 时**强制**生效 |
| 2. 单笔硬上限 | 超过 `live.max_order_value_cny` 的委托直接拒绝 | 每笔委托 |
| 3. 当日亏损熔断 | 当日权益回撤超过 `live.daily_loss_limit_cny` 停止新开仓 | 每笔委托前 |
| 4. 在途占用 | 券商回报延迟窗口内，同标的同方向不可重复委托 | 引擎自动 |

### 第一级：影子验收（零成本）

连真实券商环境，但所有委托被拦截。验证：连接、对账（券商现金/
持仓真实拉取）、行情、信号生成、风控。

```bash
# A股（需 QMT 环境）：配置 gateway.cn = qmt，live.shadow = true
python scripts/live/qmt_bridge.py --port 17001 --account <资金账号> --qmt-path "<QMT目录>"
./build/quantd config/quantd_live_mock.conf   # 影子模式跑一遍

# 港美股（需 OpenD 环境）：配置 gateway.hk = futu
python scripts/live/futu_bridge.py --port 17002
./build/quantd config/quantd_live_mock.conf
```

**无券商环境**也可以全链路演练（模拟券商）：

```bash
python3 scripts/live/mock_bridge.py --port 17001      # 模拟 A股券商
python3 scripts/live/selftest_futu_bridge.py          # futu 桥协议自测(8项)
```

### 第二级：富途模拟盘（真行情、模拟成交）

富途 OpenD 提供模拟交易环境，行情真实、成交模拟、不动真钱：

```bash
python scripts/live/futu_bridge.py --port 17002 --env simulate
# quantd 配置: live.i_confirm_real_money = true, live.shadow = false
```

### 第三级：真实委托（动真钱）

1. `live.i_confirm_real_money = true`（显式确认）
2. `live.shadow = false`
3. `live.max_order_value_cny` 从小值起步（如 5000）
4. `live.daily_loss_limit_cny` 设置当日止损线

**A股 — QMT（迅投）**

QMT 只有 Windows 客户端，建议部署在 Windows 云服务器：

1. 券商开通 QMT 量化权限（资金门槛以券商为准）
2. 安装 QMT 客户端并登录，开启**极简模式（miniQMT）**
3. `pip install xtquant`
4. 运行桥：`python scripts/live/qmt_bridge.py --port 17001 --account <资金账号> --qmt-path "<QMT安装目录>"`
5. quantd 配置 `gateway.cn = qmt` + `gateway.qmt.port = 17001`

**港美股 — 富途 OpenD**（个人无量化门槛，最易落地）

1. 富途牛牛开户，官网开通 OpenAPI 权限
2. 下载运行 OpenD 网关（默认 `127.0.0.1:11111`）
3. `pip install futu-api pandas`
4. 运行桥：`python scripts/live/futu_bridge.py --port 17002`
5. quantd 配置 `gateway.hk = futu` + `gateway.futu.port = 17002`
   （美股：`market.us.enable = true`，与港股共用桥）

桥支持多引擎连接（港+美网关共用一个桥进程），委托回报按下单
连接路由。代码映射：`00700.HK ↔ HK.00700`、`AAPL.US ↔ US.AAPL`。

**港美股 — 长桥 LongPort**
- 官方提供 C SDK，可直接编入本工程实现 `LongportGateway`（接口已定义）

境内接入美股前请确认券商资质与外汇额度等合规要求。

### 行协议（引擎 ↔ 桥）

TCP + 按行文本（肉眼可读、telnet 可调试），定义见
`src/core/protocol.hpp`：

```
引擎→桥:  ping | order id=... symbol=... side=BUY|SELL type=MKT|LMT
          qty=... price=... ref=... | cancel id=... | query_asset
桥→引擎:  pong | ack id=... status=accepted|rejected reason=...
          trade id=... order_id=... symbol=... price=... qty=... fee=...
          pos symbol=... qty=... available=... avg=... | cash ccy=... amount=...
```

## 当前状态与路线

- [x] 事件驱动引擎 + 纸面撮合（A股/港股）
- [x] 多币种账户、FIFO 盈亏、T+1、整手、费用模型
- [x] 事前风控（含在途占用）+ 52 项单元测试
- [x] QMT（A股）实盘桥完整实现 + mock 全链路验证
- [x] Futu（港美）实盘桥完整实现 + 假环境全链路验证
- [x] 实盘三层安全：影子模式 / 单笔硬上限 / 当日亏损熔断
- [ ] 历史行情 CSV 回放源
- [ ] 实时行情源接入（当前 sim_feed 驱动，实盘需替换为券商推送）
- [ ] 事中风控（实时持仓监控）与自动换汇
- [ ] tick 级数据与更高频策略

## 免责声明

本项目用于量化交易技术研究。示例策略（双均线）为教学基线，不构成
任何投资建议；接入实盘产生的盈亏由使用者自行承担。
