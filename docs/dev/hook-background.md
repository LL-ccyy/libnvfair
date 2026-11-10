# libnvfair Hook 机制背景知识(前置阅读)

> 状态:v0.1(2026-11-10)
> 定位:PBI 评审与新成员上手前的共性背景,回答三个高频问题:
> ① `NV_IOCTL_MAGIC` 直通判定判的是什么、依据从哪来;
> ② `GLOBAL DEFAULT` 是什么、为什么 PBI-001 AC3 要验收它;
> ③ `RTLD_NEXT` 怎么解析、当前缓存策略与 HAMi 的差异。
> 事实源:ogkm 570.211.01 头文件(third_party/)、cuda_ioctl_sniffer sniff.cc(本地快照,实证 libcuda 真实流量)、HAMi-core 源码(本地快照)、src/ioctl_hook.cpp 现状。

## 1. NV_IOCTL_MAGIC:直通判定判的是什么

### 1.1 ioctl 的 request 参数是有编码结构的

Linux ioctl 的第二个参数 `request` 不是任意命令号,按 `_IOC` 宏打包成 32 位(x86_64 布局,asm-generic/ioctl.h):

```
 31  30 | 29 ... 16 | 15 ... 8 | 7 ... 0
 dir(2) | size(14)   | type(8)  | nr(8)
```

对应宏:`_IOC_TYPE(cmd) == (cmd >> 8) & 0xFF`、`_IOC_NR(cmd) == cmd & 0xFF`、`_IOC_SIZE(cmd) == (cmd >> 16) & 0x3FFF`。

### 1.2 NVIDIA 的约定:magic 'F' + 两族 nr

[_nv-ioctl-numbers.h](../../third_party/open-gpu-kernel-modules/kernel-open/common/inc/nv-ioctl-numbers.h):

```c
#define NV_IOCTL_MAGIC      'F'   /* 0x46 */
#define NV_IOCTL_BASE       200
```

发往 `/dev/nvidia*` 的全部 ioctl 都以 `'F'` 作 type 字节,nr 字节区分命令,分两族:

| 族 | nr 取值 | 例子 | 出处 |
|---|---|---|---|
| 基础控制 esc | 200(0xC8)+ n,n ≤ 55 | NV_ESC_CARD_INFO(200)、NV_ESC_REGISTER_FD(201)、NV_ESC_IOCTL_XFER_CMD(211) | nv-ioctl-numbers.h |
| RM escape | 0x27 ~ 0x5F | RM_ALLOC_MEMORY(0x27)、RM_FREE(0x29)、RM_CONTROL(0x2A)、RM_ALLOC(0x2B)、RM_VID_HEAP_CONTROL(0x4A) | nv_escape.h |

头文件注释 "ioctl() number > 55 will overflow" 即指:基础族 nr = 200 + n,超过 255 溢出 8-bit 字段。

### 1.3 回答:判的是哪个参数、来源是什么

- **判的是第二个参数 `request` 的 type 字节**,既不看 fd,也不解引用 argp。热路径友好:一次移位与一次比较,零开销。
- **判定依据是双重的**:① 内核 `_IOC` 编码约定(命令空间按 type 字节分片,不同驱动各占一个字母);② NVIDIA 选用 'F' 作为其驱动 ioctl 的 magic(官方开源树 nv-ioctl-numbers.h 定义)。
- **实证**:cuda_ioctl_sniffer sniff.cc:210-214 对真实 libcuda 流量用同一判据过滤(`type = (request >> 8) & 0xFF; if (type == NV_IOCTL_MAGIC)`),能完整分离出全部 RM 调用——证明 libcuda 实际流量确实全部携带 'F'。
- **内核侧反而不检查 magic**(nv.c 只取 `_IOC_NR(cmd)` 分发):因为 `/dev/nvidia*` 的 file_operations 只会收到自己设备的 ioctl。magic 字节的价值在**用户态**:劫持库拦的是进程全部 ioctl(网络/终端/磁盘/图形...),必须靠 type 字节把"不是发往 NVIDIA 驱动的调用"先摘出去。
- **直通(passthrough)语义**:type ≠ 'F' 的请求,原样转发真实 ioctl、原样回传返回值与 errno,不做任何观察或修改。这就是 PBI-001 AC2 的"非 NV_IOCTL_MAGIC 请求 100% 直通"。
- **两个已知边界**(PBI-003/PBI-008 落地时处理):
  1. magic 命中 ≠ 必须拦截,还需按 nr 分发(基础族/RM 族),未识别 nr 走"未知类"策略;
  2. 其他设备理论上也可能选 'F'(冲突概率低但非零);加固方向是 fd 归属校验(配合 open/close hook),一期可不做。

