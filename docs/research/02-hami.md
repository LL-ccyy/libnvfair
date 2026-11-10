# HAMi 设计文档(调研)

> 来源:HAMi 主仓库(Project-HAMi/HAMi)+ HAMi-core 劫持库仓库(Project-HAMi/HAMi-core),2026-11 master/main 快照。
> 谱系:HAMi-core 衍生自 tkestack/vcuda-controller(见 01-vcuda-controller.md)。

## 1. 定位与整体架构

HAMi 是 Kubernetes 上的 vGPU 共享方案:一张物理 NVIDIA 卡可被多个 Pod 按显存/算力配额共享使用。核心思想是**在容器内通过 LD_PRELOAD 劫持 CUDA/NVML 符号,实现显存硬限制与 SM 算力软限制**,应用与驱动零改动。

组件划分(三个可独立交付的部分):

| 组件 | 语言 | 职责 |
|---|---|---|
| scheduler(扩展调度器) | Go | 按 Pod 注解 `nvidia.com/gpumem`、`nvidia.com/gpucores` 做设备感知调度,把分配结果写回 Pod annotation |
| device-plugin | Go | 实现 K8s DevicePlugin 协议;Allocate 阶段把 `libvgpu.so` 注入容器(改 `ld.so.preload` 或直接挂 LD_PRELOAD),并下发限制配置 |
| libvgpu.so(HAMi-core) | C | 容器内劫持库,真正的限制执行点 |

数据流:

```
Pod 注解请求资源 → scheduler 打分绑定 → device-plugin Allocate
  → 写 LD_PRELOAD/环境变量/limits 文件 → 容器启动
  → libvgpu.so 随进程加载,劫持 CUDA/NVML 调用,执行显存+算力限制
```

scheduler 与 device-plugin 之间用 **node annotation** 通信(早期版本用 gRPC,已改为 annotation,见 CHANGELOG)。

## 2. HAMi-core(libvgpu.so)核心机制

### 2.1 Hook 的层次:动态链接层,而非 syscall 层

HAMi-core **不 hook libc 的 ioctl**,而是在动态链接层拦截 CUDA 驱动 API(`libcuda.so` 的 `cu*` 符号)与 NVML(`libnvml.so` 的 `nvml*` 符号)。

机制:

- 用一段汇编 `dlsym_entry.S`(x86_64/aarch64)重写 `dlsym` 符号。因为 LD_PRELOAD 库本身调用 `dlsym(RTLD_NEXT, ...)` 会产生递归,用汇编跳过自身。对 `RTLD_NEXT` 保留返回地址直接跳真实 dlsym;其他情况跳 `libvgpu_dlsym_dispatch`。
- `libvgpu_dlsym_dispatch`(libvgpu.c:97):符号以 `cu` 开头 → 触发一次 `preInit`(pthread_once),并从 dlopen 的 vgpulib 中取出自身实现返回;以 `nvml` 开头 → 查 NVML hook 表。libcudart 运行时会通过 dlsym 解析驱动符号,于是被引导到 HAMi 的实现。
- 真实 dlsym 用 `dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"/"GLIBC_2.17")` 按 glibc 版本逐个尝试取得(libvgpu.c:42-86)。
- 兼容 `cuGetProcAddress`:CUDA 12.x 起驱动推荐用 `cuGetProcAddress` 取符号,HAMi 同样 hook 了该函数及其 `_v2` 变体(libcuda_hook.h:272)。

### 2.2 Hook 的符号集合

`libcuda_hook.h` 中用枚举 + 表(`cuda_library_entry[]`)集中声明约 **280 个 CUDA Driver API 符号**,分 cuInit / cuDevice / cuContext / cuStream / cuMemory / cuGraph / Virtual Memory / External Resource 等类别。关键点:

- **单一事实源**:符号表集中在一个头文件里枚举,配套实现分布在 `src/cuda/`(hook.c 414 行、memory.c 831 行、graph.c 310 行)。CI 有 `make check-cuda-hook-consistency` 静态检查表与实现的一致性。
- 绝大多数符号是**纯转发**(LOG + 转发真实函数);只有显存分配/释放、上下文、NVML 查询类符号带限制逻辑。
- NVML hook(`src/nvml/` nvml_entry.c 1489 行 + hook.c):改写 `nvmlDeviceGetMemoryInfo` 等返回值,让 `nvidia-smi` 在容器内显示虚拟显存大小。

### 2.3 显存限制

