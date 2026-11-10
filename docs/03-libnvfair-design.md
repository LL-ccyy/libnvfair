# libnvfair 设计文档

> 状态:草案 v0.1(待评审)
> 参考:`docs/01-hami-design.md`(HAMi)、`docs/02-ioctl-controller-design.md`(ioctl-controller 闭源原型调研)、`docs/04-open-gpu-kernel-modules.md`(RM ioctl 官方协议源)、`docs/05-cuda-ioctl-sniffer.md`(ioctl 路线可行性验证 + 观测工具)

## 1. 定位与目标

libnvfair 是一个 **LD_PRELOAD 动态劫持库**:hook libc 的 `ioctl()` 符号,拦截进程对 `/dev/nvidia*` 的 NVIDIA RM ioctl 调用,在用户态实现类似 HAMi 的 GPU 资源共享隔离:

- **显存限额**:拦截显存分配 ioctl,超出配额时向应用返回 OOM;
- **算力限额**(二期):限制进程 SM 时间片/利用率;
- 面向场景:不改动应用、不改动驱动、不依赖 K8s(可独立使用,也可被上层资源管理器集成)。

与 ioctl-controller(闭源原型,见 02 文档)的关系:同一技术路线,但目标是**重写一个可维护的实现**,不是修补。

## 2. 路线选择:为什么是 ioctl 层

| 维度 | CUDA API 层(HAMi-core) | libc ioctl 层(本项目) |
|---|---|---|
| 拦截面 | 280 个 `cu*`/`nvml*` 符号,要跟随 CUDA 演进 | 少量 `NV_ESC_*` 命令码,集合稳定 |
| 兼容性 | 与 CUDA 版本强绑定(新 API 要补 hook) | 与驱动 ABI 绑定(RM ioctl 结构),同样需适配 |
| 记账 | 应用层分配/释放天然成对,可本地记账 | ioctl 难区分配额归属,需反查驱动或自行维护 |
| 覆盖 | 只覆盖走 libcuda 的调用 | 覆盖一切走 ioctl 的路径(包括非 CUDA 场景) |
| nvidia-smi 视图 | 需额外 hook NVML | 需额外 hook NVML(同样问题) |

**决策**:一期做 ioctl 层显存限额(核心诉求),算力限额延后;NVML 视图虚拟化作为可选二期能力。若实践发现记账精度不可接受,可在二期补充 CUDA API 层 hook(混合路线,两者复用同一配置/记账底座)。

## 3. 设计原则(对 ioctl-controller 技术债的正面回答)

1. **单一事实源**:ioctl 命令码、NVOS32/NV2080 结构、配置项、环境变量名,各自只定义一次(头文件或代码生成),C/C++ 与任何 Go 侧共享定义走生成而非手工对齐;
2. **模块小而正交**:每文件 <800 行,hook 分发、RM 结构解析、显存记账、配置、日志各自独立;
3. **热路径不阻塞、不分配、不发网络请求**:ioctl 拦截路径只读写进程内/共享内存状态;慢操作(NVML 查询、配置加载)放后台线程;
4. **状态收敛**:全部运行时状态收进一个 `struct nvfair_ctx`(类比 HAMi 的 shared_region),禁止散落的文件级全局变量构成隐式状态机;
5. **不做宿主编排**:本仓库只交付 .so + 头文件 + 测试;LD_PRELOAD 注入、配置下发由使用方(gpu-manager 类组件或运维脚本)负责;
6. **危险操作禁令**:劫持库内禁止 `exit/abort`(错误必须降级透传)、禁止把外部数据当格式串、fd 作为键的容器必须随 close hook 清理;
7. **测试可独立运行**:核心逻辑(结构解析、记账、限额判定)与 hook 机制分离,单测不依赖 GPU、不依赖真实 ioctl(用 fake backend 注入)。

## 4. 架构设计

### 4.1 模块划分

