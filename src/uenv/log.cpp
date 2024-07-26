#include <memory>

#include "spdlog/sinks/stdout_color_sinks.h"
#include "spdlog/sinks/syslog_sink.h"
#include <spdlog/spdlog.h>

#include "log.h"

namespace uenv {
void init_log(spdlog::level::level_enum console_log_level,
              spdlog::level::level_enum syslog_log_level) {
    auto console_sink = std::make_shared<spdlog::sinks::stderr_color_sink_st>();
    console_sink->set_level(console_log_level);

    auto syslog_sink = std::make_shared<spdlog::sinks::syslog_sink_st>(
        "uenv", LOG_PID, 0, false);
    syslog_sink->set_level(syslog_log_level);

    // The default logger is a combined logger that dispatches to the console
    // and syslog. We explicitly set the level to trace to allow all messages to
    // pass through the combined logger. The lower level loggers will filter
    // messages at their level.
    spdlog::set_default_logger(std::make_shared<spdlog::logger>(
        "uenv", spdlog::sinks_init_list({console_sink, syslog_sink})));
    spdlog::set_level(spdlog::level::trace);
    spdlog::set_pattern("[%Y-%m-%d %H:%M:%S.%e] [%n] [%^%l%$] %v");
}
} // namespace uenv
