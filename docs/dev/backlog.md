# libnvfair 产品 Backlog(基础需求 / PBI / AC / 拓展功能)

> 状态:v0.1(基于 docs/research/01-05 调研与 dev/design.md 设计整理,待评审)
> 优先级定义:**P0** = MVP 必须没有它项目不成立;**P1** = 一期完整交付需要;**P2** = 二期;**P3** = 探索/长期演进
> 状态:待开发 / 进行中 / 已完成(以 2026-11-10 为准)

## 1. 基础需求(BR,项目成立的根基)

| # | 需求 | 说明 | 来源 |
|---|---|---|---|
| BR1 | 显存硬限额 | 拦截 NVIDIA RM 显存分配 ioctl,超配额向应用伪造 OOM,不触碰驱动 | design §1 |
| BR2 | 多进程共享一卡 | 同卡多进程各自配额独立生效、互不侵蚀;释放可回收 | design §4.3 |
| BR3 | 绝不破坏宿主 | 任何错误路径都降级为直通,劫持库内禁止 exit/abort | design §3.6 |
| BR4 | 零侵入 | 不改应用、不改驱动、不依赖 K8s;交付 .so + 头 + 测试,注入由使用方负责 | design §1/§3.5 |
| BR5 | 可维护性 | 单一事实源(ogkm 官方头)、模块 <800 行、状态收敛 nvfair_ctx、单测不依赖 GPU | design §3(对 ioctl-controller(research/03)技术债的正面回答) |
| BR6 | 热路径零开销 | 拦截路径不阻塞、不分配、不发网络请求;慢操作全部后台化 | design §3.3 |

## 2. P0:一期核心(MVP)

### PBI-001 ioctl hook 直通骨架 — **已完成**
薄层拦截,非 NVIDIA 请求原样透传。工程基建(CMake/构建/子模块)包含在内。
- AC1:`LD_PRELOAD=libnvfair.so nvidia-smi` 与不加载行为一致(exit code 相同 +
  显存数值行相同;GPU 温度/功耗/性能档为动态噪声,不纳入比对)
- AC2:非 `NV_IOCTL_MAGIC` 请求 100% 直通(代码路径评审确认)
- AC3:`ioctl` 符号 `GLOBAL DEFAULT` 导出;RTLD_NEXT 解析一次并缓存
- AC4:ogkm 570.211.01 子模块就位,CMake 探测缺失时给出明确修复指引
- 状态:2026-11-10 复验通过(重建后 readelf 确认 GLOBAL DEFAULT,preload nvidia-smi 直通 OK);
  符号同型坑(ioctl 带 `__THROW` = noexcept,声明必须完全同型否则符号退化为 LOCAL)已记录在
  src/ioctl_hook.cpp 头注释。
- 背景阅读:magic 直通判定 / GLOBAL DEFAULT / RTLD_NEXT 缓存详解 →
  [hook-background.md](hook-background.md)

### PBI-002 日志与配置解析 — **已完成**
- AC1:`NVFAIR_LOG_LEVEL` 0-4 生效,默认级别不干扰应用 stdout/stderr 语义(单测覆盖:
  合法 0-4、非法值拒绝并保持原级别、未设置不重置;已知行为 `' 3'` 前导空白被
  strtol 容忍,测试锁定现状)
- AC2:`NVFAIR_MEMORY_LIMIT`/`_LIMIT_<n>` 值语法,锚定 **K8s resource.Quantity 双进制后缀语义的整数子集**:
  - 非负十进制整数 + 单位后缀,**后缀是否带 `i` 决定进制**:
    - 不带 `i`(十进制 SI,1000 系):`k/K`=10^3、`m/M`=10^6、`g/G`=10^9,`1g`=1,000,000,000 字节;
    - 带 `i`(二进制 IEC,1024 系):`ki/kiB`、`mi/miB`、`gi/giB` = 2^10/2^20/2^30,`1gi`=1,073,741,824 字节;
      `i` 必须跟在单位字母后,`1i` 非法;
    - 单位字母后可选 `b/B` 尾缀(`1mb`=10^6、`1mib`=2^20);单独 `1b`/`1B` 非法(与无单位歧义);
  - **无单位后缀 = 字节**(`1073741824` ≡ `1gi`,≈1.07g);
  - 与 K8s 的差异(显式约定,不是疏漏):大小写不敏感;`m` 按兆(M-mega)解析而非 K8s 的毫(milli)——毫字节的配额无意义;
  - 不支持 K8s 的小数(`0.5Gi`)、`n/u` 前缀与 `T/P/E` 大单位,有真实需求再扩;
  - **对账提示**:nvidia-smi/CUDA 语境全是二进制,配 `24GiB` 应写 `24gi`;
    写 `24g` 得到 24e9 字节 ≈ 22.35GiB,差 ~5%;从 HAMi 配置迁移同理(`512m` 在 HAMi 是 2^29,此处是 512e6);
  - 非法即降级:空串、非数字开头、未知符号(`1t`/`1x`)、中间空白(`1 g`)、
    小数、溢出(≥2^64)一律按"该进程不限额"处理并记 WARN,绝不 exit/abort(BR3);
  - `0` = 显式不限额,记 INFO 与"未设置(静默,不限额)"区分;
  - 容忍首尾空白;
  - `_LIMIT_<n>` 存在即覆盖全局 `_LIMIT`,两者解析相互独立、同一解析函数
    (单卡覆盖语义,P3-5.3 才有消费者,002 只负责解析正确)。
