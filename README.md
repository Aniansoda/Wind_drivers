# Wind Driver — Android ARM64 内核驱动 (wind.ko)

**中文** | [English](README.en.md) | [日本語](README.ja.md) | [Русский](README.ru.md) | [한국어](README.ko.md)

Android / Linux ARM64 内核态内存读写与调试框架
单模块 `wind.ko`，十一个子系统，一套操作码，三条通讯通道
零 CRC 路线，五个断点家族并存，全部白名单外符号运行时解析

---

## 作者与联系方式

| 项目 | 信息 |
|---|---|
| 作者 | 阿念-川上富江Tomie |
| 致谢 | 小黑 — JiagNight |
| 联系邮箱 | xuboyuan763@gmail.com |
| 问题反馈 | 优先提 issue，附内核版本、机型、`dmesg` 尾部 200 行 |

作者本人只有 `阿念-川上富江Tomie` 两个名号
本项目开源仅供学习交流，请勿商用或用于非法用途

---

## 支持范围

| 内核 | 编译目标 | 状态 |
|---|---|---|
| 5.10.198 | wind.ko | 已适配，零 CRC 路线 |
| 5.15.149 | wind.ko | 已适配，零 CRC 路线 |
| 6.1 | wind.ko | 已适配，零 CRC 路线 |
| 6.6.66 | wind.ko | 真机回归过，零 CRC 路线 |
| 6.12.58 | wind.ko | 真机 config 对齐，零 CRC 路线 |
| 6.18.21 | wind.ko | 已适配，零 CRC 路线 |

架构 aarch64 / arm64
形态 可加载内核模块，`insmod` 加载、`rmmod` 卸载
产物 单文件 `wind.ko`，全部子系统合并进一个模块，无依赖模块
源码 `kernel/` 共 8067 行，19 个 `.c` / `.h`

---

## 这一版改了什么

相对上一版，追踪面整体换过一轮

| 码位 | 旧 | 现 |
|---|---|---|
| `0x26..0x2B` | ETM/PTM 分支追踪 | **WRP 数据观察点** `wrpbp` |
| `0x38..0x3D` | 预留 | 已撤销，不再定义 |
| `0x33` | — | `OP_STATS` 缓存统计 |
| `0x34 / 0x37` | — | `HWBP_SET_REGS` / `HWBP_REFRESH` |
| `0x35 / 0x36` | — | `PTEBP_SET_REGS` / `PTEBP_REFRESH` |
| `0x3E / 0x3F` | — | `BP_DIAG` / `BP_ALL_OFF` |
| 陀螺仪 | 单个执行断点常驻 | 执行断点定位 → **转 WRP 稳态**，X/Y 改写，Z 不进窗口 |
| 符号 | 直接引用 | 32 个白名单外符号全部 `register_kprobe` 运行时解析 |
| 兼容 | `#ifdef` 散在代码里 | 统一 `compat.h`，5.10 → 6.18 一份源码 |

ETM 那条路已删除：Coresight 需要目标 SoC 打开时钟与 funnel，机型覆盖率不够，且观测不到数据本身
`0x26..0x2B` 六个码位改给 WRP，结构体大小与旧 `ETMBP_POINT_CONFIG` 同为 24 字节，老二进制不会错位

---

## 快速开始

```bash
# 1 编译（GitHub Actions 一键出 ko）
#    仓库 Actions → build-wind-ko → 选目标内核 → 下载 artifact

# 2 加载
su
insmod wind.ko

# 3 看日志（先确认这条，符号解析结果都打在这里）
dmesg | grep -i wind

# 4 卸载
rmmod wind
```

编译产物路径 `kernel-src/drivers/wind/wind.ko`

加载后第一屏日志应能看到

```
wind: sym use_mm=ffffffffxxxxxxxx/ffffffffxxxxxxxx
wind: sym vmap=... filp=... hwbp=... misc=...
wind: netlink unit=31
```

已预编译 `user/Wind-Drivers-6.6.ko`，六个版本的自解压安装包在 `user/Wind-Drivers-*.sh`

---

## 通讯通道

驱动同时实现三条通道，共用同一套操作码与同一分发入口 `wind_dispatch_ioctl`
加载时按 `transport` 参数与探测结果择路

| transport | 通道 | 原理 | 说明 |
|---|---|---|---|
| 3 | netlink 私有协议 | 注册私有 `AF_NETLINK` 协议号，用户态 `socket(AF_NETLINK, SOCK_RAW, 31)` 发 `WIND_NL_MSG` | 默认首选，`/dev` 与 `/proc` 下无节点，不写任何内核对象 |
| 2 | 私有 misc 字符设备 | `misc_register` 注册字符设备 | 不碰 `/dev/null` 不碰 rodata，兼容性最好，代价是多一个设备节点 |
| 1 | `/dev/null` fops 重定向 | 把 `inode->i_fop` 指向模块内副本 | 兜底路径，需 `allow_fops_hook=1` 才允许，部分机型有卡死重启记录 |

