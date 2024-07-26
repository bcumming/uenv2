#include <spdlog/spdlog.h>

namespace uenv {
void init_log(spdlog::level::level_enum console_log_level,
              spdlog::level::level_enum syslog_log_level);
}