- AC3:解析函数独立可单测(不依赖 GPU、不依赖 preload)
- AC4:日志函数绝不致命退出(禁 exit/abort,静态断言或评审约束)
- 依赖:PBI-001

### PBI-003 rm_defs.h 协议定义层 — **待开发(下一个)**
从 ogkm 官方头引出 libnvfair 使用的最稳定子集;版本差异唯一收纳处(偏移表模式)。
- AC1:NVOS32(VID_HEAP_CONTROL)/NVOS54(RM_FREE)/NVOS33(RM_CONTROL)/NVOS64(RM_ALLOC)定义齐备
- AC2:编译期 `static_assert(sizeof/offsetof)` 锚定 570.211.01 布局,漂移即编译失败
- AC3:NVOS64 的 status 偏移按 **dataSize 分支**(同内核 escape.c 判据:`size==sizeof(NVOS64)` vs `sizeof(NVOS21)`),不依赖驱动版本字符串
- AC4:hook 分发层(rm/ 以外)不出现任何字段偏移字面量(代码评审确认)
- 依赖:PBI-001

### PBI-004 分配预检 + OOM 伪造 — **待开发(核心)**
- AC1:fake backend 单测:配额内 → 放行真实 ioctl 并记账;超限 → 返回 `NV_ERR_NO_MEMORY`,真实 ioctl 零调用
- AC2:伪造状态码写入位置按 PBI-003 偏移表确定(NVOS64 双布局均覆盖)
- AC3:热路径无 malloc/锁竞争临界区在微秒级(评审 + 基准)
- AC4:多线程并发分配同一账本,原子记账无丢失(压测单测)
- 依赖:PBI-002、PBI-003

### PBI-005 句柄表与释放记账 — **待开发**
- AC1:`hObject → size` 加锁哈希表;RM_FREE 成功后查表记 −,查不到(非本进程分配)直通不动账
- AC2:close hook 清理该 fd 关联句柄;fd 复用不串账(句柄表随 ctx 生命周期)
- AC3:fake backend 单测:alloc→free→alloc 序列额度精确回归
- 依赖:PBI-004

### PBI-006 共享内存账本(多进程聚合)— **待开发(核心)**
- AC1:账本 mmap 文件(`NVFAIR_SHARED_CACHE`,默认 `/tmp/nvfair-<id>.cache`),magic + 版本号,字段全 `_Atomic`
- AC2:两进程共享一卡,各自限额独立生效;A 超限 OOM 时 B 不受影响
- AC3:进程 free 后额度可被另一等待分配的进程即时使用
- AC4:账本文件被删/损坏/并发初始化 → 重建或降级,绝不 crash 宿主(参照 HAMi hostpid_fallback_lock 测试集)
- 依赖:PBI-004

### PBI-007 后台校准线程 — **待开发**
- AC1:低频(秒级)用 `NV2080_CTRL_CMD_GPU_GET_PID_INFO` 反查驱动真实值,偏差超阈值修正账本
- AC2:kill -9 一个已分配进程后,其额度在秒级被回收可复用(集成测试)
- AC3:校准线程持锁时间有界,不阻塞热路径(代码评审)
- 依赖:PBI-006

### PBI-008 能力探测与降级直通 — **待开发**
- AC1:init 时探测驱动 ABI(sizeof 探针/返回码),不支持 → 全直通 + WARN 日志,功能关闭
- AC2:未知分配类 ioctl → 记审计日志(不擅自放行/拒绝,策略可配)
- 依赖:PBI-003

**P0 验收总景(design M2)**:两个进程共享一卡,各自限额生效,free 后额度可回收;全程 `nvidia-smi` 等非受管路径不受影响。

## 3. P1:一期完整交付(健壮性与可信度)

### PBI-101 审计模式 — **待开发**
只记录不拦截的全量 RM ioctl 结构化 dump(dump 代码从 ogkm 头生成,借 pstruct 方法论,见 research/05)。
- AC1:`NVFAIR_AUDIT=1` 时输出可读的分配/释放流水(字段名级别,非 hex dump)
- AC2:审计开关关闭时性能零损耗(分支可预测/编译期裁剪)

### PBI-102 真实负载拦截清单验证 — **待开发**
用 sniff.so(research/05)对 PyTorch 等负载抓包,沉淀为测试用例。
- AC1:至少 3 类真实负载(torch 训练、推理、nvidia-smi)的 ioctl 路径样本入库
- AC2:样本全部纳入 fake backend 回归,拦截清单与实际路径一致

