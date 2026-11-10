# NVIDIA open-gpu-kernel-modules 调研文档

> 来源:NVIDIA/open-gpu-kernel-modules @ `6dd092d`(固定提交,已下载 /tmp/ogkm)
> 对 libnvfair 的价值:**RM ioctl 接口的唯一权威定义来源**——本项目 hook libc ioctl 所要解析的全部命令码与参数结构,都在这里。

## 1. 它是什么

NVIDIA 自 R515 起开源的内核驱动部分(GSP 固件仍为闭源 blob)。对用户态劫持库而言,它最重要的身份不是"内核模块源码",而是:

- **libcuda.so 与内核之间 RM API 的协议规范**。闭源用户态库(libcuda/libnvidia-ml)发给 `/dev/nvidiactl`、`/dev/nvidia<n>`、`/dev/nvidia-uvm` 的每个 ioctl,其 request 编码、参数结构、状态码,都以此仓库头文件为准;
- ioctl-controller 当年"内嵌 open-gpu-kernel-modules 整棵源码树作头文件"、cuda_ioctl_sniffer 直接 include 它的头,都是同一用法;HAMi 的 nvml-subset.h 也是从这里摘的。

## 2. 关键目录与头文件(libnvfair 的 `rm_defs.h` 取材处)

| 路径 | 内容 | 对本项目的用途 |
|---|---|---|
| `kernel-open/common/inc/nv-ioctl.h` + `nv-ioctl-numbers.h` | ioctl request 编码宏(magic 'F'=0x46、nr、size 组合;'F' 已经 sniff.cc 对 libcuda 真实流量实证) | hook 分发时识别 NVIDIA 请求的依据 |
| `src/nvidia/arch/nvalloc/unix/include/nv_escape.h` | **27 个 `NV_ESC_*` escape 码全集**(RM_ALLOC_MEMORY=0x27、RM_FREE=0x29、RM_CONTROL=0x2A、RM_ALLOC=0x2B、RM_VID_HEAP_CONTROL=0x4A、RM_MAP_MEMORY=0x4E…) | libnvfair 拦截清单的唯一事实源,直接 include 而非手抄 |
| `src/common/sdk/nvidia/inc/nvos.h` | `NVOS32_FUNCTION_*`(ALLOC_SIZE=2、FREE=3、ALLOC_SIZE_RANGE=14…)及 `NVOS32_PARAMETERS` 结构 | 解析 VID_HEAP_CONTROL 参数体、区分分配/释放、读取 size 与 flags |
| `src/common/sdk/nvidia/inc/ctrl/ctrl2080/` | `NV2080_CTRL_*` 控制命令族(fb/bus/gpu/…,ctrl2080fb.h 里有 FB_GET_INFO,ctrl2080gpu.h 里有 GET_PIDS/GET_PID_INFO) | 后台校准线程反查驱动统计所需的结构与命令码 |
| `src/common/sdk/nvidia/inc/ctrl/ctrl0080.h`、`ctrla06c*.h` 等 | 按 class 分的控制命令 | SET_TIMESLICE 等算力相关(二期) |
| `src/nvidia/generated/g_allclasses.h` | 全部 class id(NV20_SUBDEVICE_0 等) | RM_ALLOC 的 hClass 判定 |
| `kernel-open/nvidia-uvm/uvm_ioctl.h` | UVM(unified memory)ioctl 族 | 若后续覆盖 managed memory 路径 |
| `src/common/inc/nvstatus.h`(及 sdk 下同名) | `NV_ERR_*` 状态码(如 NV_ERR_NO_MEMORY) | 伪造 OOM 时返回的真实状态码,保证 libcuda 侧行为一致 |

## 3. 价值所在

1. **消灭魔法数与手抄结构**:ioctl-controller 把 `3223340586`、0x51、`NV2080_CTRL_GPU_GET_PIDS_PARAMS_ID 49600` 等以魔法数形式硬编码并重复定义 4 次,是因为当年开源树不可得/未使用。现在直接 `#include` 这些头,定义有官方出处、随版本可升级。
2. **ABI 版本对齐的锚点**:pin 住一个 commit(本项目选用 `6dd092d`)即锁定一套 ABI;升级驱动支持 = 换 pin + 重新编译,结构体布局漂移由编译器发现而非运行时诡异行为。
3. **覆盖完整性可枚举**:27 个 escape 码 + NVOS32 子功能号是有限的、可列全的,可以做"拦截清单 ↔ 官方定义"的一致性检查(类似 HAMi 的 check-cuda-hook-consistency),避免遗漏分配路径。
4. **伪造状态码的正确性**:拒绝分配时要返回驱动真实会返回的 `NV_ERR_NO_MEMORY` 及正确 status 字段布局,否则 libcuda 行为异常;唯一可靠来源就是这套头文件。
5. **理解 ioctl 之上的全貌**:RM_ALLOC(hClass/hRoot 句柄体系)、MAP_MEMORY、DUP_OBJECT 等定义让我们知道哪些路径必须跟踪才能正确记账(例如 dup/import 的句柄共享)。

## 4. 在 libnvfair 中的使用方式(建议)

- 以 git submodule 或 `third_party/open-gpu-kernel-modules`(pin commit `6dd092d`)形式引入**头文件**,不进编译产物;
- `src/rm/rm_defs.h` 只做一件事:include 上述官方头 + 少量本项目自己的常量,不重复定义;
- 构建系统设 `OGKM_DIR` 变量单点声明 include 路径,避免多处写死;
- 升级 pin 时跑一遍 ABI 兼容测试(见 dev/design.md M3),而不是在代码里 `strstr(driver_version)`。

## 5. 风险与注意

- **头文件 ≠ ABI 保证**:这些头描述的是该版本驱动协议;旧驱动(470 系等闭源时代)结构可能不同。一期目标驱动矩阵确定后,用编译期 sizeof 断言 + 运行时能力探测兜底;
- 仓库体量大(~10MB 头文件子集可用),include 时按需取(`nv_escape.h` + `nvos.h` + 目标 ctrl 头),不要整树挂进 include path 导致符号污染;
- License:MIT/GPL dual,头文件使用无传染性顾虑,但 vendored 拷贝需在 NOTICE 中声明。