netlink 常量

```
协议号默认 31         WIND_NL_DEFAULT_UNIT
消息魔数 0x574E4C31   WIND_NL_MAGIC  ("WNL1")
操作码基线 0x574E0000 WIND_OP_BASE
接收白名单           WIND_OP_BASE + 0x01 .. WIND_OP_BASE + 0x3F
```

白名单外的 op 一律丢弃不回包，避免任何意外输入被 dispatch 当成合法 cmd 深解引用

### 模块参数

| 参数 | 类型 | 默认 | 作用 |
|---|---|---|---|
| `transport` | int | 3 | `3`=netlink，`2`=misc 字符设备，`1`=/dev/null i_fop |
| `install_hook` | bool | 1 | 加载时是否立即安装通道 hook，`0`=只解析符号不动任何对象 |
| `allow_fops_hook` | bool | 0 | 是否允许回退到改 `/dev/null` 的 `inode->i_fop` |
| `hook_level` | int | — | 二分定位用 `0`=off，`1`=只重定向 fops，`2`=重定向并接管 ioctl/mmap，`3`=保留原 `.open` |

```bash
insmod wind.ko transport=3 install_hook=1
insmod wind.ko transport=0            # 只加载，不装通道，用于排查
```

---

## 操作码总表

高 16 位固定 `0x574E`（ASCII "WN"），低 16 位为序号
不与通用 `0x800` 系列示例驱动撞车

### 会话与内存

| 码 | 名称 | 参数 | 功能 |
|---|---|---|---|
| 0x01 | `OP_BIND` | pid | 绑定目标进程，缓存 `task_struct` |
| 0x02 | `OP_UNBIND` | 0 | 解绑并释放缓存 |
| 0x03 | `OP_READ_MEM` | `COPY_MEMORY*` | 单次读 |
| 0x04 | `OP_WRITE_MEM` | `COPY_MEMORY*` | 单次写 |
| 0x05 | `OP_READ_BATCH` | `PBATCH_REQUEST` | 批量读，一次 syscall 读多个离散地址 |
| 0x06 | `OP_WRITE_BATCH` | `PBATCH_REQUEST` | 批量写 |
| 0x07 | `OP_SET_MODE` | `MODE_REQUEST*` | 切换读取模式 FAST / SAFE |
| 0x08 | `OP_MODULE_BASE` | `MODULE_BASE*` | 取目标进程内某模块加载基址 |
| 0x33 | `OP_STATS` | `wind_map_stats*` | 读页映射缓存与翻译缓存统计 |

### 隐藏

| 码 | 名称 | 功能 |
|---|---|---|
| 0x09 | `OP_HIDE_FILE` | 按 basename 隐藏文件与目录项，最多 16 条 |
| 0x0A | `OP_HIDE_PROCESS` | 隐藏进程目录项，最多 64 个 pid |
| 0x0B | `OP_UNHIDE_ALL` | 撤销全部隐藏 |
| 0x0E | `OP_KGSL_HIDE` | 在 kgsl 侧隐藏指定 pid |
| 0x0F | `OP_KGSL_HOOK` | 安装 kgsl hook |

### 硬件断点 BRP/WRP

| 码 | 名称 | 功能 |
|---|---|---|
| 0x0C | `OP_BRK_SET` | 旧版单点硬件断点，兼容接口 |
| 0x0D | `OP_BRK_CLEAR` | 清除旧版单点断点 |
| 0x14 | `OP_HWBP_SET` | 设置断点/观察点组，16 槽 |
| 0x15 | `OP_HWBP_REMOVE` | 清除全部并归还 debug 寄存器 |
| 0x16 | `OP_HWBP_GET_CONFIG` | 读当前配置与可用 BRP/WRP 数量 |
| 0x17 | `OP_HWBP_GET_RECORDS` | 分段读触发 PC 聚合记录 |
| 0x18 | `OP_HWBP_SET_RECORD` | 写回记录与 2-bit 寄存器改写掩码 |
| 0x19 | `OP_HWBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |
| 0x34 | `OP_HWBP_SET_REGS` | 命中时改写寄存器（`s3`/`s4`/`s5`），`.inst` 原始编码 |
| 0x37 | `OP_HWBP_REFRESH` | 重新登记断点，用于 exec 后地址漂移 |

### PTE 影子页执行断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x1A | `OP_PTEBP_SET` | 改目标进程 PTE 属性触发 fault，16 槽，不占 debug 寄存器 |
| 0x1B | `OP_PTEBP_REMOVE` | 清除全部并恢复 PTE |
| 0x1C | `OP_PTEBP_GET_CONFIG` | 读当前配置 |
| 0x1D | `OP_PTEBP_GET_RECORDS` | 分段读命中记录 |
| 0x1E | `OP_PTEBP_SET_RECORD` | 写回记录 |
| 0x1F | `OP_PTEBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |
| 0x35 | `OP_PTEBP_SET_REGS` | 命中时改写寄存器，走影子页跳板 |
| 0x36 | `OP_PTEBP_REFRESH` | 影子页与 PTE 重新对齐 |

