# Wind Driver — Android ARM64 内核驱动 (wind.ko)

Android / Linux ARM64 内核态内存读写与调试框架
单模块 `wind.ko`，九个子系统，一套操作码，三条通讯通道

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
| 5.10 | wind.ko | 已适配，零 CRC 路线 |
| 5.15 | wind.ko | 已适配，零 CRC 路线 |
| 6.1 | wind.ko | 已适配，零 CRC 路线 |
| 6.6 | wind.ko | 已适配，真机回归过，零 CRC 路线 |
| 6.12.58 | wind.ko | 真机 config 对齐，零 CRC 路线 |
| 6.18 | wind.ko | 已适配，零 CRC 路线 |

架构 aarch64 / arm64
形态 可加载内核模块，`insmod` 加载、`rmmod` 卸载
产物 单文件 `wind.ko`，全部子系统合并进一个模块，无依赖模块

---

## 快速开始

```bash
# 1 编译（GitHub Actions 一键出 ko）
#    仓库 Actions → build-wind-ko → 选目标内核 → 下载 artifact

# 2 加载
su
insmod wind.ko

# 3 卸载
rmmod wind

# 4 看日志
dmesg | grep -i wind
```

编译产物路径 `kernel-src/drivers/wind/wind.ko`

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
协议号默认 31        WIND_NL_DEFAULT_UNIT
消息魔数 0x574E4C31  WIND_NL_MAGIC  ("WNL1")
```

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
| 0x01 | `OP_BIND` | pid | 绑定目标进程，缓存 `task_struct` 与 `mm` |
| 0x02 | `OP_UNBIND` | 0 | 解绑并释放缓存 |
| 0x03 | `OP_READ_MEM` | `COPY_MEMORY*` | 单次读 |
| 0x04 | `OP_WRITE_MEM` | `COPY_MEMORY*` | 单次写 |
| 0x05 | `OP_READ_BATCH` | `PBATCH_REQUEST` | 批量读，一次 ioctl 读多个离散地址 |
| 0x06 | `OP_WRITE_BATCH` | `PBATCH_REQUEST` | 批量写 |
| 0x07 | `OP_SET_MODE` | `MODE_REQUEST*` | 切换读取模式 FAST / SAFE |
| 0x08 | `OP_MODULE_BASE` | `MODULE_BASE*` | 取目标进程内某模块加载基址 |
| 0x33 | `OP_STATS` | `wind_map_stats*` | 读页映射缓存与翻译缓存统计 |

### 隐藏

| 码 | 名称 | 参数 | 功能 |
|---|---|---|---|
| 0x09 | `OP_HIDE_FILE` | 路径字符串 | 按 basename 隐藏文件与目录项 |
| 0x0A | `OP_HIDE_PROCESS` | pid | 隐藏进程目录项 |
| 0x0B | `OP_UNHIDE_ALL` | 0 | 撤销全部隐藏 |
| 0x0E | `OP_KGSL_HIDE` | pid | 在 kgsl 侧隐藏指定 pid |
| 0x0F | `OP_KGSL_HOOK` | 0 | 安装 kgsl hook |

### 硬件断点 BRP/WRP

| 码 | 名称 | 功能 |
|---|---|---|
| 0x0C | `OP_BRK_SET` | 旧版单点硬件断点，兼容接口 |
| 0x0D | `OP_BRK_CLEAR` | 清除旧版单点断点 |
| 0x14 | `OP_HWBP_SET` | 设置断点/观察点组，最多 16 点 |
| 0x15 | `OP_HWBP_REMOVE` | 清除全部并归还 debug 寄存器 |
| 0x16 | `OP_HWBP_GET_CONFIG` | 读当前配置与可用 BRP/WRP 数量 |
| 0x17 | `OP_HWBP_GET_RECORDS` | 分段读触发 PC 聚合记录 |
| 0x18 | `OP_HWBP_SET_RECORD` | 写回记录与 2-bit 寄存器改写掩码 |
| 0x19 | `OP_HWBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |
| 0x34 | `OP_HWBP_SET_REGS` | 命中时改写 FP 寄存器（v3/v4/v5 语义） |

