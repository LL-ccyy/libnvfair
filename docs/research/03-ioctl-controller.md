# ioctl-controller 设计文档(闭源原型调研)

> 来源:某闭源仓库的 ioctl-controller 模块(2026-11 快照)。该模块是 TKEStack vcuda-controller(见 01-vcuda-controller.md)的二次开发产物,属"hook libc ioctl"路线,与 libnvfair 定位一致,但实现停留在原型演化状态,本文档同时整理其机制与技术债。**注:本文档已脱敏,闭源仓库路径与产品名以泛称代替。**

## 1. 定位

在容器内通过 `LD_PRELOAD=/usr/local/nvidia/lib64/libioctl.so` 劫持 libc 的 `ioctl()` 符号,拦截发往 `/dev/nvidia*` 的 NVIDIA RM(Resource Manager)ioctl,实现:

- 显存分配限额(硬拒绝 / 软等待 / 向 gpu-manager HTTP 申请弹性额度);
- 算力限制(改写驱动的 SET_TIMESLICE 时间片参数);
- 进程级显存统计(hijack_stat 变体,已不在默认构建)。

## 2. 整体结构

```
gpu-resource-manager/          # 下游主仓库,以下仅列相关部分
├── ioctl-controller/          # 本次调研主体,C/C++ LD_PRELOAD 库
│   ├── ioctl/
│   │   ├── hijack_ioctl.cc        # 3247 行,核心劫持逻辑
│   │   ├── hijack_stat.cc         # 3684 行,统计版(未构建)
│   │   ├── hijack_ioctl-bak.cc    # 3449 行,手工备份(技术债)
│   │   ├── nvml-subset.h/nvml-monitor.h  # NVML 结构子集(沿袭自 vcuda-controller)
│   │   └── include/open-gpu-kernel-modules/  # 内嵌 NVIDIA 驱动头文件
│   ├── httpclient/
│   │   ├── vcuda_http_client.c    # 434 行,手写 socket HTTP/1.0 客户端
│   │   ├── cJSON.c/h              # vendored JSON 库
│   │   └── httphjack.cc           # 528 行,独立测试工具
│   ├── lib/                       # 提交入库的二进制 .so(技术债)
│   └── CMakeLists.txt + Dockerfile + vcuda.spec
├── gpu-manager/               # Go 侧:K8s DevicePlugin + HTTP/gRPC 服务
│   └── pkg/services/virtual-manager/  # 生成/热更新配置、prememory 接口
├── gpu-manager-wsl/           # WSL 变体
└── (另有容器内应用显存预留 C++ SDK,与本文档主线无关)
```

产物:`libioctl.so`(劫持库)+ `libvcuda-gpu-client.so`(HTTP 客户端),RPM 打包装到 `/usr/local/nvidia/lib64/`。

## 3. 核心机制

### 3.1 hook 入口

`hijack_ioctl.cc:1670-1687`:`dlsym(RTLD_NEXT,"ioctl")` 取真实 ioctl,导出同名符号覆盖。

伪代码:

```
ioctl(fd, req, argp):
  if (req >> 8 & 0xFF) != NV_IOCTL_MAGIC: return origin(...)   # 非 NVIDIA 直通
  pthread_once(initialization)                                 # 读配置+HTTP注册
  switch (req & 0xFF 即 nr):
    NV_ESC_RM_VID_HEAP_CONTROL:  # 显存分配
        if flags 命中白名单:     # 6 组硬编码 flag 组合
            ret = gpu_memory_limiter_v3(size)   # 限额判定,可阻塞
            if ret != NV_OK: 伪造 status=0x51(OUT_OF_MEMORY) 返回 0
    NV_ESC_RM_ALLOC:             # 记录 hRoot/hObjectNew 句柄(fds→map)
    NV_ESC_RM_CONTROL:           # 拦截 SET_TIMESLICE,改写时间片
  return origin(...)
```

### 3.2 显存限制(`gpu_memory_limiter_v3`,单函数约 390 行)

**不自记账**,每次分配前实时向驱动查询:

1. `get_fb_info`:发 `NV2080_CTRL_CMD_FB_GET_INFO` 取全局显存 free/total;
2. `get_pid_info`:`NV2080_CTRL_CMD_GPU_GET_PIDS` 取卡上所有 PID,再批量 `NV2080_CTRL_CMD_GPU_GET_PID_INFO` 取每进程显存;
3. 本容器 PID 集来自 `pids.config` 文件,bsearch 匹配后累加得 `used_memory`;
4. 与配置 `gpu_memory`(预留)/`gpu_memory_limit`(上限)比较:超硬限直接伪造 OOM;软限区间循环等待(每轮睡 2.5s)或 HTTP 询问 gpu-manager `/prememory` 接口。

为降查询开销有三重阈值短路:累计分配 <256M 或次数 <500 或单次 <64M 时跳过查询直接放行。

### 3.3 算力限制

拦截 `NVA06C_CTRL_CMD_SET_TIMESLICE`,按公式 `timeslice = offset + scale * (vcore - 1)` 改写时间片参数,先 SET 再 GET 验证。offset/scale 按驱动版本硬编码两套(470 → 240/8,其他 → 1920/256),另留 `vcore_timeslice.txt` 旁路文件。配置热更新时 inotify 线程主动对所有已记录 fd 重设。

### 3.4 配置通道(三条并存)