### BRK 软断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x20 | `OP_BRKBP_SET` | 把目标指令改写为 `BRK`，16 槽 |
| 0x21 | `OP_BRKBP_REMOVE` | 清除全部并还原原始指令字节 |
| 0x22 | `OP_BRKBP_GET_CONFIG` | 读当前配置 |
| 0x23 | `OP_BRKBP_GET_RECORDS` | 分段读命中记录 |
| 0x24 | `OP_BRKBP_SET_RECORD` | 写回记录 |
| 0x25 | `OP_BRKBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### WRP 数据观察点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x26 | `OP_WRPBP_SET` | 在目标 VA 上装写观察点，`LEN_8` 窗口 |
| 0x27 | `OP_WRPBP_REMOVE` | 拆除并归还 WRP 寄存器 |
| 0x28 | `OP_WRPBP_GET_CONFIG` | 读当前窗口配置 |
| 0x29 | `OP_WRPBP_GET_RECORDS` | 分段读命中记录 |
| 0x2A | `OP_WRPBP_SET_VEC` | 写回向量与 2-bit 掩码（X/Y 分别取位） |
| 0x2B | `OP_WRPBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### UDP 断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x2E | `OP_UDPBP_SET` | 装/更新一个 UDP 断点槽，8 槽 |
| 0x2F | `OP_UDPBP_REMOVE` | 拆槽，`-1` 拆全部 |
| 0x30 | `OP_UDPBP_GET_CONFIG` | 读槽位表 |
| 0x31 | `OP_UDPBP_GET_RECORDS` | 分段读命中记录，每槽 256 条 |
| 0x32 | `OP_UDPBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### 陀螺仪

| 码 | 名称 | 功能 |
|---|---|---|
| 0x10 | `OP_GYRO_INSTALL` | 安装陀螺仪 hook，参数为目标 pid |
| 0x11 | `OP_GYRO_REMOVE` | 卸载 hook |
| 0x12 | `OP_GYRO_SET` | 设置目标角速度 x/y/z（rad/s，IEEE 754 位模式） |
| 0x13 | `OP_GYRO_ENABLE` | 开关篡改 |
| 0x2C | `OP_GYRO_STOP` | 暂停篡改，保留 hook |
| 0x2D | `OP_GYRO_RESUME` | 恢复篡改，断点丢失时自动重绑 |

### 诊断与总闸

| 码 | 名称 | 功能 |
|---|---|---|
| 0x3E | `OP_BP_DIAG` | 一次性读五个断点家族的槽位与命中计数 |
| 0x3F | `OP_BP_ALL_OFF` | 一把关掉全部断点，切换追踪方式前先调它 |

`0x38..0x3D` 已撤销，不要在客户端里占位

---

## 功能详解

### 1 会话绑定

`open` 之后调一次 `OP_BIND` 把 `task_struct` 引用缓存到 `driver_session`
之后每次读写不再走 `find_get_pid` + `get_pid_task`，省掉一次 pid 哈希查找与两次引用计数操作
`close` 时 `session_destroy` 释放引用
`mm` 不缓存，每次 `get_task_mm` 现取，规避 `exec` 换 `mm` 后的野指针

### 2 内存读写

**单次读写** 一次 ioctl 读写一段连续虚拟地址，跨页时逐页翻译

**批量读写** 一次 ioctl 传 N 个离散地址，N 次点读合并成 1 次 syscall
批量内部只取一次 `mm`，把 N 次 `mmgrab`/`mmput` 压到 1 次，多线程高频读时明显降 CPU 与锁竞争

**mmap 零拷贝** 把目标进程物理页 `remap_pfn_range` 映射进调用者地址空间
之后直接指针读写，零 ioctl，适合大块采样

**双模式读取**

| 模式 | 实现 | 优点 | 缺点 |
|---|---|---|---|
| `MODE_FAST` | `phys_to_virt` 直读 + `memcpy` | 高性能、低发热 | 可能被 cache 侧信道探测 |
| `MODE_SAFE` | `ioremap_nocache` + `__flush_dcache_area` | 无缓存，防侧信道 | 慢、发热 |

**页映射缓存** 读物理页时复用已有映射，带命中/淘汰统计，`OP_STATS` 可读

**VA→PA 翻译缓存** 同一虚拟地址反复读时省去页表遍历，统计项 `tr_hits` / `tr_misses`

**swap 兜底** 遇到 `!pte_present`（swap entry 或未映射）时用 `get_user_pages_remote(FOLL_GET)` 做读 fault 换入后重译
用 `FOLL_GET` 而非 `FOLL_WRITE`，避免对只读映射触发 COW 副作用

**跨页正确性** 按 `PAGE_SIZE - (addr & (PAGE_SIZE-1))` 分段，每段单独翻译，不在页边界上读错物理页

**缓存别名修复** 写目标进程内存后 GPU / ION 缓冲区读到旧值会导致黑屏
普通 RAM 走线性映射直读（与进程页表同为 WB cache，ARM64 PIPT 物理索引天然一致），写后 `__flush_dcache_area` 回写主存

### 3 模块基址

`OP_MODULE_BASE` 传模块名，遍历目标进程 `mm` 的 `vm_area_struct` 找对应 ELF 的加载基址
6.1 之前手写链表遍历，6.1 之后走 maple tree VMA 迭代器，`compat.h` 里用 `KH_HAVE_VMA_ITER` 分流
`get_module_base` 亦供内核内部调用

### 4 隐藏

**文件隐藏** 钩 `filldir64`（回退 `filldir`），在 `pre_handler` 把目标目录项名字改成 `.` 且 `namlen=1`，目录列表里不再出现原名
按 basename 匹配，最多 16 条

**进程隐藏** 同一 hook 里按纯数字 PID 匹配目录项，最多 64 个

**kgsl 隐藏** 在 GPU kgsl 侧隐藏指定 pid，配合 `kgsl_hook_install` 使用

**全撤** `OP_UNHIDE_ALL` 一次清空文件与进程隐藏表

隐藏为什么用 kprobe Android GKI 5.x+ 带 kCFI 与只读 rodata，直接改函数头会触发 CFI 校验崩溃或写保护异常
kprobe 走内核断点机制注册回调，不写代码段，天然绕过 CFI

符号延迟注册 首次 `hide_*` 时才按候选表（`filldir64` → `filldir`）注册，符号不可见也不会导致加载失败

### 5 五个断点家族

同一套「槽位 + 记录环形缓冲 + 分段读回」骨架，共用 `0x3E` 诊断与 `0x3F` 总闸
执行断点之间互斥要自己排，因为 arm64 的 BRP 只有 4 个、WRP 只有 2 个

| 家族 | 机制 | 占用 | 记录里能拿到 | 用途 |
|---|---|---|---|---|
| `hwbp` | BRP 执行断点 / WRP 观察点，perf 事件 | 4 BRP + 2 WRP | 触发 PC 与寄存器 | 函数入口、读写点 |
| `ptebp` | PTE 属性位 + 影子页 32 字节跳板 | 不占 debug 寄存器 | 命中点与可控寄存器 | 想同时盯十几个点 |
| `wrpbp` | WRP 数据观察点，`LEN_8` 窗口 | 2 WRP | 写入时刻与向量 | 盯单个字段的改写 |
| `brkbp` | 指令改 `BRK` + kprobe 异常接管 | 不占硬件 | 命中 PC 与通用寄存器 | 记录型观测 |
| `udpbp` | kprobe UDP 收发路径 | 不占硬件 | 包内容预览 | 观测网络节奏 |

**硬件断点 `hwbp`** arm64 上执行断点只支持 `HW_BREAKPOINT_LEN_4`
命中后按配置记录触发 PC，分段读回，可写回记录并带 2-bit 寄存器改写掩码
`OP_HWBP_SET_REGS` 在命中回调里直接写 `s3`/`s4`/`s5`

**PTE 影子页断点 `ptebp`** 改目标进程 PTE 属性位触发 fault，16 槽，每槽最多覆盖 8 页
不占用 debug 寄存器，因此可与 `hwbp` 同时使用
原指令被搬到跳板里执行，跳板在目标 `mm` 里现开一页 RWX
支持直接读写 PTE 位域（`PTE_UXN` / `PTE_PXN` / `PTE_RDONLY` / `PTE_VALID` / `PTE_AF`），改完刷 TLB

**WRP 观察点 `wrpbp`** `arch_install_hw_breakpoint` 的 `LEN_8` 数据窗口
命中时读 `perf_event` 里的值写进记录，`OP_WRPBP_SET_VEC` 带 2-bit 掩码回写
陀螺仪的稳态就架在这一层

**BRK 软断点 `brkbp`** 把目标指令改写为 `BRK`，通过异常 handler 接管，16 槽
命中时用内核态跨进程读写 `wind_read_process_va` / `wind_write_process_va` 取参数与改返回值，不经过 `__user`
当前只做记录，不做寄存器回写

**UDP 断点 `udpbp`** 在内核 UDP 收发路径上打 kprobe

```
TX  udp_send_skb  →  ip_send_skb  →  ip_local_out   按顺序取第一个能解析到的
RX  __udp4_lib_rcv
```

TX 首选 `udp_send_skb`，因为 GKI 的 LTO 构建会把 `ip_local_out` 内联
按 pid、端口、方向过滤，命中写入记录环形缓冲，可分段读回
8 槽，每槽 256 条记录，单条预览 32 字节
仅 IPv4，IPv6 的 `__udp6_lib_rcv` 未接

**旧版 brk** `OP_BRK_SET` / `OP_BRK_CLEAR` 用 `register_user_hw_breakpoint` 实现单点执行断点，保留兼容

**FP 寄存器改写** 内核用 `-mgeneral-regs-only` 编译，clang 集成汇编器拒收 `stp q3,q4,[x9]` 这类助记符
`hwbp_fp_reg_direct` 改用 `.inst` 写原始编码，基址固定 `x9`

| 指令 | 编码 |
|---|---|
| `stp q3, q4, [x9]` | `0xad001123` |
| `str q5, [x9]` | `0x3d800125` |
| `ldp q3, q4, [x9]` | `0xad401123` |
| `ldr q5, [x9]` | `0x3dc00125` |

### 6 陀螺仪

控制方式不是用户态 `/dev/uinput` 注入，而是内核态 hook 系统库的 BitTube 出口

```
目标符号 android::BitTube::sendObjects(sp<BitTube> const&, void const*, size_t, size_t)
候选模块 libsensor.so → libsensorservice.so → libui.so
```

定位流程 `filp_open` + `kernel_read` 读目标 ELF，解析 `.dynsym` 取符号偏移，加上目标进程里对应模块的加载基址得到运行时地址
`sendObjects` 参数（arm64）`x1` = events 数组、`x2` = count，`x0` 是 tube，不参与数据定位

两阶段追法

1. **定位阶段** 在 `sendObjects` 上挂执行断点，第一次命中时把 events 缓冲区地址学下来
2. **稳态阶段** 立刻摘掉执行断点，改成 `LEN_8` 的写观察点盯住 `data[0..1]`

稳态只剩一个 WRP，system_server 里不再有执行断点，命中频率从「每次 sensor 批量」降到「每次改写」
从自己的 overflow handler 里注销 perf 事件会死锁，所以切换动作放在 `gyro_set_velocity`（进程上下文，每帧一次）

事件布局（`sensors_event_t`，0x68 字节）

```
0x00 version | 0x04 sensor | 0x08 type | 0x10 timestamp
0x18 float data[16] | 0x58 flags
```

type 认 `SENSOR_TYPE_GYROSCOPE`(4) 与 `SENSOR_TYPE_GYROSCOPE_UNCALIBRATED`(16)
两个的 x/y/z 都在 `data[0..2]`，改写方式一致
NDK 的 `ASensorEvent` 把 type 挪到 `0x04`，所以代码先试 `0x08` 再试 `0x04`

**Z 轴不动是结构性的**，不是靠参数控制：执行断点写 8 字节在 `+0x18`，WRP 窗口 `LEN_8` 覆盖 `data[0..1]`，客户端掩码 `0x3`
`+0x20`（z）从头到尾没进过任何写窗口

WRP 递归安全 自己的 `copy_to_user_nofault` 会再次触发观察点
用 `user_mode(regs)` 加 per-CPU `gyro_wrp_busy` / `wrpbp_busy` 双重挡

`gyro_stop` / `gyro_resume` 只是暂停与恢复篡改，hook 不断开
`gyro_remove` 才真正卸载
目标进程退出时 `gyro_resume` 返回 `-ESRCH`，未安装时返回 `-ENODEV`，均为幂等

### 7 影子页与内核写保护

**影子页 PTE 操作** `wind_shadow_pte_set` / `wind_shadow_pte_get`
遍历目标 `mm` 页表定位 PTE，按 `(old & ~mask) | (val & mask)` 改写并刷 TLB，用于无痕断点与页权限微调
物理地址字段用 `KH_PTE_PA_MASK` 取，新 PTE 用 `KH_PHYS_TO_PTE` 拼
52 位 PA 的机器上物理地址高 2 位在 PTE 的 `[9:8]`，直接 `| page_to_phys()` 会写坏页表项

**内核镜像写保护绕过** `wind_force_kernel_page_rw` / `wind_kernel_page_ro_state`
`set_memory_rw` 不接受内核镜像地址，这里自行走页表把镜像页改成可写
`ro_state` 只读探测 校验整条页表路径后读叶子 AP[2]，不写页表即可判定只读状态

---

## 符号策略：零 CRC + 运行时解析

两条腿走路，缺一条都装不上真机

### 零 CRC

编译完成后把 `__versions` / `__version_ext_crcs` / `__version_ext_names` 三个段长度清零
内核 `check_version()` 因此整段跳过符号 CRC 比对，只留一次 `pr_warn_once`
同时 `__versions` 段存在使 `same_magic(has_crcs=1)` 只比较 vermagic 第一个空格之后的尾巴 `SMP preempt mod_unload modversions aarch64`

| 内核 | 文件 | 版本表为空时的分支 | `same_magic` 第三参 |
|---|---|---|---|
| 5.10.198 | `kernel/module.c` | `Broken toolchain` → `return 1` | `info->index.vers` |
| 5.15.149 | `kernel/module.c` | 同上 | 同上 |
| 6.1 | `kernel/module/version.c` | 同上 | 同上 |
| 6.6.66 | `kernel/module/version.c` | 同上 | 同上 |
| 6.12.58 | `kernel/module/version.c` | 同上，另有 ext 表分支 | 同上 |
| 6.18.21 | `kernel/module/version.c` | 同上，另有 ext 表分支 | 同上 |

适配边界 这条路省掉的是「符号 CRC 必须逐台一致」，不是 vermagic
尾巴（`SMP preempt mod_unload modversions aarch64`）仍要与目标内核逐字一致
所以同一个 ko 能覆盖同系列、同配置特征的不同机型与不同 `LOCALVERSION` 前缀
跨系列（6.1 的 ko 装到 6.12）仍会被 `bad vermagic` 拒掉
前提仍是 config 与真机同构，否则 CFI 类型哈希与结构体布局不符会加载成功然后崩

真机 vermagic

```
6.12.58-android16-6-g7704a1ae279b-ab15213644-4k SMP preempt mod_unload modversions aarch64
```

### 运行时符号解析

GKI 冻结 KMI，vendor 模块只能引用白名单交集里的符号
本驱动用到的 32 个函数不在交集里，一路直接引用就会

```
wind: Unknown symbol kthread_use_mm (err -2)
```

`err -2` 是 ENOENT，导出表里根本没有这个名字，`insmod` 直接失败
所以这些函数统一改走 `sym.c`：`register_kprobe`（白名单内）按符号名取地址，编译期不产生 CRC 引用

```
mm/进程   mmput  kthread_use_mm  kthread_unuse_mm  remap_pfn_range
vmap      __get_vm_area_caller  free_vm_area  vunmap  ioremap_page_range  iounmap
页表      set_memory_rw  set_memory_ro  contpte_ptep_get  __arch_clear_user
文件      filp_open  kernel_read  file_path
设备      misc_register  misc_deregister
netlink   netlink_kernel_create  __netlink_kernel_create  netlink_kernel_release
          __netlink_kernel_release  netlink_unicast  __alloc_skb