### PTE 影子页执行断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x1A | `OP_PTEBP_SET` | 改目标进程 PTE 属性触发 fault，最多 16 点，不占用 debug 寄存器 |
| 0x1B | `OP_PTEBP_REMOVE` | 清除全部并恢复 PTE |
| 0x1C | `OP_PTEBP_GET_CONFIG` | 读当前配置 |
| 0x1D | `OP_PTEBP_GET_RECORDS` | 分段读命中记录 |
| 0x1E | `OP_PTEBP_SET_RECORD` | 写回记录 |
| 0x1F | `OP_PTEBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### BRK 软断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x20 | `OP_BRKBP_SET` | 把目标指令改写为 `BRK`，最多 16 点 |
| 0x21 | `OP_BRKBP_REMOVE` | 清除全部并还原原始指令字节 |
| 0x22 | `OP_BRKBP_GET_CONFIG` | 读当前配置 |
| 0x23 | `OP_BRKBP_GET_RECORDS` | 分段读命中记录 |
| 0x24 | `OP_BRKBP_SET_RECORD` | 写回记录 |
| 0x25 | `OP_BRKBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### ETM/PTM 分支追踪断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x26 | `OP_ETMBP_SET` | Coresight 硬件级执行流观测 |
| 0x27 | `OP_ETMBP_REMOVE` | 拆除并关闭追踪 |
| 0x28 | `OP_ETMBP_GET_CONFIG` | 读当前配置 |
| 0x29 | `OP_ETMBP_GET_RECORDS` | 分段读追踪记录 |
| 0x2A | `OP_ETMBP_SET_RECORD` | 写回记录 |
| 0x2B | `OP_ETMBP_CLEAR_RECORDS` | 清记录，`-1` 清全部 |

### UDP 断点

| 码 | 名称 | 功能 |
|---|---|---|
| 0x2E | `OP_UDPBP_SET` | 装/更新一个 UDP 断点槽 |
| 0x2F | `OP_UDPBP_REMOVE` | 拆槽，`-1` 拆全部 |
| 0x30 | `OP_UDPBP_GET_CONFIG` | 读槽位表 |
| 0x31 | `OP_UDPBP_GET_RECORDS` | 分段读命中记录 |
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
现在普通 RAM 走线性映射直读（与进程页表同为 WB cache，ARM64 PIPT 物理索引天然一致），写后 `__flush_dcache_area` 回写主存

### 3 模块基址

`OP_MODULE_BASE` 传模块名，遍历目标进程 `mm` 的 `vm_area_struct` 找对应 ELF 的加载基址
`get_module_base` 亦供内核内部调用

### 4 隐藏

**文件隐藏** 钩 `filldir64`（回退 `filldir`），在 `pre_handler` 把目标目录项名字改成 `.` 且 `namlen=1`，目录列表里不再出现原名
按 basename 匹配，最多 16 条

**进程隐藏** 同一 hook 里按纯数字 PID 匹配目录项，最多 64 条

**kgsl 隐藏** 在 GPU kgsl 侧隐藏指定 pid，配合 `kgsl_hook_install` 使用

**全撤** `OP_UNHIDE_ALL` 一次清空文件与进程隐藏表

隐藏为什么用 kprobe Android GKI 5.x+ 带 kCFI 与只读 rodata，直接改函数头会触发 CFI 校验崩溃或写保护异常
kprobe 走内核断点机制注册回调，不写代码段，天然绕过 CFI

符号延迟注册 首次 `hide_*` 时才按候选表（`filldir64` → `filldir`）注册，符号不可见也不会导致加载失败

### 5 断点框架

**硬件断点 `hwbp`** 用 arm64 BRP/WRP debug 寄存器，最多 16 点，支持执行断点与数据观察点
命中后按配置记录触发 PC，分段读回，可写回记录并带 2-bit 寄存器改写掩码

**PTE 影子页断点 `ptebp`** 改目标进程 PTE 属性位触发 fault，最多 16 点
不占用 debug 寄存器，因此可与 `hwbp` 同时使用
支持直接读写 PTE 位域（`PTE_UXN` / `PTE_PXN` / `PTE_RDONLY` / `PTE_VALID` / `PTE_AF`），改完刷 TLB

**BRK 软断点 `brkbp`** 把目标指令首字节改写为 `BRK`，通过异常 handler 接管，最多 16 点
命中时用内核态跨进程读写 `wind_read_process_va` / `wind_write_process_va` 取参数与改返回值，不经过 `__user`

**ETM/PTM 断点 `etmbp`** 走 Coresight 硬件追踪，做全局执行流观测，不修改目标

