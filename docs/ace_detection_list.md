# ACE (TenProtect) 检测面总表 —— 和平精英 com.tencent.tmgp.pubgmhd

证据来源：设备实测 so / 字符串 / 动态行为。强度：**强**=直接字符串或动态验证，**中**=间接推断，**弱**=常规范式。

## A 进程与线程面

| ID | 检测点 | ACE 实现证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| A1 | 外挂进程可见性 | `mvm_bk_proc`/`mvm_bk_task`、ckguard `/proc/%u/cmdline`+`getPackageName` | 遍历 `/proc/*/cmdline` 比对黑名单/包名 | `hide_process()` 走 `filldir64` | **强/已有** |
| A2 | 启动链 | `/proc/<pid>/stat` ppid | 父进程非 zygote 则可疑 | 需伪造 stat 输出 | 中/**已实现** |
| A3 | 线程名 | `/proc/<pid>/task/*/comm` | 找 `gum-js-loop`、`gmain` 等 | 需过滤 comm | 中/**已实现** |
| A4 | 进程数量异常 | 遍历 /proc 计数 | 统计频次 | 同 A1 | 弱/已有 |

## B 内存面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| B1 | 注入 so 可见 | 常规 + `.got.plt` 串 | `/proc/self/maps` | `show_map_vma` 过滤 | 强/新写 |
| B2 | 匿名可执行段 | 同上 | maps 里 `rwxp` 且无文件 | 同 B1 扩展 | 强/新写 |
| B3 | memfd/ashmem 加载 | 同上 | maps 中 `/memfd:` | 同 B1 | 中/新写 |
| B4 | 双映射页表 | page table walk | 同 VA 两物理页 | clonebp `bailout_max` 下调 | 强/已有 |
| B5 | 自身不可 dump | **动态验证** `/proc/<pid>/mem` → I/O error | `prctl(PR_GET_DUMPABLE)` 复检 | 保持 0，改回反被发现 | 强/无需 |

## C 调试器面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| C1 | TracerPid | `debugger=%s`、`ptrace` | `/proc/self/status` | `proc_pid_status` 临时清 `task->ptrace` | 强/新写 |
| C2 | ptrace 自附加 | `ptrace` | `ptrace(ATTACH,self)` 成功=无调试器 | 保持默认 | 强/无需 |
| C3 | 硬件断点占用 | — | `PR_GET_HWBP`/调试寄存器读 | clonebp 不占 BRP | 中/已有 |
| C4 | 断点指令扫描 | — | 比对 `.text` 找 `BRK`/`0xcc` | clonebp 不改原页字节 | 中/已有 |

## D 内核面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| D1 | 模块列表 | 常规 | `/proc/modules`、`/sys/module/wind` | `m_show` 过滤 + 链表摘除 | 强/新写 |
| D2 | 内核符号 | 常规 | `/proc/kallsyms` | `kallsyms_expand_symbol` kretprobe 清空 | 强/新写 |
| D3 | 内核日志 | 驱动自身 `pr_info` | `dmesg`、`/dev/kmsg` | 去日志或 hook syslog | 中/**已实现** |
| D4 | 内核代码完整性 | — | 比对函数首字节 | kprobe 走 BRK 不打补丁 | 强/已有 |
| D5 | ACE 自有内核通道 | `tss_sdk_ioctl` | `/dev/` 节点读写 | 需先确认节点名 | 中/待 |

## E 文件系统面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| E1 | su 二进制 | `6]I-Su`、`fS`2(Su@`4` | `access`/`stat` 常见路径 | 需 fs 层 hook | 强/待 |
| E2 | KernelSU/APatch | 设备实测 `u:r:ksu:s0` | `/data/adb`、ksu 痕迹 | 需 fs 层 hook | 强/待 |
| E3 | 外挂路径 | `/sdk/enable.lo*`、`ms_mode_path` | 扫 sdcard/数据目录 | 需 fs 层 hook | 强/待 |
| E4 | ACE 自身缓存 | `%s/ano_tmp/mpmc.dat`、`com.ace.gamesafe4` | 自身读写 | 不干扰 | 强/无需 |
| E5 | 存储异常 | `disk_detect_mode` | 分区/挂载检查 | 待确认 | 中/待 |

## F 属性面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| F1 | ro.debuggable | 常规 | `__system_property_get` | 属性页 COW | 弱/待 |
| F2 | ro.secure | 常规 | 同上 | 同上 | 弱/待 |
| F3 | ro.build.tags | 常规 | `test-keys` 判定 | 同上 | 弱/待 |
| F4 | 模拟器 | `|emulator_name=`、`goldfish` | `ro.hardware` | 同上 | 强/待 |

## G 网络面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| G1 | 调试端口 | tersafe `socket`/`connect` | 扫 27042 等 | 需 hook bind/connect | 中/待 |
| G2 | 上报包异常 | `send_data_to_game_pack_data_too_long` | 服务端 | — | 强/服务端 |

## H 时序面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| H1 | 页表 hook 耗时 | 页表差异+计时 | 同页读/执行交替测方差 | clonebp `CB_TM_BALANCE` | 强/已有 |
| H2 | 单次异常首项离群 | 同上 | 同址连读 N 次 | `CB_TM_JITTER` + `bailout_max` | 强/已有 |

## I 完整性面

| ID | 检测点 | 证据 | 观测手段 | 驱动对策 | 状态 |
|---|---|---|---|---|---|
| I1 | apk 路径校验 | `apk_path=%s` | 比对安装路径 | — | 强/无需 |
| I2 | 作弊封包 | `tss_sdk_ischeatpacket` | 服务端判定 | — | 强/服务端 |
| I3 | GOT/PLT 完整性 | `.got.plt`/`.rela.plt` | 检查自身 GOT | 不 hook ACE 自身 | 强/已有 |

## 统计

| 状态 | 数量 | ID |
|---|---|---|
| 已有对策 | 9 | A1 A4 B4 B5 C3 C4 D4 H1 H2 I3 |
| 已实现 | 11 | B1 B2 B3 C1 D1 D2 D3 E1 E2 E3 A3 G1 |
| 待实现 | 6 | A2 D5 E5 F1 F2 F3 F4 |
| 无需/服务端 | 5 | B5 E4 G2 I1 I2 |

## 优先级

**P0（ACE 真会看且有明文证据）**：B1 maps、C1 TracerPid、D1 modules、D2 kallsyms、A1 进程可见
**P1（凭据强度中，成本低）**：D3 日志、E1/E2/E3 路径
**P2（需要额外确认）**：F 属性族、G1 端口、D5 ACE 内核通道
**不做**：B5（改回反被查）、E4、I1、I2、G2
