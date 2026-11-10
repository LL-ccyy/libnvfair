# vcuda-controller 设计文档(调研)

> 来源:tkestack/vcuda-controller(master @ 72e0115,2026-11 快照)。Tencent TKEStack 的 GPU 虚拟化劫持库,gpu-manager 的组件。它是本系列已调研的两个项目的**共同祖先**:HAMi-core([02-hami.md](02-hami.md))由它衍生,内部 ioctl-controller([03-ioctl-controller.md](03-ioctl-controller.md))是它的 ioctl 路线二次开发产物。本文档补全谱系,并整理其机制与历史局限。

## 1. 定位与谱系

LD_PRELOAD 劫持库(产物 `libcuda-control.so`),在容器内拦截 CUDA Driver API 与 NVML 符号,配合 gpu-manager(K8s DevicePlugin + 调度器)实现单卡多 Pod 共享与 QoS(配套论文 IEEE 8672318)。

```
tkestack/vcuda-controller (2019, CUDA API + NVML 符号层)
├──→ Project-HAMi/HAMi-core(02-hami.md)      保持 CUDA API 层,重写记账/配置,持续活跃
└──→ 内部 ioctl-controller(03-ioctl-controller.md)        改走 hook libc ioctl 路线,内部演化
```

**注意**:尽管名字带 controller、且 ioctl-controller 自称其二次开发产物,vcuda-controller 本体 src/ 中 **零 ioctl 引用**——hook libc ioctl 的路线是下游发明,不是上游既有设计。

