// quantd — 程序入口。
// 用法: quantd [config路径]  （默认 config/quantd.conf）
#include <csignal>
#include <filesystem>
#include <iostream>

#include "core/config.hpp"
#include "core/log.hpp"
#include "engine/engine.hpp"

int main(int argc, char** argv) {
  using namespace quantd;

  const std::string cfg_path =
      argc > 1 ? argv[1] : "config/quantd.conf";

  if (!std::filesystem::exists(cfg_path)) {
    QD_ERROR("配置文件不存在: {}（用 `quantd <path>` 指定）", cfg_path);
    return 2;
  }

  auto cfg = Config::load(cfg_path);
  if (!cfg) return 2;

  if (cfg->flag("log.debug", false)) g_log_level = LogLevel::Debug;

  Engine engine(std::move(*cfg));
  if (!engine.init()) {
    QD_ERROR("引擎初始化失败");
    return 1;
  }
  return engine.run();
}