配置:环境变量 `CUDA_DEVICE_MEMORY_LIMIT`(全局)、`CUDA_DEVICE_MEMORY_LIMIT_<n>`(单卡覆盖),支持 `1g/1024m/...` 单位,0 或不设表示不限。解析逻辑独立成 `parse_limit_value`(multiprocess_memory_limit.c:121),有完备边界处理。

执行:hook 住所有分配入口(`cuMemAlloc_v2`、`cuMemAllocManaged`、`cuMemAllocAsync`、`cuMemAllocFromPoolAsync`、`cuMemCreate`、mipmapped array 等),分配**前**检查共享内存中的记账,超限直接返回 `CUDA_ERROR_OUT_OF_MEMORY`(memory.c 中约 20 处统一返回点)。

记账方式 —— **共享内存文件 + 原子操作**,而非每次问驱动:

- 每个容器一个 mmap 文件,默认 `/tmp/cudevshr.cache`(可用 `CUDA_DEVICE_MEMORY_SHARED_CACHE` 覆盖)。
- 结构体 `shared_region_t`(multiprocess_memory_limit.h:94-112):全局区(limit、sm_limit、uuid、sem 信号量、proc_num、utilization_switch)+ 每进程槽位数组 `procs[1024]`。
- 每进程槽位 `shrreg_proc_slot_t`:pid、hostpid、每卡已用显存(分 context/module/data 三类原子计数)、monitor 校准值、状态、seqlock。全字段 `_Atomic`,槽位指针缓存 + 每次使用前校验 pid 仍匹配(get_current_proc_slot,防槽位压缩后串槽)。
- 分配/释放在共享区原子加/减;后台 monitor 周期用 NVML 实测值校准(防止记账漂移)。

### 2.4 算力(SM 利用率)限制

配置:`CUDA_DEVICE_SM_LIMIT`(百分比)、`CUDA_DEVICE_SM_LIMIT_<n>`、`GPU_CORE_UTILIZATION_POLICY`(force/disable)。

算法:**令牌桶式时间分片**(自实现,非驱动 timeslice):

1. 进程内启动 watcher 线程 `utilization_watcher`(multiprocess_utilization_watcher.c:285):周期(默认约 1s)用 NVML `nvmlDeviceGetProcessUtilization` 采样本容器所有进程的 smUtil 之和。
2. 与 `sm_limit` 比较,闭环调节共享变量 `g_total_cuda_cores/g_cur_cuda_cores`(令牌数),`change_token`/`delta` 用 CAS 更新。
3. hook 的 kernel 启动类符号(`cuLaunchKernel*` 等)在执行前消耗令牌;令牌耗尽时 `ENSURE_RUNNING()` 宏挂起/等待,直到 watcher 补充令牌。效果是该容器平均 SM 利用率不超过配额。

### 2.5 进程映射与 hostpid

容器内 pid 与宿主机 pid 不同。HAMi-core 通过 `hostpid_broker.c`(480 行)+ `hostpid_fallback_lock.c`(584 行)解析 `/proc/self/cgroup`、扫描容器根进程树把容器 pid 映射到 host pid,NVML 查询(返回 host pid)才能对应到共享区槽位。找不到 hostpid 时(pidfound==0)算力限制自动关闭、仅显存生效。

### 2.6 配置与状态注入方式汇总

| 通道 | 内容 | 写入方 |
|---|---|---|
| LD_PRELOAD(/ld.so.preload) | libvgpu.so 路径 | device-plugin 写 `/usr/local/vgpu/ld.so.preload` 挂进容器 |
| 环境变量 | `CUDA_DEVICE_MEMORY_LIMIT*`、`CUDA_DEVICE_SM_LIMIT*`、`CUDA_DEVICE_MEMORY_SHARED_CACHE`、`POD_UID`、`CONTAINER_NAME` 等 | device-plugin Allocate 响应 |
| limits 文件 `/overrideEnv` | KEY=VALUE 文本,原子 rename 写入,容器内 libvgpu 读取 | device-plugin(limits.go:105-179) |
| 共享内存文件 | shared_region_t,显存/算力/进程状态 | libvgpu 写,Go 侧 monitor 读(v0/v1 spec.go 双版本布局,带 major/minor 版本号) |

## 3. 与 TKEStack vcuda-controller 的关系

HAMi-core 衍生自 TKEStack 的 vcuda-controller(谱系详情见 01-vcuda-controller.md),主要区别(CHANGELOG 及代码观察):