```
libnvfair/
├── src/
│   ├── hook/
│   │   ├── ioctl_hook.c        # ioctl 符号导出与分发(薄层,只做识别与路由)
│   │   └── hook_util.c         # dlsym/RTLD_NEXT 解析、真实符号缓存
│   ├── rm/
│   │   ├── rm_defs.h           # NV_ESC_*、NVOS32_*、NV2080_* 定义(唯一来源:include open-gpu-kernel-modules 官方头,见 docs/04)
│   │   ├── rm_alloc.c          # 显存分配/释放类 ioctl 的解析与判定
│   │   └── rm_query.c          # 反查驱动统计(FB_GET_INFO / GET_PIDS / GET_PID_INFO)
│   ├── core/
│   │   ├── ledger.c            # 显存记账(共享内存账本 + 配额判定)
│   │   ├── ledger.h            # 账本结构定义(shared region 布局,带版本号)
│   │   ├── config.c            # 配置加载(环境变量 + 配置文件 + 热更新)
│   │   └── ctx.c               # nvfair_ctx 生命周期(pthread_once 初始化/析构)
│   └── util/
│       ├── log.c               # 分级日志(默认 stderr,绝不致命退出)
│       └── cgroup.c            # 容器身份解析(可选,供记账分区)
├── include/libnvfair/nvfair.h  # 公开头文件(配置结构、错误码)
├── test/                       # ctest;fake RM backend + 并发/竞态测试
└── CMakeLists.txt
```

### 4.2 数据流

```
应用 ioctl(fd=/dev/nvidia*, req=NV_ESC_RM_*)
        │
        ▼
  ioctl_hook(薄分发) ──非 NV magic──▶ 真实 ioctl(直通)
        │
        ▼
  rm_alloc / rm_query 解析参数
        │
        ▼
  ledger_try_charge(bytes)        ← 读 ctx->ledger(共享内存原子量)
        │ 配额内                │ 超限
        ▼                      ▼
  真实 ioctl + 记账         伪造 RM 状态码(NV_ERR_NO_MEMORY)返回
        │
  rm_query 后台线程周期用驱动真实统计校准账本(防漂移)
```

### 4.3 显存记账策略

对比三种方案后选择 **本地账本 + 驱动校准**(HAMi 思路),而非 ioctl-controller 的"每次反查驱动":

- 分配 ioctl 成功后按请求 size 记 +;释放类 ioctl(`NV_ESC_RM_FREE`)按句柄表查回 size 记 −;
- 句柄表:进程内维护 `hObject → size` 映射(加锁哈希表),close hook 里清理整表;
- 容器级账本放共享内存(`mmap` 文件,默认 `/tmp/nvfair-<id>.cache`),结构带 magic + 版本号,字段全部 `_Atomic`,参照 HAMi `shared_region_t` 布局;
- 账本与驱动漂移的场景(进程崩溃、句柄泄漏)由后台校准线程兜底:低频(秒级)用 `NV2080_CTRL_CMD_GPU_GET_PID_INFO` 反查修正;
- 分配**前**预检 + 分配**后**记账,预检失败直接返回 OOM 不触碰驱动。

### 4.4 配置设计

收敛为两级,单一解析入口(`config.c`):

| 配置 | 来源 | 说明 |
|---|---|---|
| `NVFAIR_MEMORY_LIMIT` / `NVFAIR_MEMORY_LIMIT_<n>` | 环境变量 | 显存上限,`1g/512m` 语法(解析逻辑独立可测) |
| `NVFAIR_SM_LIMIT` | 环境变量 | 算力百分比(二期启用) |
| `NVFAIR_SHARED_CACHE` | 环境变量 | 账本文件路径,默认 `/tmp/nvfair.cache` |
| `NVFAIR_LOG_LEVEL` | 环境变量 | 0-4,语义对齐 HAMi 的 LIBCUDA_LOG_LEVEL |
| 配置文件(可选) | `NVFAIR_CONFIG_FILE` 指向的 ini/文本 | 批量部署时用;inotify 热更新(二期) |