### 1.4 附:XFER 特例

参数结构大于 _IOC size 字段(14-bit)上限的 ioctl 走 `NV_ESC_IOCTL_XFER_CMD`(nv-ioctl.h 的 nv_ioctl_xfer_t):真实 cmd/size/ptr 打包传递,内核侧二次拷贝(nv.c:2371-2399)。它的外层 request 同样携带 'F',magic 判定不受影响;但 **PBI-003 分发层若要覆盖超大参数命令,须识别并解包这一层**。

## 2. GLOBAL DEFAULT:AC3 在验收什么

### 2.1 ELF 符号表的两个维度

`readelf -sW libnvfair.so` 每行格式:`Num: Value Size Type Bind Vis Ndx Name`。

| 维度 | 取值 | 含义 | 对劫持的意义 |
|---|---|---|---|
| Bind(绑定) | GLOBAL / LOCAL | GLOBAL 符号进入动态符号表,参与全局解析;LOCAL 仅本对象可见 | **LOCAL = 劫持死亡**:动态链接器根本看不到我们的 ioctl |
| Vis(可见性) | DEFAULT / PROTECTED / HIDDEN / INTERNAL | DEFAULT 允许符号被其他对象的同名定义抢占(interpose) | LD_PRELOAD 抢占机制正依赖 DEFAULT 语义 |

PBI-001 AC3 的验收输出即:`FUNC GLOBAL DEFAULT … ioctl`。

### 2.2 为什么会退化成 LOCAL:符号同型坑(已踩)

glibc 头声明 `extern int ioctl (int, unsigned long, ...) __THROW`;`__THROW` 展开为 noexcept。C++ 中异常说明是函数类型的一部分——若我们的定义与 glibc 声明不同型(漏掉 noexcept),符号会退化 LOCAL,劫持**静默失效**:不报错、不崩溃,只是"一切直通"(AC1 也照样通过,nvidia-smi 行为无差异)。所以 AC3 用 readelf 强制显式验收。src/ioctl_hook.cpp 头注释已固化此教训。

### 2.3 背后的机制:为什么 GLOBAL DEFAULT 才能劫持

动态链接器对未绑定符号按 **global scope** 顺序解析:可执行文件 → LD_PRELOAD 库(按注入顺序)→ 对象依赖链。libnvfair.so 的 ioctl 若是 GLOBAL DEFAULT,所有经 PLT 的 ioctl 调用都会绑到我们这里;libc 的真实 ioctl 再通过 `dlsym(RTLD_NEXT, "ioctl")` 取回——引出 §3。

## 3. RTLD_NEXT:真实符号怎么拿、怎么缓存

### 3.1 RTLD_NEXT 是什么

`RTLD_NEXT` 是一个**伪句柄**,含义:从**当前调用者之后**的 link-map 顺序查找下一个同名符号。preload 库用 `dlsym(RTLD_NEXT, "ioctl")` 即"找到排在 libnvfair 后面的那个 ioctl"(通常是 libc)——被覆盖的真实实现。每次 dlsym 有查找成本,所以解析结果(函数指针)必须缓存。

### 3.2 当前实现:已是惰性缓存,但有三个已知弱点