断点      register_user_hw_breakpoint  unregister_hw_breakpoint
          register_wide_hw_breakpoint  unregister_wide_hw_breakpoint
浮点      fpsimd_preserve_current_state
nofault   copy_from_user_nofault  copy_to_user_nofault
```

解析结果在加载时打进 dmesg，解析失败的会打 `wind: ... 未解析到`
对应能力降级但不影响加载

### 兼容层

`compat.h` 一处收口，5.10 到 6.18 一份源码

| 项 | 分界 | 处理 |
|---|---|---|
| `mmap_lock` | 5.8 | `mmap_read_lock` ↔ `down_read(&mm->mmap_sem)` |
| `kmap_local_page` / `kunmap_local` | 5.11 | 退到 `kmap_atomic` / `kunmap_atomic` |
| `ptep_get` | 5.10 | 无此函数时直接 `*(ptep)` |
| maple tree VMA 迭代 | 6.1 | `KH_HAVE_VMA_ITER` |
| `get_user_pages_remote` 元数 | 5.9 / 6.5 | 三个分支 |
| `__flush_dcache_area` | 5.11 | `dc civac` 内联序列 |
| `arm64_use_ng_mappings` | 6.1 / 6.6 / 6.12 | 逐版本取值 |
| `PTE_ADDR_MASK` | 5.11 删除 | `KH_PTE_PA_MASK` 四级收敛 |
| 物理地址进 PTE | 52 位 PA | `KH_PHYS_TO_PTE` 按位形拼 |
| 版本头 | 6.10 删除 `linux/version.h` | 引 `generated/uapi/linux/version.h` |

---

## 用户态对接

| 文件 | 作用 |
|---|---|
| `user/Wind-对接头文件.h` | 一个 `c_wind` 类包含全部能力：读写 / 批量 / 帧缓存 / 快照 / 隐藏 / kgsl / 陀螺仪 / 五套断点 |
| `user/Wind-drivers-netlink_v1.3.0.zip` | netlink 通道客户端示例 |
| `user/Wind-Drivers-*.sh` | 各内核版本自解压安装包（5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18） |
| `user/Wind-Drivers-6.6.ko` | 6.6 预编译模块 |
| `user/1.sh` | 装包脚本 |

客户端最小流程

```cpp
#include "wind.hpp"

