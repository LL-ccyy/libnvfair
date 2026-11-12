#include "log.h"

#include <atomic>
#include <cstdlib>
#include <cstdarg>
#include <cstdio>
#include <unistd.h>

namespace nvfair {

// 日志统一前缀
static const char* kLogPrefix = "nvfair: ";

// 单条日志消息的最大长度(含 '\0');
static constexpr size_t kMsgMax = 4096;

static std::atomic<LogLevel> log_level(LogLevel::Off);

void log_set_level(LogLevel level){
    log_level.store(level, std::memory_order_relaxed);
}

void log_init_from_env(){
    const char* env = std::getenv("NVFAIR_LOG_LEVEL");
    if(env == nullptr){
        return;
    }
    if(*env == '\0'){
        return;
    }
    char* end = nullptr;
    long v = std::strtol(env, &end, 10);
    if(end == env){
        return;
    }
    if(*end != '\0'){
        return;
    }
    if(v < 0 || v > 4){
        return;
    }

    log_level.store(static_cast<LogLevel>(v), std::memory_order_relaxed);

}

void log_write(LogLevel lv, const char* fmt, ...){
    if(static_cast<int>(lv) > static_cast<int>(log_level.load(std::memory_order_relaxed))){
        return;
    }
    const char* tag;
    switch (lv) {
        case LogLevel::Off:
            return;
        case LogLevel::Error:
            tag = "ERROR";
            break;
        case LogLevel::Warn:
            tag = "WARN";
            break;
        case LogLevel::Info:
            tag = "INFO";
            break;
        case LogLevel::Debug:
            tag = "DEBUG";
            break;
        default:
            return;
    }

    if(fmt == nullptr){
        return;
    }

    char msg[kMsgMax];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    if(n < 0){
        return;
    }
    (void)n;

    // 余量 = 前缀(8) + "[DEBUG]: "(9) + '\n' 最坏组合,另留余量凑 64 对齐
    char buf[kMsgMax + 64];
    int total = snprintf(buf, sizeof(buf), "%s[%s]: %s\n", kLogPrefix, tag, msg);
    if (total > 0) {
        if (static_cast<size_t>(total) >= sizeof(buf)) {
            total = sizeof(buf) - 1;
        }
        ssize_t ignored = ::write(STDERR_FILENO, buf, static_cast<size_t>(total));
        (void)ignored;
    }
}
}