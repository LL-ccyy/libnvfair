#include <gtest/gtest.h>

#include <cstdint>

#include "lib/config/config.h"

namespace {

using nvfair::parse_memory_from_env;

// 每条断言独立作用域,out 变量互不污染
#define EXPECT_OK(str, expected)                \
    do {                                        \
        unsigned long long out = 12345;         \
        ASSERT_TRUE(parse_memory_from_env(str, &out)) << str; \
        EXPECT_EQ(out, (expected)) << str;      \
    } while (0)

#define EXPECT_BAD(str)                         \
    do {                                        \
        unsigned long long out = 12345;         \
        EXPECT_FALSE(parse_memory_from_env(str, &out)) << str; \
        EXPECT_EQ(out, 12345ULL) << "失败时不得写 out: " << str; \
    } while (0)

// ---------- 入参边界 ----------
TEST(ParseMemoryFromEnv, NullArgs) {
    unsigned long long out = 0;
    EXPECT_FALSE(parse_memory_from_env(nullptr, &out));
    EXPECT_FALSE(parse_memory_from_env("1g", nullptr));
}

TEST(ParseMemoryFromEnv, Bytes) {                // 无单位 = 字节
    EXPECT_OK("0", 0);
    EXPECT_OK("1073741824", 1073741824ULL);
    EXPECT_OK("18446744073709551615", 0xFFFFFFFFFFFFFFFFULL);  // UINT64_MAX 本身合法(不乘倍率)
}

// ---------- 1000 系(SI) ----------
TEST(ParseMemoryFromEnv, DecimalSuffix) {
    EXPECT_OK("1k", 1000ULL);
    EXPECT_OK("1K", 1000ULL);
    EXPECT_OK("1m", 1000000ULL);
    EXPECT_OK("1M", 1000000ULL);
    EXPECT_OK("1g", 1000000000ULL);
    EXPECT_OK("1G", 1000000000ULL);
    EXPECT_OK("24g", 24000000000ULL);
    EXPECT_OK("512m", 512000000ULL);
}

// ---------- 单位字母 + b/B 尾缀(仍是 1000 系) ----------
TEST(ParseMemoryFromEnv, DecimalWithB) {
    EXPECT_OK("1kb", 1000ULL);
    EXPECT_OK("1kB", 1000ULL);
    EXPECT_OK("512mb", 512000000ULL);
    EXPECT_OK("2GB", 2000000000ULL);
    EXPECT_OK("1gb", 1000000000ULL);
}

// ---------- 1024 系:单位后缀带 i ----------
TEST(ParseMemoryFromEnv, BinarySuffix) {
    EXPECT_OK("1ki", 1ULL << 10);
    EXPECT_OK("1kib", 1ULL << 10);
    EXPECT_OK("512mi", 1ULL << 29);
    EXPECT_OK("512MIB", 1ULL << 29);
    EXPECT_OK("1gi", 1ULL << 30);
    EXPECT_OK("24gib", 24ULL << 30);
    EXPECT_OK("1GIB", 1ULL << 30);
    EXPECT_OK("1kib", 1024ULL);
}

// ---------- 首尾空白 ----------
TEST(ParseMemoryFromEnv, Whitespace) {
    EXPECT_OK(" 512mi ", 1ULL << 29);
    EXPECT_OK("\t1g\n", 1000000000ULL);
    EXPECT_OK("\r\n 2g \r\n", 2000000000ULL);
}

// ---------- 大数值边界(逐单位一行,贴近溢出线) ----------
TEST(ParseMemoryFromEnv, NearOverflow) {
    EXPECT_OK("18446744073g", 18446744073ULL * 1000000000ULL);  // 1000 系最大合法
    EXPECT_BAD("18446744074g");                                  // 再大 1 就溢出
    EXPECT_OK("17179869183gi", 17179869183ULL << 30);            // 1024 系最大合法
    EXPECT_BAD("17179869184gi");
    EXPECT_OK("18446744073709551615", 0xFFFFFFFFFFFFFFFFULL);
    EXPECT_BAD("18446744073709551616");                    // strtoull 置 ERANGE
}

// ---------- 非法输入矩阵 ----------
TEST(ParseMemoryFromEnv, Rejects) {
    // 空与纯空白
    EXPECT_BAD("");
    EXPECT_BAD("   ");
    // 非数字开头
    EXPECT_BAD("abc");
    EXPECT_BAD("g");
    EXPECT_BAD("-5");                            // strtoull 静默吃负号,必须前置 isdigit 挡住
    EXPECT_BAD("+5");                            // strtoull 同样会吃加号
    // 小数不支持
    EXPECT_BAD("1.5g");
    EXPECT_BAD("0.5gib");
    // 杂尾
    EXPECT_BAD("1x");
    EXPECT_BAD("1t");                            // 大单位不支持
    EXPECT_BAD("1p");
    EXPECT_BAD("1 g");                           // 中间空白
    EXPECT_BAD("1\tg");
    // i / b 语法位置错误
    EXPECT_BAD("1i");                            // i 前必须有单位字母
    EXPECT_BAD("1b");                            // 单独 b 与"无单位即字节"歧义
    EXPECT_BAD("1bi");                           // 顺序颠倒
    EXPECT_BAD("1ib");                           // i 必须跟在单位字母后
    // 单位后杂尾
    EXPECT_BAD("1gx");
    EXPECT_BAD("1gig");
    EXPECT_BAD("1gix");
    // 数字后多段
    EXPECT_BAD("1g1g");
    EXPECT_BAD("12 34");
}

#undef EXPECT_OK
#undef EXPECT_BAD

}