c_wind &d = c_wind::instance(target_pid);   // 默认 netlink unit 31
int v = d.read<int>(base + 0x10);
d.write<int>(base + 0x10, 123);

d.frame_begin();                            // 一帧内同地址只读一次
uint64_t hp = 0;
d.frame_read(base + 0x100, hp);

d.snap_start(base + 0x200, 60);             // 后台 60Hz 刷新
uint32_t cur = d.snap_value<uint32_t>();
d.snap_stop();
```

`sizeof(c_wind)` 约 48 KB，其中 47 KB 是帧缓存表（2048 槽）
局部实例建议 `new` 到堆上

切换追踪方式前先 `bp_all_off()`，否则上一套断点占着 BRP/WRP，新的一套装不上

```cpp
d.bp_all_off();        // 0x3F，一把关掉五个家族
```

---

## 编译

### 方式 A — GitHub Actions（推荐）

```
Actions → build-wind-ko → Run workflow → 选内核 → 下载 artifact
```

当前矩阵六行，**全部走零 CRC 路线**（`zero_versions: "1"`）

```
android12-5.10.198
android13-5.15.149
android14-6.1
android15-6.6.66
android16-6.12.58-device      真机 config 对齐
android17-6.18.21
```

### 方式 B — 本地交叉编译

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LLVM=1 LLVM_IAS=1
make -j"$(nproc)" M=kernel modules
```

