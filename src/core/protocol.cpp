#include "core/protocol.hpp"

#include <cmath>
#include <format>
#include <sstream>

namespace quantd {

std::map<std::string, std::string> parse_line(const std::string& line) {
  std::map<std::string, std::string> out;
  std::istringstream ss(line);
  std::string tok;
  if (!(ss >> tok)) return out;          // 空行 → 空 op
  out["op"] = tok;
  while (ss >> tok) {
    size_t eq = tok.find('=');
    if (eq == std::string::npos) continue;  // 容忍未知裸 token
    out[tok.substr(0, eq)] = tok.substr(eq + 1);
  }
  return out;
}

std::string make_line(std::string_view op,
                      const std::vector<std::pair<std::string, std::string>>& fields) {
  std::string s{op};
  for (const auto& [k, v] : fields) {
    s += ' ';
    s += k;
    s += '=';
    s += v;
  }
  return s;
}

std::string fmt_amount(double v) {
  if (std::fabs(v) < 5e-5) return "0";
  for (int p = 1; p <= 4; ++p) {
    std::string s = std::format("{:.{}f}", v, p);
    if (std::stod(s) == v) return s;
  }
  return std::format("{:.4f}", v);
}

}  // namespace quantd