**UDP 断点 `udpbp`** 在内核 UDP 收发路径上打 kprobe
```
TX  udp_send_skb  →  ip_send_skb  →  ip_local_out   按顺序取第一个能解析到的
RX  __udp4_lib_rcv
```
TX 首选 `udp_send_skb`，因为 GKI 的 LTO 构建会把 `ip_local_out` 内联
按 pid、端口、方向过滤，命中写入记录环形缓冲，可分段读回
仅 IPv4，IPv6 的 `__udp6_lib_rcv` 未接
用于观测目标进程的 UDP 收发内容与节奏

**旧版 brk** `OP_BRK_SET` / `OP_BRK_CLEAR` 用 `register_user_hw_breakpoint` 实现单点执行断点，保留兼容

**FP 寄存器改写** `OP_HWBP_SET_REGS` 在命中回调里改写浮点寄存器，对齐 v3/v4/v5 语义

### 6 陀螺仪

控制方式不是用户态 `/dev/uinput` 注入，而是内核态 hook 系统库

```
目标文件 /system/lib64/libsensor.so
目标符号 _ZN7android7BitTube11sendObjectsERKNS_2spIS0_EEPKvmm
```

定位流程 `filp_open` + `kernel_read` 读目标 ELF，解析 `.dynsym` 取符号偏移，加上目标进程里 `libsensor.so` 的加载基址得到运行时地址
hook 方式 `register_user_hw_breakpoint` 执行断点（uprobe 语义）

在 `sensors_event_t` 经 BitTube 传输时拦截并篡改 `SENSOR_TYPE_GYROSCOPE` 事件的 x/y/z 角速度
角速度以 IEEE 754 位模式传入，单位 rad/s

`gyro_stop` / `gyro_resume` 只是暂停与恢复篡改，hook 不断开
`gyro_remove` 才真正卸载
目标进程退出时 `gyro_resume` 返回 `-ESRCH`，未安装时返回 `-ENODEV`，均为幂等

### 7 影子页与内核写保护

**影子页 PTE 操作** `wind_shadow_pte_set` / `wind_shadow_pte_get`
遍历目标 `mm` 页表定位 PTE，按 `(old & ~mask) | (val & mask)` 改写并刷 TLB，用于无痕断点与页权限微调

**内核镜像写保护绕过** `wind_force_kernel_page_rw` / `wind_kernel_page_ro_state`
`set_memory_rw` 不接受内核镜像地址，这里自行走页表把镜像页改成可写
`ro_state` 只读探测 校验整条页表路径后读叶子 AP[2]，不写页表即可判定只读状态

---

## 用户态对接

| 文件 | 作用 |
|---|---|
| `user/Wind-对接头文件.h` | 全部结构体、操作码、netlink 消息定义，客户端直接包含即可 |
| `user/1.sh` | 装包脚本 |
| `user/Wind-drivers-netlink_v1.2.1.zip` | netlink 通道客户端示例 |
| `user/Wind-Drivers-*.sh` | 各内核版本自解压安装包（5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18） |
| `user/Wind-Drivers-6.6.ko` | 6.6 预编译模块 |

netlink 客户端最小流程
```c
int fd = socket(AF_NETLINK, SOCK_RAW, 31);
struct _WIND_NL_MSG msg = { .magic = 0x574E4C31u, .cmd = OP_BIND, .arg = pid };
send(fd, &msg, sizeof(msg), 0);
recv(fd, &msg, sizeof(msg), 0);      /* msg.result 为返回值 */
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
android15-6.6
android16-6.12.58-device     真机 config 对齐
android17-6.18.21
```

零 CRC 路线做法 编译完成后把 `__versions` / `__version_ext_crcs` / `__version_ext_names` 三个段长度清零
内核 `check_version()` 因此整段跳过符号 CRC 比对，只留一次 `pr_warn_once`
同时 `__versions` 段存在使 `same_magic(has_crcs=1)` 只比较 vermagic 第一个空格之后的尾巴 `SMP preempt mod_unload modversions aarch64`
逐内核核对过的依据（源码级）

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