工具链要求 clang 14 及以上（5.x 需要无前缀 LLVM 工具），或 aarch64-linux-gnu-gcc
内核用 `-mgeneral-regs-only` 编译，裸汇编里的 FP/SIMD 必须写成 `.inst`，助记符形式只有 gcc 会放过

### 辅助工具

| 工具 | 用途 |
|---|---|
| `tools/dump_kcrc.py` | `dump` / `harvest` / `merge` / `check` / `diff` / `count` 内核 CRC 表操作 |
| `tools/fix_versions.py` | 按 symvers 表重写 `__versions` 里的 CRC |
| `tools/fix_vermagic.py` | 把 `.modinfo` 移到文件尾并写入目标 vermagic |
| `tools/extract_installer_ko.py` | 从自解压安装包 `.sh` 里抠出内置 ko 与 CRC 表 |
| `tools/zero_versions.py` | 零 CRC 路线 版本段长度清零（`-n` 只看不改，就地改自动留 `.bak`） |
| `tools/harvest_kcrc.sh` | 从真机 vendor 模块批量采集 CRC 表 |
| `tools/windbench.cpp` | 读写通道压测与发热对比 |
| `tools/btf_offsets.py` | 从 BTF 取结构体偏移 |
| `tools/pad_info.sh` / `pad_pack.sh` / `pad_probe.sh` | 机型信息采集、打包、探测 |
| `tools/diag_lockup.sh` | 死机现场采集 |
| `extract_symvers.py` | 从 ko 里抽 symvers |