src/ioctl_hook.cpp:`if (real_ioctl == nullptr) real_ioctl = dlsym(RTLD_NEXT, "ioctl")`。首次调用解析、后续调用零 dlsym——AC3 说的"解析一次并缓存"即此,**并非每次调用都走 dlsym**。

弱点(后续收敛 hook_util 时处理,不阻塞当前阶段):

| # | 问题 | 建议 |
|---|---|---|
| 1 | 首调用多线程数据竞争(各线程解析结果相同、良性,但按 C++ 内存模型不严格) | pthread_once 或 relaxed atomic |
| 2 | 无法区分"未解析"与"解析结果为 NULL"→ dlsym 失败后每次调用重复失败路径 | 独立 done 标志 |
| 3 | dlsym 意外失败时调空指针段错误,违背 BR3(错误路径必须降级直通) | 兜底 `syscall(SYS_ioctl, ...)`(glibc ioctl 即薄 syscall 包装) |

另一处已知取舍:hook 用 `va_arg` 无条件取第三个参数;对无参数型 `_IO` 命令(调用方只传两个参数)会取出垃圾指针。内核侧按 `_IOC_SIZE=0` 不解引用,实际无害;sniffer/ioctl-controller/HAMi 均同此写法。记录在案,不修改。

### 3.3 对比:HAMi 怎么解析与缓存

先澄清直觉——**"HAMi 的 RTLD_NEXT 会有很多"不精确**:RTLD_NEXT 是常量伪句柄,不随符号数变多;变多的是"每个被 hook 符号各需一次 `dlsym(RTLD_NEXT, name)`"。HAMi hook 了 280 个 cu* + 548 个 nvml* 符号,量确实大,它的做法:

1. **批量一次**:preInit 时循环遍历 `cuda_library_entry[]` 逐符号解析(hook.c:268-285):优先 `real_dlsym(dlopen("libcuda.so.1"), name)`,失败回退 `real_dlsym(RTLD_NEXT, name)`,再失败按 `_v2/_v3` 旧版本名重试。结果全部存进 `cuda_library_entry[i].fn_ptr` 静态表,热路径只查表。
2. **它真正棘手的是 dlsym 自递归**:HAMi 连 `dlsym` 符号本身都 hook(libcudart 靠 dlsym 取 cu* 函数,要引导到 hook 实现)。preload 库自己再调 dlsym(RTLD_NEXT) 就会递归进自己的 hook → 用汇编跳板 `dlsym_entry.S` 分流:handle==RTLD_NEXT 直接尾跳真实 dlsym,其余进 `libvgpu_dlsym_dispatch`(libvgpu.c:97-121)。
3. **"真实 dlsym" 用 dlvsym 按版本取**(libvgpu.c:42-86):依次试 `dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"/"GLIBC_2.17"/...)`,全部失败再退 `dlopen("libc.so.6")`。

**libnvfair 为什么不需要这一套**:我们只 hook 一个符号(ioctl),不 hook dlsym,不存在自递归;无需汇编跳板与版本探测。当前"单指针惰性缓存"就是本问题的最小解,仅按 §3.2 补强即可。这也印证 design §3 原则 2:薄分发层,最小拦截面。

### 3.4 解析时机

当前在**首次被劫持的 ioctl 调用内**惰性解析(HAMi 在 preInit 批量解析)。也可移到库构造函数(dlsym RTLD_NEXT 在构造期合法,link map 已就位),好处是不占首调用延迟。两者皆可,是否改随 PBI-002 收敛 hook_util 时顺手定。

## 4. 概念 → Backlog 对照

| 概念 | 所在 AC | 本文 |
|---|---|---|
| 非 NV_IOCTL_MAGIC 请求 100% 直通 | PBI-001 AC2 | §1.3 |
| GLOBAL DEFAULT 导出验收 | PBI-001 AC3 | §2 |
| RTLD_NEXT 解析一次并缓存 | PBI-001 AC3 | §3.2 |
| nr 分发不出现偏移字面量 | PBI-003 AC4 | §1.2/§1.3 |
| 未知命令类策略 | PBI-008 AC2 | §1.3 边界 1 |