仓库状态(2026-11 GitHub API):2019-11-05 创建,546★/160 fork,Apache-2.0(Tencent);最后实质提交 **2022-11-15**(PR #31 补 cuGetProcAddress hook),pushed_at 2024-06;README 支持声明 **CUDA ≤ 11.5.1**;issue #41(2025-08)"Is this repo has been stopped?" 无官方回应。CHANGELOG 仅 v1.0(2019-11)/v1.0.1(2020-05)。

## 2. 整体结构

C 语言,CMake 3.5 构建,Docker 三阶段(CUDA 11.2-devel → centos7 rpmbuild)出 RPM,静态链接 libgcc/libstdc++。产物:`libcuda-control.so` + 独立工具 `nvml-monitor`。全仓 12,543 行:

| 文件 | 行数 | 说明 |
|---|---|---|
| src/cuda_originals.c | 2892 | 约 500 个 CUDA 符号的**纯转发**实现 |
| src/nvml_entry.c | 1705 | 约 250 个 NVML 符号的纯转发(nvidia-smi 路径) |
| src/loader.c | 1235 | 符号表(533 CUDA + 258 NVML)、配置加载、cgroup 解析、驱动版本探测 |
| src/hijack_call.c | 1055 | **唯一带业务逻辑的文件**:显存限额 + 算力令牌桶 |
| include/cuda-subset.h / nvml-subset.h | 2368/1314 | 手抄 CUDA/NVML 类型子集 |
| include/cuda-helper.h / nvml-helper.h | 1127/563 | 符号枚举表(单一事实源) |
| include/hijack.h | 192 | 配置结构 resource_data_t、宏、常量 |
| src/register.c | 92 | fork+exec gpu-client RPC 注册 |
| tools/monitor_dockernized.c | ~190 | 独立进程级利用率/显存报表工具 |

## 3. 核心机制

### 3.1 劫持方式:符号导出 + 按版本号 dlopen(非 dlsym/RTLD_NEXT)

- 库直接导出 `cuInit`/`cuMemAlloc_v2`/`nvmlInit` 等同名符号,靠 LD_PRELOAD 的符号查找顺序完成 interposition;
- 真实驱动库不走 `dlsym(RTLD_NEXT)`,而是用正则从 `/proc/driver/nvidia/version` 的 NVRM 行提取版本号(loader.c:1164-1182),`dlopen("libcuda.so.495.29.05")` **按精确文件名**打开(loader.c:858-940),逐符号 `dlsym` 填表;
- 表结构 `entry_t {fn_ptr, name}`(loader.c:34 起);`bug_on()` 用 `BUILD_BUG_ON` 编译期锚定"枚举数 == 表项数"(loader.c:832-838);`find_new_lib.sh` 用 `nm -D` diff 新驱动库导出符号,辅助手工补表;
- 拦截面 533+258 个符号中仅 **25 个有逻辑**(hijack_call.c:153-183 `cuda_hooks_entry`):显存分配族(cuMemAlloc/Managed/Pitch/Array/3D/Mipmapped)、视图族(cuDeviceTotalMem/cuMemGetInfo)、launch 族(cuLaunchKernel* / cuLaunchGrid* / cuLaunch / cuFuncSetBlockShape),外加 cuInit/cuDriverGetVersion/cuGetProcAddress 入口。其余全部纯转发;
- cuGetProcAddress(2022-11 最后一次功能提交)只在真返回后按表覆盖 `*pfn`(hijack_call.c:626-650);**未处理 CUDA 12 的 `cuGetProcAddress_v2`**。

### 3.2 显存限额:每次分配实时反查 NVML(无本地账本)

判定逻辑(hijack_call.c:652-914)统一模式:

```
hook cuMemAlloc*(...) :
  used = 0
  atomic_action(pid_path, get_used_gpu_memory, &used)   # 打开 pids.config 读 PID 表
  if used + request_size > g_vcuda_config.gpu_memory:
      return CUDA_ERROR_OUT_OF_MEMORY                   # 预检拒绝,真实 API 零调用
  转发真实 API
```

- `get_used_gpu_memory`(hijack_call.c:482-529):`nvmlDeviceGetComputeRunningProcesses` 取卡上全部进程,bsearch 匹配 pids.config 中本容器 PID 累加 usedGpuMemory。**热路径上每次分配一次 NVML 查询**,无任何本地记账;
- pids.config(`/etc/vcuda/<cid>/pids.config`)由 gpu-manager 在宿主侧写好挂进容器,原始 int 数组,天然绕开容器/宿主 pid 映射问题(所以它不需要 HAMi-core 的 hostpid_broker);
- **失败语义 fail-closed**:NVML 查询失败 → `used = gpu_memory(上限)` → 后续分配全部 OOM(hijack_call.c:497-499),与 libnvfair 的 BR3"错误降级直通"相反;
- 视图虚拟化仅在 CUDA 层:`cuDeviceTotalMem`/`cuMemGetInfo` 返回配额值(hijack_call.c:916-966);**NVML 层纯转发**(nvml_entry.c:498-502),`nvidia-smi` 看到的仍是真实显存(HAMi-core 后来才补上 NVML 视图)。

### 3.3 算力限额:用户态令牌桶(后来的 HAMi 路线即源于此)

- 总量 `g_total_cuda_cores = sm_num × max_thread_per_sm × 32(FACTOR)`(hijack_call.c:583);
- launch 族 hook 先过 `rate_limiter(grids, blocks)`(hijack_call.c:255-274):CAS 扣减令牌(扣 grids,blocks 参数实际未用),令牌 <0 则 `nanosleep(10ms)` 自旋重试;
- 后台线程 `utilization_watcher`(120ms 周期,hijack_call.c:303-378):NVML `nvmlDeviceGetProcessUtilization` 按本容器 PID 采样 smUtil(+enc/dec×0.85 归一),`delta()`(hijack_call.c:276-300)按与上限的差距弹性增减令牌,CAS `change_token`;
- 双策略:`hard_limit=1` 恒定上限;`=0` 软限——单进程按 `limit` 跑满,多进程时每 30 个采样周期按系统空闲量逐步上调 `up_limit`(CHANGE_LIMIT_INTERVAL=30 / USAGE_THRESHOLD=5),即"空闲弹性借用";
- 这是纯用户态闭环,与驱动版本无关——与 ioctl-controller"改写驱动 SET_TIMESLICE"是两条不同路线。

### 3.4 配置与注册通道

| 通道 | 内容 | 说明 |
|---|---|---|
| 二进制文件 `/etc/vcuda/<cid>/vcuda.config` | packed struct `resource_data_t` 原样 read() | gpu-manager CGO 写入;`occupied[4044]` 魔法占位字段(hijack.h:121-136);cgroup 解析 container id 定位目录(loader.c:943-1117) |
| `pids.config` | 容器进程 PID 裸 int 数组 | 同目录 |
| 环境 `LOGGER_LEVEL` | 日志级别 | |
| fork+exec RPC | `/usr/local/nvidia/bin/gpu-client --addr /etc/vcuda/vcuda.sock ...`(register.c:35-92) | 向 gpu-manager unix socket 注册(bus_id/pod_uid/container);gpu-client 是独立二进制,非库内网络调用 |

驱动库定位依赖版本号字符串(dlopen `libcuda.so.<ver>`),配置里有 `driver_version` 字段但实际以 procfs 正则结果为准。

## 4. 技术债与历史局限

1. **符号表维护成本即路线成本**:533+258 项枚举、2892 行纯转发文件手写;CUDA 12 以后未跟进(cuGetProcAddress_v2 缺失、cuMemAllocAsync/mempool 族缺失),README 自认 CUDA ≤11.5.1;
2. **驱动版本字符串驱动行为**:正则解析 procfs 版本 → 拼精确 dlopen 文件名;解析失败或库缺失 → `LOGGER(FATAL)` → `exit(-1)` **杀死宿主业务进程**;
3. **热路径 NVML 查询**:每次分配一次驱动查询,无记账、无短路(对比 ioctl-controller(03)还有 256M/500 次/64M 三重短路;对比 HAMi-core 零查询);
4. **LOGGER 宏每次调用 getenv**(hijack.h:142-157);FATAL 直接退出;
5. **自旋锁热路径**:`rate_limiter` CAS 自旋 + 10ms 睡眠循环;`cuFuncSetBlockShape` 忙等自旋锁(hijack_call.c:1039-1055);
6. **全局状态散落**:十几个文件级 `g_*` 变量(g_cur_cuda_cores、g_pids_table、g_block_x/y/z…),无 ctx 收敛;
7. **手抄头文件**:cuda-subset.h 2368 行 + nvml-subset.h 1314 行从 CUDA/NVML 官方头手工摘录,版本漂移靠人肉;
8. **单卡假设**:NVML 调用全部硬编码 device 0(hijack_call.c:407/495/537),gpu-manager 一容器一卡的前提;
9. **无任何单测**:仓库没有 test 目录,唯一工具是 monitor;CMake 的 `USE_ORIGINAL` 开关两分支目标完全相同,是死配置;
10. **死代码与魔法数**:pids 表固定 MAX_PIDS=1024、FACTOR=32、0.85 CODEC 归一化等常量无来源注释。

## 5. 三项目对照(谱系视角)

| 维度 | vcuda-controller(本文) | ioctl-controller(03) | HAMi-core(02) | libnvfair(dev/design.md) |
|---|---|---|---|---|
| 拦截面 | CUDA API + NVML 符号导出 | libc ioctl(RM ioctl) | dlsym dispatch(CUDA API + NVML) | libc ioctl(RM ioctl) |
| 真实符号解析 | dlopen `libcuda.so.<版本>` | dlsym(RTLD_NEXT) | 汇编跳板 hook dlsym | dlsym(RTLD_NEXT) |
| 显存记账 | 无(每次 NVML 反查) | 无(每次反查 + 三重短路) | 共享内存账本 + 周期校准 | 共享内存账本 + 后台校准(PBI-006/007) |
| 算力 | 用户态令牌桶 | 改写驱动 timeslice | 用户态令牌桶 | 二期(PBI-201) |
| 配置 | 二进制 packed 文件 + cgroup | 同左(继承)+ HTTP | 环境变量 + 文本 limits | 环境变量(二期配置文件) |
| 宿主耦合 | 强绑定 gpu-manager(RPC 注册) | 强绑定 gpu-manager(HTTP) | 弱(可独立运行) | 无(BR4) |
| 失败语义 | NVML 失败→全 OOM;FATAL exit | FATAL exit 风险 | pid 找不到→算力关闭显存仍生效 | 任何错误→直通(BR3) |
| 多卡 | device 0 硬编码 | — | 按卡 `_ <n>` 覆盖 | 计划按 UUID(拓展 5.3) |
| 测试 | 无 | 测试宏污染生产 | 20+ 无 GPU 单测 | ctest + fake backend |
| 状态 | 停滞(2022-11 后无实质开发) | 内部演化 | 活跃 | 立项中 |

## 6. 对 libnvfair 的启示

**正面借鉴**:
- **枚举表 + 编译期一致性锚定**:entry_t 表 + `BUILD_BUG_ON(枚举数==表项数)` 与 libnvfair PBI-003 的 `static_assert(sizeof/offsetof)` 同一方法论,证明该模式在这类项目里十年有效;
- **预检→伪造 OOM 的判定语义**:先判后调、拒绝时真实 API 零调用,与 PBI-004 AC1 一致,是最小正确形态;
- **令牌桶算力限制是纯用户态方案**:不碰驱动参数、与驱动版本解耦,PBI-201 的"用户态令牌桶"分支可参照其 delta() 弹性算法与硬/软双策略,但须修正其自旋等待与全局变量问题;
- **25/791 的拦截比例**:绝大多数符号只需转发,关键逻辑收敛在极少数入口——libnvfair 的 ioctl 路线天然更少(NV_ESC_* 个位数),但同样要区分"转发壳"与"业务壳"。

**反面教训(对既有结论的印证与新证据)**:
- **按驱动版本字符串定位/分支是脆弱起点**:vcuda 用 procfs 正则拼 dlopen 文件名,ioctl-controller 用 strstr("470"),HAMi-core 彻底去掉版本串——libnvfair 的"按 dataSize 分支、不依赖版本号"(PBI-003 AC3)是三者经验的收束;
- **热路径驱动反查不可持续**:vcuda 每分配一查、无任何短路,是 ioctl-controller 三重短路和 HAMi 本地账本共同的反面起点,印证 BR6/PBI-007 的必要性;
- **fail-closed 是错误方向**:NVML 查询失败即全量 OOM 会让宿主业务随辅助通道故障而不可用;libnvfair BR3(错误降级直通)的对照样本;
- **手抄头文件的终局**:2368+1314 行手抄类型 + 停在 CUDA 11.5,印证 open-gpu-kernel-modules 调研(04)的决策(ogkm 官方头为单一事实源、不手抄);
- **它是 ioctl-controller(03)技术债的"更早形态"**:巨型转发文件、全局变量状态机、无测试,在 2019 年的原型里已全部存在;ioctl-controller 与 HAMi-core 是同一原型的两条修正路线,libnvfair 选择的是第三条(ioctl 层 + 现代工程实践)。