### 方式 B — 本地交叉编译

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LLVM=1 LLVM_IAS=1
make -j"$(nproc)" M=kernel modules
```

工具链要求 clang 14 及以上（5.x 需要无前缀 LLVM 工具），或 aarch64-linux-gnu-gcc

### 辅助工具

| 工具 | 用途 |
|---|---|
| `tools/dump_kcrc.py` | `dump` / `harvest` / `merge` / `check` / `diff` / `count` 内核 CRC 表操作 |
| `tools/fix_versions.py` | 按 symvers 表重写 `__versions` 里的 CRC |
| `tools/fix_vermagic.py` | 把 `.modinfo` 移到文件尾并写入目标 vermagic |
| `tools/extract_installer_ko.py` | 从自解压安装包 `.sh` 里抠出内置 ko 与 CRC 表 |
| `tools/zero_versions.py` | 零 CRC 路线 版本段长度清零（`-n` 只看不改，就地改自动留 `.bak`） |
| `fix_nocrc.sh`（`windfix/`） | 手机端纯 sh 版，无 python 也能清零版本段 |
| `tools/harvest_kcrc.sh` | 从真机 vendor 模块批量采集 CRC 表 |
| `tools/windbench.cpp` | 读写通道压测与发热对比 |
| `tools/pad_*.sh` | 机型信息采集、打包、探测 |
| `tools/btf_offsets.py` | 从 BTF 取结构体偏移 |

---

## 目录结构

```
Wind_driver/
├── kernel/                 内核模块源码
│   ├── entry.c             加载入口、通道安装、ioctl 分发
│   ├── memory.c            内存读写、页表翻译、页映射缓存、mmap
│   ├── process.c           模块基址、进程相关
│   ├── hide.c              文件与进程隐藏（kprobe）
│   ├── breakpoint.c        五套断点框架
│   ├── gyro.c              陀螺仪 hook
│   ├── kgsl.c              GPU kgsl hook
│   ├── nlcomm.c            netlink 私有通道
│   ├── sym.c               符号解析与 kprobe 注册
│   ├── comm.h              操作码与协议结构
│   ├── compat.h            内核版本兼容层
│   ├── memory.h            会话、页表翻译、影子页、批量读写声明
│   ├── breakpoint.h        五套断点与 UDP 断点声明
│   ├── gyro.h / nlcomm.h / process.h / kgsl.h / sym.h
│   ├── Kconfig
│   └── Makefile
├── user/                   用户态对接头文件、客户端、安装包
├── tools/                  编译与真机辅助脚本
├── .github/workflows/      CI 交叉编译
├── CHANGES.md              逐轮改动与真机实证记录
├── README.md
└── LICENSE
```

---

## 排错

| 现象 | 先看 |
|---|---|
| 加载被拒 `disagrees about version of symbol` | CRC 与真机不符，走零 CRC 路线或重编 |
| 加载被拒 `version magic ... should be` | vermagic 与真机不一致 |
| `insmod` 后无任何通道 | `dmesg` 找 `transport`、`misc_register`、netlink 协议号 |
| 打开通道后卡死重启 | `transport=2`，不要用 `allow_fops_hook=1` |
| 读到的数据是旧值 | 切 `MODE_SAFE`，检查目标是否为 GPU / ION 缓冲 |
| 隐藏无效 | `CONFIG_KALLSYMS_ALL` 是否开启，`filldir64` 是否可见 |
| 陀螺仪不动 | 目标进程是否为读 sensor 的那个，`libsensor.so` 符号是否可解析 |

关键日志关键字
```
wind:
wind: misc_register
wind: netlink
filldir64
gyro
hwbp / ptebp / brkbp / etmbp / udpbp
```

---

## 已知限制

- 隐藏功能依赖目标内核符号可见性，`CONFIG_KALLSYMS_ALL` 未开时 `filldir64` 查不到
- `/dev/null` fops 通道在部分机型上有卡死重启记录，默认不启用
- ETM/PTM 需要目标 SoC 打开 Coresight 时钟与 funnel，部分机型不可用
- 零 CRC 路线只跳过符号 CRC，不跳过 vermagic 尾巴比对，也不跳过 CFI 与结构体布局校验
- 零 CRC 不跨内核系列生效，6.1 的 ko 不能装到 6.12，尾巴不一致会被 `bad vermagic` 拒掉
- UDP 断点只覆盖 IPv4，IPv6 收发路径未接
- 6.12.58 之前的产物依赖 `tools/device/` 里的设备 config 与 CRC 表，换机型需重新采集

---

## 版本历史

详细逐轮改动、真机证据与未完成项见 `CHANGES.md`
性能与发热对比见 `tools/windbench.cpp`