- 记账从"每次向驱动 ioctl 查询"改为"进程内原子记账 + 共享内存 + NVML 周期校准",消除热路径上的 ioctl 风暴;
- 适配 CUDA 11.3+/12.x/13.x 的新分配 API(cuMemAllocAsync、mempool、cuGetProcAddress);
- 算力限制从"改写驱动 timeslice"改为"用户态令牌桶 + NVML 闭环调节",与驱动版本解耦;
- 配置通道从二进制 packed struct 文件改为环境变量 + 文本 limits 文件。

## 4. 设计上的优点(值得学习)

1. **hook 表单一事实源**:280 个符号集中在 `libcuda_hook.h` 枚举,配合 CI 一致性检查;新增符号只需一处登记。
2. **热路径零 IPC、零驱动查询**:显存判定只读写共享内存原子变量;慢查询(NVML)放在 watcher 线程且不持锁(见 utilization_watcher.c:242 注释明确说明先查后锁)。
3. **模块边界清晰**:`src/cuda`(CUDA hook)、`src/nvml`(NVML hook)、`src/allocator`(显存跟踪)、`src/multiprocess`(共享区+限流)、`src/hostpid_*`(pid 映射),每个文件 <1700 行,职责单一。
4. **并发严谨**:全字段 `_Atomic` + seqlock 快照 + 槽位指针使用前再校验 + 锁外做慢 NVML 查询;`test/` 下 20+ 个针对并发/竞态的测试(test_concurrent_oom_race、test_shared_region_concurrency、test_postinit_owner_death 等),大多不需要 GPU,用 ctest 跑。
5. **版本演进可控**:共享区结构带 major/minor 版本号;Go monitor 侧 v0/v1 双 spec 共存迁移。
6. **与驱动版本解耦**:不解析驱动版本字符串,所有限制逻辑基于公开 CUDA API 语义。

## 5. 关键文件清单

| 路径 | 行数 | 说明 |
|---|---|---|
| HAMi-core/src/libvgpu.c | 981 | dlsym 劫持入口、preInit 初始化、进程生命周期 |
| HAMi-core/src/dlsym_entry.S | ~36 | 汇编 dlsym 跳板,防 RTLD_NEXT 递归 |
| HAMi-core/src/include/libcuda_hook.h | 285 | CUDA 符号枚举表(单一事实源) |
| HAMi-core/src/include/libnvml_hook.h | 548 | NVML 符号表 |
| HAMi-core/src/cuda/memory.c | 831 | 所有显存分配/释放 hook 与限额判定 |
| HAMi-core/src/cuda/hook.c | 414 | 通用转发与上下文 hook |
| HAMi-core/src/allocator/allocator.c | 660 | 显存记账 |
| HAMi-core/src/multiprocess/multiprocess_memory_limit.c | 1610 | 共享区管理、limit 解析、进程槽位 |
| HAMi-core/src/multiprocess/multiprocess_memory_limit.h | ~170 | shared_region_t 布局、环境变量名、常量 |
| HAMi-core/src/multiprocess/multiprocess_utilization_watcher.c | 357 | SM 令牌桶闭环限流 |
| HAMi-core/src/hostpid_broker.c / hostpid_fallback_lock.c | 480/584 | 容器 pid → 宿主 pid 映射 |
| HAMi-core/src/nvml/nvml_entry.c | 1489 | NVML hook(虚拟化 nvidia-smi 视图) |
| HAMi/pkg/device-plugin/.../limits.go | ~180 | 环境变量/limits 文件下发 |
| HAMi/pkg/monitor/nvidia/v1/spec.go | ~87 | Go 侧共享区结构镜像(读取方) |
| HAMi/lib/nvidia/ld.so.preload | 1 | 容器内 preload 配置 |

## 6. 对 libnvfair 的启示

- hook 点选择:HAMi 选择 CUDA API 层而非 ioctl 层,换来的是**驱动兼容性**(不用理解 RM ioctl 参数结构)和**记账精度**(应用层视角的分配与释放天然成对)。代价是要维护一张大符号表,并跟随 CUDA 新 API 演进。
- ioctl 层 hook(见 03-ioctl-controller.md)的代价则是:要自己解析 `NV_ESC_*` 命令码与参数结构,驱动版本差异大,且分配记账只能靠反向查询驱动,热路径昂贵。
- 两条路线的公共底座是一致的:LD_PRELOAD 注入、共享内存记账、NVML 监控校准、环境变量/文件配置通道。
