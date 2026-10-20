// quantd — 配置加载。
// 格式：UTF-8 文本，`key = value`，`#` 开头为注释。
// 扁平键值 + 点号分节（如 strategy.fast / sim.600519.SH.vol），
// 避免引入第三方解析库，零依赖即可运行。
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace quantd {

class Config {
 public:
  // 解析文件；失败返回 std::nullopt 并输出原因。
  static std::optional<Config> load(const std::string& path);

  bool has(const std::string& key) const { return kv_.count(key) > 0; }

  std::string str(const std::string& key,
                  const std::string& def = "") const;
  double num(const std::string& key, double def = 0.0) const;
  int64_t integer(const std::string& key, int64_t def = 0) const;
  bool flag(const std::string& key, bool def = false) const;

  // 逗号分隔列表 → 去空格后的元素
  std::vector<std::string> list(const std::string& key) const;

 private:
  std::map<std::string, std::string> kv_;
};

}  // namespace quantd
