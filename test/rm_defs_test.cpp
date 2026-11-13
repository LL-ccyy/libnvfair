// PBI-003 布局锚定与偏移选择的运行时验证。
#include "rm/rm_defs.h"

#include <gtest/gtest.h>

namespace {

TEST(RmDefs, StatusOffsetSelectsNvos64Layout)
{
    EXPECT_EQ(rm_alloc_status_offset(sizeof(NVOS64_PARAMETERS)), 40);
}

TEST(RmDefs, StatusOffsetSelectsNvos21Layout)
{
    EXPECT_EQ(rm_alloc_status_offset(sizeof(NVOS21_PARAMETERS)), 28);
}

TEST(RmDefs, StatusOffsetRejectsUnknownSize)
{
    // 非两种信纸之一:必须拒识(-1),调用方降级直通,不得猜写
    EXPECT_EQ(rm_alloc_status_offset(0), -1);
    EXPECT_EQ(rm_alloc_status_offset(16), -1);   // NVOS00 是 FREE 的,不是 ALLOC 的
    EXPECT_EQ(rm_alloc_status_offset(64), -1);
}

TEST(RmDefs, RuntimeOffsetsMatchAnchoredValues)
{
    // 与 rm_defs.h 锚定区对应;若有漂移,static_assert 先炸,这里兜底确认
    EXPECT_EQ(sizeof(NVOS64_PARAMETERS), 48u);
    EXPECT_EQ(offsetof(NVOS64_PARAMETERS, status), 40u);
    EXPECT_EQ(sizeof(NVOS21_PARAMETERS), 32u);
    EXPECT_EQ(offsetof(NVOS21_PARAMETERS, status), 28u);
    EXPECT_EQ(sizeof(NVOS00_PARAMETERS), 16u);
    EXPECT_EQ(offsetof(NVOS00_PARAMETERS, status), 12u);
    EXPECT_EQ(sizeof(NVOS54_PARAMETERS), 32u);
    EXPECT_EQ(offsetof(NVOS54_PARAMETERS, status), 28u);
    EXPECT_EQ(sizeof(NVOS32_PARAMETERS), 184u);
    EXPECT_EQ(offsetof(NVOS32_PARAMETERS, data), 40u);
}

}  // namespace
