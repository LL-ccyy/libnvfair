# 文档索引

分两类:`research/` 是外部项目调研(理解现状、借鉴经验),`dev/` 是本项目的开发文档(设计与计划)。

## research/ — 外部项目调研

按谱系排序,vcuda-controller 是其余两者的共同祖先:

| 文档 | 主题 | 一句话结论 |
|---|---|---|
| [01-vcuda-controller.md](research/01-vcuda-controller.md) | tkestack/vcuda-controller(2019,共同祖先) | CUDA API + NVML 符号劫持原型;停滞于 CUDA 11.5;HAMi-core 与 ioctl-controller 都由它衍生 |
| [02-hami.md](research/02-hami.md) | HAMi / HAMi-core(活跃) | 同为 CUDA API 层,但记账改共享内存账本 + NVML 校准,算力用用户态令牌桶;工程实践最佳 |
| [03-ioctl-controller.md](research/03-ioctl-controller.md) | ioctl-controller(闭源原型,已脱敏) | 与 libnvfair 同路线(hook libc ioctl);原型演化状态,技术债即反面教材清单 |
| [04-open-gpu-kernel-modules.md](research/04-open-gpu-kernel-modules.md) | NVIDIA open-gpu-kernel-modules | RM ioctl 协议唯一权威定义源;libnvfair 以 submodule pin 引入其头文件 |
| [05-cuda-ioctl-sniffer.md](research/05-cuda-ioctl-sniffer.md) | geohot/cuda_ioctl_sniffer | 证明裸 ioctl 足以驱动 GPU;其 pstruct 头解析方法论用于审计 dump 代码生成 |

三项目横向对照表见 [01-vcuda-controller.md §5](research/01-vcuda-controller.md)。

## dev/ — 开发文档

| 文档 | 主题 |
|---|---|
| [design.md](dev/design.md) | libnvfair 设计:路线选择(ioctl 层)、设计原则(对 03 技术债的正面回答)、模块划分、记账策略、配置、里程碑 M1-M4 |
| [backlog.md](dev/backlog.md) | 产品 Backlog:基础需求 BR1-6、PBI 与验收标准(P0 MVP / P1 交付质量 / P2 二期 / P3 方向池) |
| [hook-background.md](dev/hook-background.md) | Hook 机制背景:MAGIC 直通判定 / GLOBAL DEFAULT / RTLD_NEXT |
| [log-background.md](dev/log-background.md) | 日志系统背景:输出方式与原子性 / 内存序 relaxed 选型 |

## 当前状态速览

- PBI-001(ioctl hook 直通骨架)、PBI-002(日志与配置解析,含单测)已完成;下一个是 PBI-003(rm_defs.h 协议定义层)
- 工程现状:`src/ioctl_hook.cpp` 直通 hook + LD_PRELOAD 冒烟通过;`src/util/log` + `src/lib/config`
  完成并纳入 `nvfair_test`(ctest:config / log 两组 16 用例)
