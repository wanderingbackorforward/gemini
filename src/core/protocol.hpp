// quantd — 引擎与实盘桥之间的行协议。
//
// 协议设计（TCP + 按行文本，刻意不用 JSON：两侧零依赖、肉眼可读、
// 便于 telnet 手动排查实盘问题）：
//
//   引擎 → 桥:
//     ping
//     order id=<n> symbol=<SYM> side=BUY|SELL type=MKT|LMT qty=<n> price=<x> ref=<昨收>
//     cancel id=<n>
//     query_asset
//
//   桥 → 引擎:
//     pong
//     ack id=<n> status=accepted|rejected reason=<...>          （reason 可省略）
//     trade id=<n> order_id=<n> symbol=<SYM> price=<x> qty=<n> fee=<x>
//     pos symbol=<SYM> qty=<n> available=<n> avg=<x>            （对账快照，可多行）
//     cash ccy=<CNY|HKD|USD> amount=<x>                         （对账快照）
//     log msg=<...>                                             （桥侧日志透传）
//
// 约定：字段值不含空格；数字 price/fee 保留至多 4 位小数。
#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace quantd {

// 解析一行协议。首 token 为 op，其余按 key=value 解析。
// 非法行返回空 op。
std::map<std::string, std::string> parse_line(const std::string& line);

// 构造一行协议。例: make_line("order", {{"id","1"},{"symbol","600519.SH"}})
std::string make_line(std::string_view op,
                      const std::vector<std::pair<std::string, std::string>>& fields = {});

// 金额格式化（去尾零，至多4位小数，协议用）
std::string fmt_amount(double v);

}  // namespace quantd
