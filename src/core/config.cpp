#include "core/config.hpp"

#include <fstream>
#include <sstream>

#include "core/log.hpp"

namespace quantd {

namespace {

std::string trim(const std::string& s) {
  size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

}  // namespace

std::optional<Config> Config::load(const std::string& path) {
  std::ifstream in(path);
  if (!in) {
    QD_ERROR("无法打开配置文件: {}", path);
    return std::nullopt;
  }

  Config cfg;
  std::string line;
  int lineno = 0;
  while (std::getline(in, line)) {
    ++lineno;
    // 去掉行内注释
    size_t pos = line.find('#');
    if (pos != std::string::npos) line = line.substr(0, pos);
    line = trim(line);
    if (line.empty()) continue;

    size_t eq = line.find('=');
    if (eq == std::string::npos) {
      QD_WARN("配置第{}行缺少 '='，已跳过", lineno);
      continue;
    }
    std::string key = trim(line.substr(0, eq));
    std::string val = trim(line.substr(eq + 1));
    if (key.empty()) continue;
    cfg.kv_[key] = val;
  }
  return cfg;
}

std::string Config::str(const std::string& key, const std::string& def) const {
  auto it = kv_.find(key);
  return it == kv_.end() ? def : it->second;
}

double Config::num(const std::string& key, double def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  try {
    return std::stod(it->second);
  } catch (...) {
    QD_WARN("配置 {}={} 不是合法数字，使用默认值 {}", key, it->second, def);
    return def;
  }
}

int64_t Config::integer(const std::string& key, int64_t def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  try {
    return std::stoll(it->second);
  } catch (...) {
    QD_WARN("配置 {}={} 不是合法整数，使用默认值 {}", key, it->second, def);
    return def;
  }
}

bool Config::flag(const std::string& key, bool def) const {
  auto it = kv_.find(key);
  if (it == kv_.end()) return def;
  const std::string& v = it->second;
  return v == "1" || v == "true" || v == "yes" || v == "on";
}

std::vector<std::string> Config::list(const std::string& key) const {
  std::vector<std::string> out;
  std::stringstream ss(str(key));
  std::string item;
  while (std::getline(ss, item, ',')) {
    item = trim(item);
    if (!item.empty()) out.push_back(item);
  }
  return out;
}

}  // namespace quantd
