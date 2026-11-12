#pragma once

namespace nvfair {

enum class LogLevel : int {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4
};

void log_init_from_env();
void log_set_level(LogLevel level);
void log_write(LogLevel lv, const char* fmt, ...)
__attribute__((format(printf, 2, 3)));
}