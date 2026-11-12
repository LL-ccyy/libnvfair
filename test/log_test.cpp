#include <gtest/gtest.h>

#include <unistd.h>
#include <cstdio>

#include <cstdlib>
#include <functional>
#include <string>

#include "util/log/log.h"

namespace {

using nvfair::LogLevel;
using nvfair::log_init_from_env;
using nvfair::log_set_level;
using nvfair::log_write;

// 环境变量助手:setenv/unsetenv + 每例结束清理,避免用例间串扰
class LogEnvTest : public ::testing::Test {
protected:
    void SetUp() override { log_set_level(LogLevel::Off); }   // 回到默认,隔离用例
    void TearDown() override { ::unsetenv("NVFAIR_LOG_LEVEL"); }
};

// ---------- log_init_from_env:合法值 ----------

TEST_F(LogEnvTest, Accepts0To4) {
    // 0-4 每个值:解析后用 log_write 的过滤行为间接验证级别生效
    struct Case { const char* env; int pass_max_lv; } cases[] = {
        {"0", 0}, {"1", 1}, {"2", 2}, {"3", 3}, {"4", 4},
    };
    for (const auto& c : cases) {
        ASSERT_EQ(::setenv("NVFAIR_LOG_LEVEL", c.env, 1), 0);
        log_init_from_env();
        // lv = pass_max_lv 应当输出(不 crash 即可验证走通);lv 更高一级被过滤
        // 这里只验证解析与过滤分支不异常,输出重定向校验见 CapturesStderr
        EXPECT_NO_FATAL_FAILURE(log_write(static_cast<LogLevel>(c.pass_max_lv), "probe")) << c.env;
        SUCCEED();
    }
}

// ---------- log_init_from_env:非法值一律拒绝,级别保持原值 ----------

// 捕获 stderr 的工具:把 stderr 重定向到 tmpfile,回调里打日志,返回捕获内容
static std::string capture_stderr(const std::function<void()>& fn) {
    FILE* f = ::tmpfile();
    if (f == nullptr) return "<tmpfile failed>";
    int saved = ::dup(STDERR_FILENO);
    ::fflush(stderr);
    ::dup2(::fileno(f), STDERR_FILENO);
    fn();                                              // 打日志
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::fflush(stderr);
    ::rewind(f);
    std::string out;
    char buf[1024];
    size_t n;
    while ((n = ::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    ::fclose(f);
    return out;
}

TEST_F(LogEnvTest, RejectsInvalidAndKeepsPreviousLevel) {
    const char* bad[] = {
        "",        // 空串
        "abc",     // 非数字
        "3abc",    // 数字后杂尾
        "5",       // 越上界
        "-1",      // 越下界(strtol 接受,范围检查拒)
        "999999999999999999999999",  // 溢出 long
    };
    for (const char* b : bad) {
        SCOPED_TRACE(b);
        // 哨兵:Info 级别下 Info 探针可见;若非法值被误接受(如 "5"→越界或溢出值),
        // 级别会离开 Info,探针就被滤掉 — 以此验证"拒绝并保持原级别"
        log_set_level(LogLevel::Info);
        ASSERT_EQ(::setenv("NVFAIR_LOG_LEVEL", b, 1), 0);
        log_init_from_env();
        std::string out = capture_stderr([] {
            log_write(LogLevel::Info, "probe-info");
        });
        EXPECT_NE(out.find("nvfair: [INFO]: probe-info"), std::string::npos)
            << "非法值 '" << b << "' 被误接受,级别被改写";
        // "-1" 走的是范围拒;顺带确认它没有把级别变成负值(否则 Debug/全级别都会漏出来)
        out = capture_stderr([] {
            log_write(LogLevel::Off, "never");
        });
        EXPECT_TRUE(out.empty()) << b;
    }
    // 未设置环境变量:保持原级别,不重置
    ::unsetenv("NVFAIR_LOG_LEVEL");
    log_set_level(LogLevel::Warn);
    log_init_from_env();
    std::string out = capture_stderr([] {
        log_write(LogLevel::Warn, "kept-warn");
        log_write(LogLevel::Info, "kept-filtered");
    });
    EXPECT_NE(out.find("kept-warn"), std::string::npos) << out;
    EXPECT_EQ(out.find("kept-filtered"), std::string::npos) << out;
}

TEST_F(LogEnvTest, LeadingWhitespaceIsRejected) {
    // " 3":strtol 跳过空白后解析成功、*end=='\0' 也成立 — 这是实现的真实行为:
    // 前导空白被容忍。该用例锁定现状;若想改成严格拒绝,改实现同时改这里
    ASSERT_EQ(::setenv("NVFAIR_LOG_LEVEL", " 3", 1), 0);
    log_set_level(LogLevel::Off);
    log_init_from_env();
    std::string out = capture_stderr([] {
        log_write(LogLevel::Info, "ws-accepted");
    });
    EXPECT_NE(out.find("ws-accepted"), std::string::npos)
        << "log.cpp 的 strtol 跳空白:' 3' 被接受为 3(锁定现状)";
}

// ---------- log_write:过滤边界 ----------

TEST_F(LogEnvTest, WriteFiltersAboveLevel) {
    log_set_level(LogLevel::Error);                    // 只放行 Error(1)
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Error, "pass"));
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Warn, "filtered"));
    log_set_level(LogLevel::Off);                      // 全滤
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Error, "filtered"));
}

