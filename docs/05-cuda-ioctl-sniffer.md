# cuda_ioctl_sniffer 调研文档

> 来源:geohot/cuda_ioctl_sniffer(George Hotz,tinygrad 作者;master 快照,已下载 /tmp/cuda_ioctl_sniffer)
> 对 libnvfair 的价值:**证明"裸 ioctl 足以完整驱动 GPU"的最小可行证据** + 一套可直接复用的 ioctl 观测/解析工具链。

## 1. 它是什么

一个研究性小项目(总代码约 1800 行),两个互相支撑的目标:

1. **sniff.so** — LD_PRELOAD 劫持库,拦截 CUDA 应用的全部 NVIDIA ioctl/mmap,把每个调用的参数结构**按官方头文件定义格式化打印**出来;
2. **gpu_driver / tc_context** — 完全不链接 libcuda,只用裸 ioctl + mmap 完成 GPU 初始化(`cuInit`+`cuCtxCreate` 的替代品)并推送 command buffer 跑一个 kernel(`cudaless.sh`:"launch a CUDA kernel without using the CUDA driver runtime")。

## 2. 核心机制

### 2.1 观测端(sniff.cc,600 行)

- LD_PRELOAD 导出 `ioctl`/`mmap`/`munmap` 等符号,`dlsym(RTLD_NEXT,...)` 取真实实现——与 libnvfair 同一注入手法;
- 直接 include open-gpu-kernel-modules 头:`nv-ioctl.h`、`nv-ioctl-numbers.h`、`nv_escape.h`、`nvos.h`、`ctrl2080.h`、`ctrla06c.h`、`g_allclasses.h` 等(sniff.cc:15-39)——即 04 文档所述的官方协议源;
- **pstruct(pstruct/pstruct.py + include.cc)**:一个头文件解析器,扫描 ogkm 头生成 `out/printer.h`——把每个 ioctl 参数结构转成"按字段名+类型"的打印函数。这是它最独特的资产:**不用手写每个结构的 dump 代码,协议定义自动变成可读的调用追踪**;
- mmap 拦截 + SIGSEGV 陷阱页:对 GPU MMIO/命令队列映射页先映射假页,写触发 segfault 后在 handler 里反汇编 RIP 处指令识别是哪种 store,转写真实页并 dump 命令 buffer(sniff.cc:55-130)。这让它能观测"不写 ioctl、直接写寄存器"的路径(GPFIFO push)。

### 2.2 执行端(gpu_driver.cc 372 行 + tc_context.cc 261 行)

用 sniff 观测到的 ioctl 序列,手工重放等价调用:open `/dev/nvidiactl` → RM_ALLOC 建立 client/subdevice 句柄树 → 分配显存对象 → MAP_MEMORY → 建 channel/GPFIFO → 推送 PTX 编译出的 kernel 命令。验证了对 ioctl 协议的理解是自洽完备的。

## 3. 价值所在

1. **路线可行性背书**:geohot 用 ~600 行证明 ioctl 层是理解与控制 NVIDIA 用户态栈的**充分**层次——libcuda 所做的一切最终都可表示为有限 RM ioctl 序列。libnvfair 在同层次做"限制"而非"重放",复杂度低一个量级。
2. **白嫖的调试工具**:`sniff.so` 几乎可以直接拿来用——开发 libnvfair 时用它 dump 真实 CUDA 应用的 ioctl 流,回答"cuMemAlloc 实际走哪个 NVOS32 子功能、flags 是什么、哪个驱动版本引入了新路径",替代 ioctl-controller 式的"6 组 flags 硬编码 + 手工试凑"。
3. **pstruct 的方法论**:协议结构 → 自动生成打印代码。libnvfair 的调试/审计模式(03 文档 M3)应采用同一思路:从 ogkm 头生成 dump 代码,而不是手写。
4. **给出拦截清单的实验验证手段**:03 文档的风险项"分配路径不止 HEAP_CONTROL 一类",可以用 sniff.so 对 PyTorch/真实工作负载跑一遍得到经验性全集,再决定白名单。
5. **mmap 写路径的警示**:命令提交(GPFIFO doorbell)不走 ioctl 走 MMIO 直写——提醒 libnvfair:ioctl 层能管住**资源分配**,管不住**命令流**;这再次印证一期只做显存限额、算力方案待评估的边界设定。

## 4. 使用建议

- 作为**开发期工具**引入(dev-only):`third_party/` 或独立 clone,不进发布产物;
- 典型用法:`LD_PRELOAD=sniff.so python -c "import torch; ..."` 抓分配路径样本,沉淀为 libnvfair 的拦截清单测试用例;
- 不抄其代码风格(全局 map、exit(-1)、readelf 式探索代码)——它是研究原型,价值在数据与方法,不在工程质量。

## 5. 关键文件

| 文件 | 行数 | 说明 |
|---|---|---|
| sniff.cc | 600 | ioctl/mmap 劫持与结构化打印;SIGSEGV 陷阱页观测 MMIO |
| pstruct/pstruct.py + include.cc | — | 从 ogkm 头自动生成结构打印器(方法论核心) |
| gpu_driver.cc | 372 | 裸 ioctl 推送命令队列跑 kernel |
| tc_context.cc/h | 261/25 | cuInit/cuCtxCreate 的裸 ioctl 替代实现 |
| make_sniff.sh / run.sh / driver.sh / cudaless.sh | — | 构建与演示脚本(依赖 open-gpu-kernel-modules 头路径) |
| saxpy.cu / simple.ptx / d2d.cu | — | 观测用样例负载 |
