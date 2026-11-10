# 外部项目调研

本目录是对外部项目的调研笔记,按谱系排序编号——**vcuda-controller 是共同祖先,其余是它的两条修正路线 + 两个参照物**:

```
tkestack/vcuda-controller (01, 2019, CUDA API + NVML 符号层)
├──→ Project-HAMi/HAMi-core (02)   保持 CUDA API 层,重写记账/配置,持续活跃
└──→ 内部 ioctl-controller (03)    改走 hook libc ioctl 路线(与 libnvfair 同路线)
                                    │
NVIDIA/open-gpu-kernel-modules (04) ┼── RM ioctl 协议官方定义,libnvfair 的头文件来源
geohot/cuda_ioctl_sniffer      (05) ┘   裸 ioctl 可行性验证 + dump 工具方法论
```

- 01/02/03 是"同一种需求的三种实现",横向对照见 01 §5;
- 04 是协议事实源(不是同类竞品),libnvfair 直接 include 它的头;
- 05 证明 ioctl 路线完备可行,其 pstruct 方法论被 PBI-101(审计模式)采用。

新调研文档请按编号续排,并把谱系/引用关系同步进 [docs/README.md](../README.md)。