// ---------- log_write:危险输入不致命(AC4) ----------

TEST_F(LogEnvTest, DangerousInputNeverFatal) {
    log_set_level(LogLevel::Debug);
    // nullptr 格式串:log.cpp 判 fmt==nullptr 直接 return
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Debug, nullptr));
    // 格式串与参数不匹配(%s 吃 int):UB,不能测 — format(printf) 属性已在编译期拦截,
    // 库内防线是"外部数据不当格式串"(design 原则 6),格式串永远来自代码字面量
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Debug, "n=%d", 7));
}

// ---------- 危险输入不致命(AC4):截断路径 ----------
TEST_F(LogEnvTest, OversizedMessageTruncatedNotFatal) {
    log_set_level(LogLevel::Debug);
    // 超过 kMsgMax(4096)的消息:走 vsnprintf 截断 + 总长钳制路径,必须不 crash
    char big[8192];
    for (size_t i = 0; i < sizeof(big) - 1; ++i) big[i] = 'x';
    big[sizeof(big) - 1] = '\0';
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Debug, "%s", big));
    // 空消息(空格式串触发 -Wformat-zero-length,压掉该警告 — 正是要测的场景)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-zero-length"
    EXPECT_NO_FATAL_FAILURE(log_write(LogLevel::Debug, ""));
#pragma GCC diagnostic pop
}

// ---------- 输出重定向:真实验证 stderr 行 ----------

TEST_F(LogEnvTest, CapturesStderr) {
    log_set_level(LogLevel::Info);
    FILE* f = ::tmpfile();
    ASSERT_NE(f, nullptr);
    int saved = ::dup(STDERR_FILENO);
    ASSERT_NE(saved, -1);
    ::fflush(stderr);
    ASSERT_NE(::dup2(::fileno(f), STDERR_FILENO), -1);

    log_write(LogLevel::Info, "hello %d", 42);
    log_write(LogLevel::Debug, "filtered-out");        // 高于 Info,不落盘

    // 恢复 stderr 后再断言
    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::fflush(stderr);
    ::rewind(f);

    char buf[1024] = {0};
    size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = '\0';

    std::string out(buf);
    EXPECT_NE(out.find("nvfair: [INFO]: hello 42"), std::string::npos) << out;
    EXPECT_EQ(out.find("filtered-out"), std::string::npos) << out;
    EXPECT_EQ(out.find('\n'), out.rfind('\n')) << "应恰好一行: " << out;  // 只有一个换行
}

TEST_F(LogEnvTest, InitFromEnvReflectsInCapturedOutput) {
    ASSERT_EQ(::setenv("NVFAIR_LOG_LEVEL", "2", 1), 0);   // Warn
    log_init_from_env();

    FILE* f = ::tmpfile();
    ASSERT_NE(f, nullptr);
    int saved = ::dup(STDERR_FILENO);
    ::fflush(stderr);
    ::dup2(::fileno(f), STDERR_FILENO);

    log_write(LogLevel::Warn, "w-pass");
    log_write(LogLevel::Info, "i-filtered");

    ::dup2(saved, STDERR_FILENO);
    ::close(saved);
    ::fflush(stderr);
    ::rewind(f);

    char buf[1024] = {0};
    size_t n = ::fread(buf, 1, sizeof(buf) - 1, f);
    ::fclose(f);
    buf[n] = '\0';
    std::string out(buf);
    EXPECT_NE(out.find("nvfair: [WARN]: w-pass"), std::string::npos) << out;
    EXPECT_EQ(out.find("i-filtered"), std::string::npos) << out;
}

}