---

## 目录结构

```
Wind_driver/
├── kernel/                 内核模块源码（8067 行）
│   ├── entry.c             加载入口、通道安装、ioctl 分发
│   ├── memory.c            内存读写、页表翻译、页映射缓存、mmap
│   ├── process.c           模块基址、进程相关
│   ├── hide.c              文件与进程隐藏（kprobe）
│   ├── breakpoint.c        五套断点框架 + UDP 断点 + 影子页
│   ├── gyro.c              陀螺仪 hook（exec bp → WRP 稳态）
│   ├── kgsl.c              GPU kgsl hook
│   ├── nlcomm.c            netlink 私有通道与 op 白名单
│   ├── sym.c               32 个白名单外符号的运行时解析
│   ├── comm.h              操作码与协议结构
│   ├── compat.h            内核版本兼容层（5.10 → 6.18）
│   ├── memory.h / breakpoint.h / gyro.h / nlcomm.h / process.h / kgsl.h / sym.h
│   ├── Kconfig
│   └── Makefile
├── user/                   用户态对接头文件、客户端、安装包
├── tools/                  编译与真机辅助脚本
├── .github/workflows/      CI 交叉编译
├── CHANGES.md              逐轮改动与真机实证记录
├── CHANGELOG.md            早期里程碑
├── HOOKPTE-对照.md          hook 实现对照
├── README.md               中文（本文件）
├── README.en.md / README.ja.md / README.ru.md / README.ko.md
└── LICENSE
```