不做:二进制 packed struct 配置(ioctl-controller 最大技术债)、HTTP 客户端(编排层职责,不进库)。

### 4.5 关键风险与对策

| 风险 | 对策 |
|---|---|
| RM ioctl 结构随驱动版本变化 | `rm_defs.h` 只收最稳定子集;运行时读驱动版本但**按 ABI 能力探测**(sizeof/返回码)而非字符串匹配;不支持的版本降级为直通并打日志 |
| 分配路径不止 HEAP_CONTROL 一类 | 先枚举已知分配路径并白名单放行策略;未知分配 ioctl 记录日志(审计模式),不擅自放行/拒绝 |
| 绕过:应用直接 mmap 驱动映射内存 | 明确文档声明边界(同 HAMi);后续按需提供 CUDA API 层 hook 补充 |
| 账本文件被删/并发初始化 | magic + 版本号 + fcntl 文件锁 + owner pid 心跳,参照 HAMi-core hostpid_fallback_lock 的测试集设计 |

## 5. 开发计划(里程碑)

### M1 骨架与直通(MVP 前置)
- [ ] CMake 工程 + ctest 框架;`libnvfair.so` 编译产物
- [ ] ioctl hook 薄层:符号导出、RTLD_NEXT 解析、非 NVIDIA 请求直通
- [ ] 日志模块 + 环境变量配置解析(含单测)
- [ ] 验收:`LD_PRELOAD=./libnvfair.so nvidia-smi` 行为与不加载完全一致

### M2 显存限额(核心)
- [ ] `rm_defs.h`:NV_ESC_RM_VID_HEAP_CONTROL / RM_ALLOC / RM_FREE 结构定义
- [ ] 句柄表 + 进程内账本 + 预检拒绝(伪造 `NV_ERR_NO_MEMORY`)
- [ ] 共享内存账本(多进程聚合)+ 后台校准线程
- [ ] fake RM backend 单测:分配/释放/超限/并发竞态(参照 HAMi-core test/ 的测试风格)
- [ ] 验收:两个进程共享一卡,各自限额生效,`free` 后额度可回收

### M3 健壮性与可观测
- [ ] close hook 清理、崩溃进程账本回收(owner 心跳)
- [ ] 审计模式(只记录不拦截)与直通降级开关;审计日志的结构化 dump 代码从 ogkm 头生成(借用 cuda_ioctl_sniffer 的 pstruct 方法论,见 docs/05)
- [ ] 用 sniff.so 抓取 PyTorch 等真实负载的 ioctl 分配路径样本,沉淀为拦截清单的测试用例
- [ ] 驱动版本兼容矩阵测试(选定 2-3 个目标驱动版本)

### M4(二期,独立评估)
- [ ] 算力限额:改写 SET_TIMESLICE 或用户态令牌桶(需先用真机数据对比两条路线的行为差异再定)
- [ ] NVML hook:nvidia-smi 虚拟视图
- [ ] 配置文件热更新

## 6. 非目标(明确不做)

- 不做 K8s DevicePlugin / 调度器(上游编排层职责);
- 不做 HTTP/gRPC 客户端,不与外部服务通信;
- 不支持 WSL / Windows(一期仅 Linux x86_64);
- 不 hook CUDA API(一期;见第 2 节混合路线说明);
- 不追求对所有 NVIDIA 内核 API 的完备拦截,先覆盖显存分配主路径。

## 7. 开放问题(评审时确认)

1. 一期是否只做显存,算力延后?(本文档按此假设)
2. 账本文件粒度:每容器一个文件 vs 每节点一个文件按容器分区?(倾向前者,同 HAMi)
3. 配置是否引入纯文本配置文件(可选项),还是一期只做环境变量?
4. 句柄表是否需要跨进程共享(进程 fork 后共享 ctx 的场景)?
5. 目标驱动版本矩阵是哪些?(决定 rm_defs.h 子集范围)