### PBI-103 崩溃与泄漏兜底完善 — **待开发**
- AC1:owner pid 心跳失效检测 + 槽位回收(参照 HAMi postinit_owner_death 测试)
- AC2:容错注入单测(账本损坏/半写)全通过

### PBI-104 驱动兼容矩阵测试 — **待开发**
- AC1:CI 脚本对支持矩阵内每个 ogkm tag 检出并跑 sizeof 探针,漂移即报错
- AC2:矩阵内至少 2 个真实驱动版本真机冒烟(直通 + 限额)

## 4. P2:二期(独立评估后启动)

| PBI | 内容 | 关键 AC(初稿) |
|---|---|---|
| PBI-201 算力限额 | SET_TIMESLICE 改写 vs 用户态令牌桶,先用真机数据对比再定路线(design M4) | 容器平均 SM 利用率不超配额 ± 容差;不引入可感知延迟毛刺 |
| PBI-202 NVML hook 虚拟视图 | nvidia-smi 显示虚拟显存/配额 | 容器内 free = 配额 − 已用;宿主视图不受影响 |
| PBI-203 配置文件 + 热更新 | `NVFAIR_CONFIG_FILE` ini 文本 + inotify | 热更新生效无重启;非法配置拒绝并保留旧值 |
| PBI-204 cgroup 容器身份解析 | 供记账分区/上层集成 | v1/v2 cgroup 解析正确,失败降级 |

## 5. 拓展功能(P3:可长期演进的方向池)

> 不承诺、不排期;按需孵化。每项列出价值与前置条件。

### 5.1 混合路线:CUDA API 层 hook 补充
- **价值**:ioctl 层管"资源分配",CUDA API 层管"记账精度"(应用层 alloc/free 天然成对);design §2 已预留此决策。
- **触发条件**:实践发现 ioctl 记账精度不可接受(如 cudaMallocAsync 池化路径)。
- **形态**:复用同一 ledger/config 底座,新增 `cuda/` hook 模块(HAMi-core 的符号表方法论可直接借鉴)。

### 5.2 软限/硬限两级配额与弹性协商
- **价值**:ioctl-controller 的软限等待语义(gpu_memory 与 gpu_memory_limit 区间)在公平共享场景有用;更进一步可用尽后向上层管理器协商(HAMi 的 prememory HTTP 模式)。
- **前置**:热路径禁网络请求(BR6)→ 协商必须由上层写本地文件/共享内存下发,库内只读。

### 5.3 每 GPU UUID 独立配额
- **价值**:多卡机器上 `NVFAIR_MEMORY_LIMIT_<n>` 按卡分配(HAMi 已有语义),配合调度器做"卡级切分"。
- **前置**:账本从单卡扩展为按 uuid 分区(账本结构 v2)。

### 5.4 上游生态适配层
- **价值**:作为 HAMi 的 libvgpu.so 替代/备选,或被 gpu-manager 类组件集成;项目只交付 .so,适配层以独立小仓库/脚本形式存在。
- **形态**:device-plugin 注入脚本、共享区 Go 读取侧镜像(HAMi monitor v1 spec 模式)、K8s 示例。

### 5.5 观测工具链产品化
- **价值**:PBI-101 的审计模式反向输出为独立 sniff 工具(结构化 dump + 统计报表),服务驱动协议调试与新版本适配(每次驱动升级先用它 diff ioctl 流)。
- **形态**:pstruct 方法论从 ogkm 头生成 dump;支持 JSON 输出供分析。

### 5.6 MIG 感知
- **价值**:MIG 实例内配额分区,支持"物理卡 MIG 切分 + 软件配额"组合场景。
- **前置**:RM ioctl 里 MIG 实例句柄语义解析(需要 sniff 实测)。

### 5.7 覆盖面扩展
- **cudaMallocAsync/Graph 分配路径**:池化分配的记账正确性验证(用 5.5 工具抓真实路径后决定)。
- **aarch64 / Grace**:ioctl 层协议架构无关,主要工作在 CI 与测试矩阵。
- **多 libc / 静态链接场景**:musl 容器内 LD_PRELOAD 可用性验证。
- **fork 继承语义**:句柄表跨 fork 共享(design 开放问题 4,先实验后定)。

### 5.8 校准数据源演进
- **价值**:当前依赖 NV2080 控制命令反查;可演进为 eBPF/procfs 对照源,降低对 RM 控制路径的耦合。
- **触发**:某驱动版本 GET_PID_INFO 语义变化或开销过大时。

## 6. 排序总览

```
P0  001(✅)→ 002 → 003 → 004 → 005 → 006 → 007 → 008   [MVP: 显存限额端到端]
P1  101 → 102 → 103 → 104                               [一期交付质量]
P2  201 → 202 → 203 → 204                               [二期: 算力/视图/运维]
P3  501-508                                              [方向池, 按需孵化]
```

依赖关系:002→004;003→004→005;006 依赖 004;007/008 依赖 006/003;101 可与 004 并行(生成器独立)。