---

## 排错

| 现象 | 先看 |
|---|---|
| 加载被拒 `Unknown symbol ... (err -2)` | 该符号没导出，改走 `sym.c` 运行时解析 |
| 加载被拒 `disagrees about version of symbol` | CRC 与真机不符，走零 CRC 路线或重编 |
| 加载被拒 `version magic ... should be` | vermagic 与真机不一致 |
| `insmod` 后无任何通道 | `dmesg` 找 `transport`、`misc_register`、`netlink` |
| 打开通道后卡死重启 | `transport=2`，不要用 `allow_fops_hook=1` |
| 读到的数据是旧值 | 切 `MODE_SAFE`，检查目标是否为 GPU / ION 缓冲 |
| 隐藏无效 | `CONFIG_KALLSYMS_ALL` 是否开启，`filldir64` 是否可见 |
| 陀螺仪不动 | 目标进程是否为读 sensor 的那个，`sendObjects` 是否解析到，`dmesg` 里 WRP 是否 armed |
| 断点装不上 | 先 `bp_all_off`，再看 `OP_BP_DIAG` 里 BRP/WRP 余量 |
| 影子页命中后目标跑飞 | 检查 `KH_PTE_PA_MASK` / `KH_PHYS_TO_PTE` 与真机 PA 位宽是否匹配 |

关键日志关键字

```
wind:
wind: misc_register
wind: netlink
wind: sym use_mm
filldir64
gyro
hwbp / ptebp / wrpbp / brkbp / udpbp
bp: all off
```

---

## 已知限制

- 隐藏功能依赖目标内核符号可见性，`CONFIG_KALLSYMS_ALL` 未开时 `filldir64` 查不到
- `/dev/null` fops 通道在部分机型上有卡死重启记录，默认不启用
- 零 CRC 路线只跳过符号 CRC，不跳过 vermagic 尾巴比对，也不跳过 CFI 与结构体布局校验
- 零 CRC 不跨内核系列生效，6.1 的 ko 不能装到 6.12，尾巴不一致会被 `bad vermagic` 拒掉
- `brkbp` 当前只做记录，不做寄存器回写
- `hwbp` / `ptebp` 尚未按 TID 限定，命中是进程级的
- arm64 执行断点只支持 `LEN_4`，指令必须 4 字节对齐
- BRP 4 个、WRP 2 个是硬件上限，五套断点同时开会互相挤占
- `ptebp` 每槽最多覆盖 8 页，翻页后要 `OP_PTEBP_REFRESH`
- UDP 断点只覆盖 IPv4，IPv6 收发路径未接
- 6.12.58 之前的产物依赖 `tools/device/` 里的设备 config 与 CRC 表，换机型需重新采集

---

## 版本历史

最近八轮（第五十轮 → 第五十七轮）

| 轮次 | 内容 |
|---|---|
| 五十 | Keep 客户端把新能力全部接上 |
| 五十一 | 切换追踪方式时的 stack corruption |
| 五十二 | `entry.c` dispatch 整段重复 + 三处编译/逻辑缺陷 |
| 五十三 | `breakpoint.c` 编译错误与缺失符号 |
| 五十四 | arm64 内核不让碰浮点寄存器，改 `.inst` 原始编码 |
| 五十五 | 5.10 / 6.12 / 6.18 三套内核的移植差异（`kmap_local_page`、`PTE_ADDR_MASK`） |
| 五十六 | 6.10 之后没有 `include/linux/version.h`，改引生成头 |
| 五十七 | `kthread_use_mm` / `kthread_unuse_mm` 未导出，改运行时解析 |

详细逐轮改动、真机证据与未完成项见 `CHANGES.md`
性能与发热对比见 `tools/windbench.cpp`