| 通道 | 内容 | 读写方 |
|---|---|---|
| 二进制文件 `/etc/vcuda/<cid>/vcuda.config` | packed struct 原样写盘,inotify 热更新 | gpu-manager CGO 写 / 劫持库读 |
| 环境变量 | `GPU_NODE_IP/PORT`、`LOGGER_LEVEL`、`GPU_CHECK` 等 | device-plugin Allocate 注入 |
| HTTP | `http://$GPU_NODE_IP:$PORT/gpu-manager/prememory?...` 配额协商、注册 | 手写 HTTP 客户端 |

初始化时从 `/proc/self/cgroup`(失败回退 mountinfo)解析 podUid/containerId,HTTP 注册到 gpu-manager。

## 4. 与 gpu-manager 的协同

```
kubelet Allocate → gpu-manager:
  写 /etc/vcuda/<cid>/vcuda.config + pids.config,挂进容器
  注入 LD_PRELOAD、GPU_NODE_IP/PORT
容器内首个 NVIDIA ioctl → 劫持库初始化:
  cgroup 解析 → HTTP 注册 → 本地限额预检 → 边界处 HTTP 弹性申请
```

gpu-manager 侧 prememory 路由族约 16 个接口(httpserver.go:76-94)。

## 5. 设计问题与技术债(“不好维护”的具体证据)

1. **巨型单文件 + 海量注释代码**:`hijack_ioctl.cc` 3247 行,约 800+ 行是注释掉的旧代码;另提交 3449 行 `-bak.cc` 备份文件。
2. **跨语言三处重复定义同一结构体**:`resource_data_t` 在 C++ 侧与 Go 侧 `cfunutils.h` 各定义一遍,靠 packed/aligned 二进制对齐,字段 `occupied[4044]` 纯魔法占位;Go 侧还有第三条 CSV 文本写入路径,与二进制读取不兼容,属半成品死代码。
3. **魔法数密集**:request 值 `3223340586` 重复定义 4 次;`STATUS_OUT_OF_MEMORY 0x51`、`MAX_PIDS 1024`、6 组硬编码 alloc flags 白名单(实际绑定特定 CUDA 版本行为,新驱动需手工试凑)。
4. **驱动兼容靠字符串匹配**:`strstr(driver_version, "470")` 散布多处;470 专属结构体手抄一份;`get_pid_info` 470/非 470 两分支近乎复制粘贴。
5. **测试脚手架污染生产**:`_IOCTL_TEST` 宏贯穿核心文件,9 个测试状态用 `setenv` 跨进程传递,且在 ioctl 热路径上加锁。
6. **危险实现**:把服务端返回的 msg 当 printf 格式串(格式化字符串漏洞);`LOGGER(FATAL)` 直接 `exit(-1)` 会杀死宿主业务进程;fd 为键的全局 map 无清理(fd 复用串句柄)、多数访问无锁。
7. **重复代码**:cgroup 解析在两处各一份;`hijack_stat.cc` 与主文件框架重复;`CMakeLists.txt` 大量注释掉的死目标。
8. **状态分散**:配置读取散在 `pthread_once 初始化 + inotify 线程 + limiter 加锁快照` 三处;十几个文件级全局变量构成隐式状态机。
9. **二进制产物入库 + 手写基础设施**:`lib/` 下 .so 直接提交 git;手写 HTTP/1.0 客户端(无 TLS、无超时、strncpy 解析状态行)。

## 6. 关键文件清单

| 路径 | 行数 | 说明 |
|---|---|---|
| ioctl-controller/ioctl/hijack_ioctl.cc | 3247 | 核心:LD_PRELOAD ioctl 劫持、显存限制、时间片改写 |
| ioctl-controller/ioctl/hijack_stat.cc | 3684 | 统计版(未构建) |
| ioctl-controller/httpclient/vcuda_http_client.c | 434 | 手写 HTTP 客户端 |
| gpu-manager/.../allocator/nvidia/allocator.go | 8545 | Allocate 注入 LD_PRELOAD/环境变量(:1539-1548) |
| gpu-manager/.../virtual-manager/manager.go | 1255 | 生成/热更新 vcuda.config、写 pids.config |
| gpu-manager/.../virtual-manager/httpserver.go | ~600 | prememory 路由与注册(:76-183) |
| gpu-manager/pkg/utils/cfunutils.go/.h | 320/171 | vcuda.config 二进制结构读写(与 C++ 侧重复) |

## 7. 对 libnvfair 的启示

**ioctl 路线的固有成本**(libnvfair 若坚持 hook ioctl 必须面对):

- 必须自己解析 `NV_ESC_*` 命令码与参数结构,需内嵌 NVIDIA 驱动头文件,跟随驱动演进;
- 无法从 ioctl 参数可靠区分"本次分配归属哪个逻辑配额",只能反查驱动统计,热路径昂贵 → 才引出 256M/500 次/64M 三重短路这种妥协;
- 算力限制只能借驱动 timeslice 参数,不同驱动代际参数/行为不同。

**反面教材清单**(libnvfair 必须避免):巨型单文件、注释代码与 -bak 文件入库、跨语言手工对齐结构体、驱动版本字符串匹配、测试宏污染生产、格式串漏洞、无锁全局状态、二进制产物入库、手写 HTTP 客户端。

**可借鉴**:LD_PRELOAD 注入点选择(libc ioctl 符号)、`/etc/vcuda` 目录式配置 + inotify 热更新、cgroup 解析拿 pod/container 身份、软限等待 + 硬限拒绝的分级拒绝策略。
