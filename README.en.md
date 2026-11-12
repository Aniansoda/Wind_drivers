# Wind Driver — Android ARM64 Kernel Module (wind.ko)

[中文](README.md) | **English** | [日本語](README.ja.md) | [Русский](README.ru.md) | [한국어](README.ko.md)

Kernel-mode memory access and debugging framework for Android / Linux ARM64
Single module `wind.ko`, eleven subsystems, one opcode space, three transports
Zero-CRC route, five coexisting breakpoint families, every out-of-whitelist symbol resolved at runtime

---

## Authors

| Item | Info |
|---|---|
| Author | 阿念-川上富江Tomie |
| Credits | 小黑 — JiagNight |
| Contact | xuboyuan763@gmail.com |
| Bug reports | Open an issue with kernel version, device model, last 200 lines of `dmesg` |

The author publishes under `阿念-川上富江Tomie` only
Open source for study and research only — no commercial or unlawful use

---

## Supported kernels

| Kernel | Target | Status |
|---|---|---|
| 5.10.198 | wind.ko | Adapted, zero-CRC route |
| 5.15.149 | wind.ko | Adapted, zero-CRC route |
| 6.1 | wind.ko | Adapted, zero-CRC route |
| 6.6.66 | wind.ko | Device regression passed, zero-CRC route |
| 6.12.58 | wind.ko | Device config aligned, zero-CRC route |
| 6.18.21 | wind.ko | Adapted, zero-CRC route |

Arch aarch64 / arm64
Form loadable kernel module, `insmod` / `rmmod`
Output single `wind.ko`, all subsystems in one module, no dependencies
Source `kernel/` 8067 lines across 19 `.c` / `.h` files

---

## What changed in this version

| Opcode | Before | Now |
|---|---|---|
| `0x26..0x2B` | ETM/PTM branch tracing | **WRP data watchpoint** `wrpbp` |
| `0x38..0x3D` | reserved | removed, no longer defined |
| `0x33` | — | `OP_STATS` cache statistics |
| `0x34 / 0x37` | — | `HWBP_SET_REGS` / `HWBP_REFRESH` |
| `0x35 / 0x36` | — | `PTEBP_SET_REGS` / `PTEBP_REFRESH` |
| `0x3E / 0x3F` | — | `BP_DIAG` / `BP_ALL_OFF` |
| `0x50..0x55` | — | **ACE anti-detection camouflage**, 10 feature bits / 11 kprobes |
| `0x0C / 0x0D` | ETM leftovers | restored **BRK software breakpoints** `brk_set` / `brk_clear` |
| Gyro | one resident execution breakpoint | execution breakpoint to locate, then **switch to steady WRP**, X/Y rewritten, Z never in window |
| Symbols | direct references | 32 out-of-whitelist symbols resolved at runtime via `register_kprobe` |
| Compat | `#ifdef` scattered in code | one `compat.h`, single source tree for 5.10 → 6.18 |

ETM is gone: Coresight needs the target SoC to enable its clocks and funnel, device coverage was poor, and it cannot observe the data itself
`0x26..0x2B` went to WRP; the config struct is still 24 bytes, same as the old `ETMBP_POINT_CONFIG`, so old binaries do not mis-stride

---

## Quick start

```bash
# 1 build (GitHub Actions produces the ko)
#    Actions -> build-wind-ko -> pick kernel -> download artifact

# 2 load
su
insmod wind.ko

# 3 check the log first, every symbol resolution result is printed here
dmesg | grep -i wind

# 4 unload
rmmod wind
```

Artifact path `kernel-src/drivers/wind/wind.ko`

Expected first lines after load

```
wind: sym use_mm=ffffffffxxxxxxxx/ffffffffxxxxxxxx
wind: sym vmap=... filp=... hwbp=... misc=...
wind: netlink unit=31
```

Prebuilt `user/Wind-Drivers-6.6.ko`; self-extracting installers for six kernels in `user/Wind-Drivers-*.sh`

---

## Transports

Three transports share one opcode space and one dispatcher `wind_dispatch_ioctl`
Selected by the `transport` parameter and probe results at load time

| transport | Transport | Mechanism | Notes |
|---|---|---|---|
| 3 | private netlink protocol | registers a private `AF_NETLINK` unit; user space does `socket(AF_NETLINK, SOCK_RAW, 31)` and sends `WIND_NL_MSG` | default, no node under `/dev` or `/proc`, touches no kernel object |
| 2 | private misc char device | `misc_register` | avoids `/dev/null` and rodata, best compatibility, costs one device node |
| 1 | `/dev/null` fops redirect | points `inode->i_fop` at a module copy | fallback only, requires `allow_fops_hook=1`, some devices reboot-loop |

netlink constants

```
default unit 31        WIND_NL_DEFAULT_UNIT
message magic 0x574E4C31  WIND_NL_MAGIC  ("WNL1")
opcode base 0x574E0000    WIND_OP_BASE
accepted range            WIND_OP_BASE + 0x01 .. WIND_OP_BASE + 0x3F
```

Ops outside the range are dropped without a reply, so no accidental input is deep-dereferenced by the dispatcher

### Module parameters

| Param | Type | Default | Effect |
|---|---|---|---|
| `transport` | int | 3 | `3` netlink, `2` misc char device, `1` /dev/null i_fop |
| `install_hook` | bool | 1 | install the transport hook at load time; `0` resolves symbols only |
| `allow_fops_hook` | bool | 0 | allow the `/dev/null` `inode->i_fop` fallback |
| `hook_level` | int | — | bisect helper: `0` off, `1` fops only, `2` fops plus ioctl/mmap, `3` keep original `.open` |

```bash
insmod wind.ko transport=3 install_hook=1
insmod wind.ko transport=0            # load only, no transport, for debugging
```

---

## Opcodes

High 16 bits fixed at `0x574E` (ASCII "WN"), low 16 bits are the index
No collision with the common `0x800` demo drivers

### Session and memory

| Op | Name | Arg | Function |
|---|---|---|---|
| 0x01 | `OP_BIND` | pid | bind target process, cache `task_struct` |
| 0x02 | `OP_UNBIND` | 0 | unbind and drop cache |
| 0x03 | `OP_READ_MEM` | `COPY_MEMORY*` | single read |
| 0x04 | `OP_WRITE_MEM` | `COPY_MEMORY*` | single write |
| 0x05 | `OP_READ_BATCH` | `PBATCH_REQUEST` | batch read, many discrete addresses per syscall |
| 0x06 | `OP_WRITE_BATCH` | `PBATCH_REQUEST` | batch write |
| 0x07 | `OP_SET_MODE` | `MODE_REQUEST*` | switch FAST / SAFE read mode |
| 0x08 | `OP_MODULE_BASE` | `MODULE_BASE*` | module load base inside the target |
| 0x33 | `OP_STATS` | `wind_map_stats*` | page-map and translation cache counters |

### Hiding

| Op | Name | Function |
|---|---|---|
| 0x09 | `OP_HIDE_FILE` | hide file and dir entry by basename, up to 16 |
| 0x0A | `OP_HIDE_PROCESS` | hide process dir entry, up to 64 pids |
| 0x0B | `OP_UNHIDE_ALL` | undo every hide |
| 0x0E | `OP_KGSL_HIDE` | hide pid on the kgsl side |
| 0x0F | `OP_KGSL_HOOK` | install the kgsl hook |

### Hardware breakpoints BRP/WRP

| Op | Name | Function |
|---|---|---|
| 0x0C | `OP_BRK_SET` | legacy single-point breakpoint |
| 0x0D | `OP_BRK_CLEAR` | clear the legacy point |
| 0x14 | `OP_HWBP_SET` | set breakpoint/watchpoint group, 16 slots |
| 0x15 | `OP_HWBP_REMOVE` | clear all, release debug registers |
| 0x16 | `OP_HWBP_GET_CONFIG` | current config and free BRP/WRP count |
| 0x17 | `OP_HWBP_GET_RECORDS` | paged read of aggregated hit PCs |
| 0x18 | `OP_HWBP_SET_RECORD` | write back records with a 2-bit register mask |
| 0x19 | `OP_HWBP_CLEAR_RECORDS` | clear records, `-1` for all |
| 0x34 | `OP_HWBP_SET_REGS` | rewrite registers on hit (`s3`/`s4`/`s5`), raw `.inst` encodings |
| 0x37 | `OP_HWBP_REFRESH` | re-register points after address drift |

### PTE shadow-page execution breakpoints

| Op | Name | Function |
|---|---|---|
| 0x1A | `OP_PTEBP_SET` | flip PTE attributes to fault, 16 slots, no debug registers |
| 0x1B | `OP_PTEBP_REMOVE` | clear all and restore PTEs |
| 0x1C | `OP_PTEBP_GET_CONFIG` | current config |
| 0x1D | `OP_PTEBP_GET_RECORDS` | paged read of hits |
| 0x1E | `OP_PTEBP_SET_RECORD` | write back records |
| 0x1F | `OP_PTEBP_CLEAR_RECORDS` | clear records, `-1` for all |
| 0x35 | `OP_PTEBP_SET_REGS` | rewrite registers on hit through the shadow trampoline |
| 0x36 | `OP_PTEBP_REFRESH` | realign shadow page and PTE |

### BRK software breakpoints

| Op | Name | Function |
|---|---|---|
| 0x20 | `OP_BRKBP_SET` | patch the instruction to `BRK`, 16 slots |
| 0x21 | `OP_BRKBP_REMOVE` | clear all and restore original bytes |
| 0x22 | `OP_BRKBP_GET_CONFIG` | current config |
| 0x23 | `OP_BRKBP_GET_RECORDS` | paged read of hits |
| 0x24 | `OP_BRKBP_SET_RECORD` | write back records |
| 0x25 | `OP_BRKBP_CLEAR_RECORDS` | clear records, `-1` for all |

### WRP data watchpoints

| Op | Name | Function |
|---|---|---|
| 0x26 | `OP_WRPBP_SET` | install a write watchpoint on a target VA, `LEN_8` window |
| 0x27 | `OP_WRPBP_REMOVE` | remove and release WRP registers |
| 0x28 | `OP_WRPBP_GET_CONFIG` | current window config |
| 0x29 | `OP_WRPBP_GET_RECORDS` | paged read of hits |
| 0x2A | `OP_WRPBP_SET_VEC` | write back vector with 2-bit mask, X/Y selectable |
| 0x2B | `OP_WRPBP_CLEAR_RECORDS` | clear records, `-1` for all |

### UDP breakpoints

| Op | Name | Function |
|---|---|---|
| 0x2E | `OP_UDPBP_SET` | install or update one UDP slot, 8 slots |
| 0x2F | `OP_UDPBP_REMOVE` | drop a slot, `-1` for all |
| 0x30 | `OP_UDPBP_GET_CONFIG` | slot table |
| 0x31 | `OP_UDPBP_GET_RECORDS` | paged read, 256 records per slot |
| 0x32 | `OP_UDPBP_CLEAR_RECORDS` | clear records, `-1` for all |

### Gyroscope

| Op | Name | Function |
|---|---|---|
| 0x10 | `OP_GYRO_INSTALL` | install the gyro hook for a pid |
| 0x11 | `OP_GYRO_REMOVE` | uninstall |
| 0x12 | `OP_GYRO_SET` | set target angular velocity x/y/z (rad/s, IEEE 754 bit pattern) |
| 0x13 | `OP_GYRO_ENABLE` | toggle tampering |
| 0x2C | `OP_GYRO_STOP` | pause tampering, keep the hook |
| 0x2D | `OP_GYRO_RESUME` | resume, re-bind if the point was lost |

### Diagnostics and master switch

| Op | Name | Function |
|---|---|---|
| 0x3E | `OP_BP_DIAG` | one-shot state of all five breakpoint families |
| 0x3F | `OP_BP_ALL_OFF` | tear down every family before switching tracking mode |

### ACE anti-detection camouflage

| Opcode | Name | Argument | Purpose |
|---|---|---|---|
| 0x50 | `OP_ACE_SET` | tgid | Select the camouflage target; 0 clears it |
| 0x51 | `OP_ACE_GET_STAT` | `ACE_STAT*` | Read the 10 hit counters |
| 0x52 | `OP_ACE_VERBOSE` | 0 / 1 | Toggle the module's own logging, off by default |
| 0x53 | `OP_ACE_INTE` | 0 / 1 | OnePlus `inte` nodes: rebuild fakes / detach only |
| 0x54 | `OP_ACE_SET_FEATURES` | bitmask | Write the 10 camouflage switches, low 10 bits |
| 0x55 | `OP_ACE_GET_FEATURES` | `uint32_t*` | Read the current switch mask |

`0x38..0x3D` are removed; do not reserve them client-side

---

## Internals

### 1 Session

One `OP_BIND` after open caches the `task_struct` reference in `driver_session`
Later accesses skip `find_get_pid` + `get_pid_task`, saving one pid hash lookup and two refcount ops
`session_destroy` drops the reference on close
`mm` is never cached; `get_task_mm` is called per operation, so an `exec` that swaps `mm` cannot leave a dangling pointer

### 2 Memory access

**Single read/write** one ioctl covers a contiguous range, translating page by page

**Batch** one ioctl carries N discrete addresses, N point accesses collapse into one syscall
Only one `mm` grab per batch, so N `mmgrab`/`mmput` become one pair; CPU and lock contention drop noticeably for multi-thread polling

**mmap zero-copy** target physical pages are `remap_pfn_range`d into the caller, then plain pointer access with no ioctl, good for large sampling

**Two read modes**

| Mode | Implementation | Upside | Downside |
|---|---|---|---|
| `MODE_FAST` | `phys_to_virt` + `memcpy` | fast, cool | detectable through cache side channels |
| `MODE_SAFE` | `ioremap_nocache` + `__flush_dcache_area` | uncached, side-channel resistant | slower, hotter |

**Page map cache** reuses existing mappings for physical pages, with hit/evict counters readable via `OP_STATS`

**VA→PA translation cache** avoids repeated page-table walks, counters `tr_hits` / `tr_misses`

**Swap fallback** on `!pte_present` (swap entry or unmapped) it uses `get_user_pages_remote(FOLL_GET)` to fault the read back in, then retranslates
`FOLL_GET` rather than `FOLL_WRITE`, so read-only mappings never take a COW side effect

**Cross-page correctness** splits at `PAGE_SIZE - (addr & (PAGE_SIZE-1))` so every segment translates on its own page

**Cache alias fix** writing target memory then reading GPU / ION buffers could return stale data and blank the screen
Normal RAM is read through the linear map (same WB cache as the process page tables; ARM64 PIPT is physically indexed), and `__flush_dcache_area` pushes writes back to memory

### 3 Module base

`OP_MODULE_BASE` walks the target `mm` `vm_area_struct` chain for a module name and returns its ELF load base
Before 6.1 it walks the list by hand, from 6.1 it uses the maple tree VMA iterator, selected by `KH_HAVE_VMA_ITER` in `compat.h`
`get_module_base` is also used internally

### 4 Hiding

**File hiding** hooks `filldir64` (falls back to `filldir`) and in `pre_handler` rewrites the target entry name to `.` with `namlen=1`
Basename matching, up to 16 entries

**Process hiding** the same hook matches pure-numeric PID entries, up to 64

**kgsl hiding** hides a pid on the GPU kgsl side, used with `kgsl_hook_install`

**Undo** `OP_UNHIDE_ALL` clears both tables at once

Why kprobe Android GKI 5.x+ ships kCFI and read-only rodata; patching a function prologue trips CFI or the write protector
kprobe registers a callback through the breakpoint machinery, never writes the text, so CFI is bypassed by construction

Lazy symbol registration `hide_*` registers on first use through the candidate list (`filldir64` → `filldir`), so an invisible symbol cannot fail the load

### 5 Five breakpoint families

Shared skeleton: slots + ring-buffer records + paged read-back, with `0x3E` diagnostics and `0x3F` master switch
Execution breakpoints exclude each other by nature: arm64 has 4 BRP and 2 WRP

| Family | Mechanism | Resources | What a record holds | Use |
|---|---|---|---|---|
| `hwbp` | BRP exec / WRP watch via perf events | 4 BRP + 2 WRP | hit PC and registers | function entries, read/write points |
| `ptebp` | PTE attribute flip + 32-byte shadow trampoline | no debug registers | hit point and controllable registers | watching a dozen points at once |
| `wrpbp` | WRP data watchpoint, `LEN_8` window | 2 WRP | write moment and vector | watching one field |
| `brkbp` | instruction patched to `BRK`, kprobe exception handling | no hardware | hit PC and general registers | records only |
| `udpbp` | kprobe on UDP paths | no hardware | payload preview | observing network rhythm |

**hwbp** arm64 exec breakpoints accept `HW_BREAKPOINT_LEN_4` only
Hits record the PC per configuration, paged back to user space, and can be written with a 2-bit register mask
`OP_HWBP_SET_REGS` writes `s3`/`s4`/`s5` directly from the hit callback

**ptebp** flips PTE attribute bits to fault, 16 slots, up to 8 pages per slot
No debug registers, so it coexists with `hwbp`
The original instruction runs from a trampoline, a RWX page opened inside the target `mm`
PTE bit fields (`PTE_UXN` / `PTE_PXN` / `PTE_RDONLY` / `PTE_VALID` / `PTE_AF`) are writable through the shadow-page helpers, with a TLB flush after each change

**wrpbp** the `LEN_8` data window of `arch_install_hw_breakpoint`
The handler reads the value out of `perf_event` into the record; `OP_WRPBP_SET_VEC` writes back with a 2-bit mask
This is the layer the gyro steady state stands on

**brkbp** patches the instruction to `BRK`, handled by the exception hook, 16 slots
Arguments and return values are read and written cross-process in kernel context via `wind_read_process_va` / `wind_write_process_va`, never through `__user`
Records only for now, no register write-back

**udpbp** kprobes on the kernel UDP paths

```
TX  udp_send_skb  ->  ip_send_skb  ->  ip_local_out   first one that resolves wins
RX  __udp4_lib_rcv
```

TX prefers `udp_send_skb` because GKI LTO inlines `ip_local_out`
Filtered by pid, port and direction; hits go into a ring buffer, paged back
8 slots, 256 records per slot, 32-byte preview per record
IPv4 only, `__udp6_lib_rcv` is not hooked

**Legacy brk** `OP_BRK_SET` / `OP_BRK_CLEAR` implement a single exec point via `register_user_hw_breakpoint`, kept for compatibility

**FP register rewrite** the kernel builds with `-mgeneral-regs-only`, and clang's integrated assembler rejects mnemonics such as `stp q3,q4,[x9]`
`hwbp_fp_reg_direct` uses `.inst` with raw encodings, base register fixed to `x9`

| Instruction | Encoding |
|---|---|
| `stp q3, q4, [x9]` | `0xad001123` |
| `str q5, [x9]` | `0x3d800125` |
| `ldp q3, q4, [x9]` | `0xad401123` |
| `ldr q5, [x9]` | `0x3dc00125` |

### 6 Gyroscope

Control is not `/dev/uinput` injection from user space; the module hooks the BitTube exit inside the system library

```
target symbol android::BitTube::sendObjects(sp<BitTube> const&, void const*, size_t, size_t)
candidate libs libsensor.so -> libsensorservice.so -> libui.so
```

Locating `filp_open` + `kernel_read` reads the target ELF, resolves the symbol offset from `.dynsym`, adds the load base of that module inside the target process
`sendObjects` arguments (arm64) `x1` = events array, `x2` = count; `x0` is the tube and is not used for data

Two phases

1. **Locate** install an execution breakpoint on `sendObjects`; the first hit teaches the events buffer address
2. **Steady** drop the execution breakpoint immediately and switch to a `LEN_8` write watchpoint on `data[0..1]`

Steady state leaves one WRP and no execution breakpoint inside system_server, so hit rate drops from every sensor batch to every rewrite
Unregistering a perf event from its own overflow handler deadlocks, so the switch runs in `gyro_set_velocity` (process context, once per frame)

Event layout (`sensors_event_t`, 0x68 bytes)

```
0x00 version | 0x04 sensor | 0x08 type | 0x10 timestamp
0x18 float data[16] | 0x58 flags
```

Recognised types `SENSOR_TYPE_GYROSCOPE`(4) and `SENSOR_TYPE_GYROSCOPE_UNCALIBRATED`(16)
Both keep x/y/z at `data[0..2]` and are rewritten the same way
The NDK `ASensorEvent` moves type to `0x04`, so the code tries `0x08` first and then `0x04`

**Z untouched is structural, not a parameter** the execution breakpoint writes 8 bytes at `+0x18`, the WRP window `LEN_8` covers `data[0..1]`, the client mask is `0x3`
`+0x20` (z) never enters any write window

WRP recursion safety our own `copy_to_user_nofault` re-triggers the watchpoint
Guarded by `user_mode(regs)` plus per-CPU `gyro_wrp_busy` / `wrpbp_busy`

`gyro_stop` / `gyro_resume` pause and resume tampering only, the hook stays
`gyro_remove` actually uninstalls
When the target is gone `gyro_resume` returns `-ESRCH`, when not installed `-ENODEV`; both idempotent

### 7 Shadow pages and kernel write protection

**Shadow page PTE ops** `wind_shadow_pte_set` / `wind_shadow_pte_get`
Walk the target `mm` page tables to the PTE, rewrite as `(old & ~mask) | (val & mask)`, flush the TLB; used for traceless breakpoints and page permission tweaks
The physical field is extracted with `KH_PTE_PA_MASK`, the new PTE is built with `KH_PHYS_TO_PTE`
On 52-bit PA parts the top two PA bits live at PTE `[9:8]`, so a plain `| page_to_phys()` corrupts the entry

**Kernel image write protection** `wind_force_kernel_page_rw` / `wind_kernel_page_ro_state`
`set_memory_rw` refuses image addresses, so the module walks the page table and makes the image page writable itself
`ro_state` verifies the whole page-table path and reads leaf AP[2] without writing anything

### 8 ACE anti-detection camouflage

Only the tgid handed to `OP_ACE_SET` is affected; every other process takes the original path
Each feature bit is independent and written in one shot by `OP_ACE_SET_FEATURES`, low 10 bits valid

| bit | Macro | Hook point | Handling |
|---|---|---|---|
| 0 | `ACE_F_MAPS` | `show_map_vma` | matching lines filtered out of the `seq_file` |
| 1 | `ACE_F_MODS` | `m_show` | `/proc/modules` line filtering |
| 2 | `ACE_F_KSYM` | `kallsyms_expand_symbol` | matching symbol names blanked |
| 3 | `ACE_F_TRACERP` | `proc_pid_status` | `TracerPid` written as 0 |
| 4 | `ACE_F_FS` | `do_sys_openat2` / `do_faccessat` / `vfs_statx` | path pointer set to `NULL`, yields `-EFAULT` |
| 5 | `ACE_F_COMM` | `proc_task_name` | matching thread names filtered |
| 6 | `ACE_F_PORT` | `__sys_connect` | debug-port connections blocked |
| 7 | `ACE_F_PROP` | `mmap_region` | property-area shared mappings counted |
| 8 | `ACE_F_FD` | `do_sys_openat2` | `/proc/<pid>/fd` directory enumeration blocked |
| 9 | `ACE_F_SMAPS` | `show_smap` | smaps segment-level filtering |

**Order** call `ace_set(pid)` first, then `ace_set_features(mask)`
Read back with `ace_get_stat` to confirm each counter actually fired
A zero counter means that hook point was never reached — usually the symbol name differs on that kernel, check the resolution result in `dmesg`

**bits 7 / 8 / 9 were added later** covering property-area detection, fd directory enumeration and smaps filtering
All three only count while the target is being read; staying at 0 when idle is normal

**`ace_enable_feature(bit, on)`** is a convenience wrapper for a single bit — read-modify-write, not atomic
Add your own lock when toggling different bits from multiple threads, or write the whole mask with `ace_set_features`

**OnePlus `inte` nodes** `ace_inte(0)` rebuilds three fake nodes, `ace_inte(1)` detaches without rebuilding
Returns `-ENODEV` on non-OnePlus devices; try once before relying on it

---

## Symbol strategy: zero CRC plus runtime resolution

Two legs; missing either one means the module will not load on a real device

### Zero CRC

After the build the lengths of `__versions` / `__version_ext_crcs` / `__version_ext_names` are zeroed
`check_version()` then skips the whole CRC comparison and leaves one `pr_warn_once`
The still-present `__versions` section makes `same_magic(has_crcs=1)` compare only the tail after the first space of vermagic, `SMP preempt mod_unload modversions aarch64`

| Kernel | File | Branch when the version table is empty | Third arg of `same_magic` |
|---|---|---|---|
| 5.10.198 | `kernel/module.c` | `Broken toolchain` → `return 1` | `info->index.vers` |
| 5.15.149 | `kernel/module.c` | same | same |
| 6.1 | `kernel/module/version.c` | same | same |
| 6.6.66 | `kernel/module/version.c` | same | same |
| 6.12.58 | `kernel/module/version.c` | same, plus the ext table branch | same |
| 6.18.21 | `kernel/module/version.c` | same, plus the ext table branch | same |

Boundary this route removes the "CRC must match every device" requirement, it does not remove vermagic
The tail (`SMP preempt mod_unload modversions aarch64`) still has to match byte for byte
So one ko covers devices of the same series with the same config traits and any `LOCALVERSION` prefix
Crossing series (a 6.1 ko on 6.12) is still rejected with `bad vermagic`
The config must still match the device, otherwise CFI type hashes and struct layouts differ and the module loads then crashes

Device vermagic

```
6.12.58-android16-6-g7704a1ae279b-ab15213644-4k SMP preempt mod_unload modversions aarch64
```

### Runtime symbol resolution

GKI freezes the KMI; a vendor module may only reference symbols in the whitelist intersection
The 32 functions this driver needs are not in it, and a direct reference produces

```
wind: Unknown symbol kthread_use_mm (err -2)
```

`err -2` is ENOENT — the name is absent from the export table, and `insmod` fails outright
So all of them go through `sym.c`: `register_kprobe` (whitelisted) resolves the address by name, and no CRC reference is emitted at build time

```
mm/proc    mmput  kthread_use_mm  kthread_unuse_mm  remap_pfn_range
vmap       __get_vm_area_caller  free_vm_area  vunmap  ioremap_page_range  iounmap
page table set_memory_rw  set_memory_ro  contpte_ptep_get  __arch_clear_user
file       filp_open  kernel_read  file_path
device     misc_register  misc_deregister
netlink    netlink_kernel_create  __netlink_kernel_create  netlink_kernel_release
           __netlink_kernel_release  netlink_unicast  __alloc_skb
breakpoint register_user_hw_breakpoint  unregister_hw_breakpoint
           register_wide_hw_breakpoint  unregister_wide_hw_breakpoint
fp         fpsimd_preserve_current_state
nofault    copy_from_user_nofault  copy_to_user_nofault
```

Results are printed at load; a failure prints `wind: ... 未解析到`
The related capability degrades but loading still succeeds

### Compat layer

`compat.h` centralises everything, one source tree for 5.10 through 6.18

| Item | Boundary | Handling |
|---|---|---|
| `mmap_lock` | 5.8 | `mmap_read_lock` ↔ `down_read(&mm->mmap_sem)` |
| `kmap_local_page` / `kunmap_local` | 5.11 | fall back to `kmap_atomic` / `kunmap_atomic` |
| `ptep_get` | 5.10 | `*(ptep)` when absent |
| maple tree VMA iteration | 6.1 | `KH_HAVE_VMA_ITER` |
| `get_user_pages_remote` arity | 5.9 / 6.5 | three branches |
| `__flush_dcache_area` | 5.11 | inline `dc civac` sequence |
| `arm64_use_ng_mappings` | 6.1 / 6.6 / 6.12 | per-version value |
| `PTE_ADDR_MASK` | removed in 5.11 | `KH_PTE_PA_MASK` four-step ladder |
| physical address into PTE | 52-bit PA | `KH_PHYS_TO_PTE` builds the bit pattern |
| version header | `linux/version.h` removed in 6.10 | include `generated/uapi/linux/version.h` |

---

## Client side

| File | Purpose |
|---|---|
| `user/Wind-对接头文件.h` | one `c_wind` class with every capability: read / write / batch / frame cache / snapshot / hiding / kgsl / gyro / five breakpoint families / clone-page breakpoints / ACE camouflage |
| `user/Wind-drivers-netlink_v1.3.0.zip` | netlink client sample |
| `user/Wind-Drivers-*.sh` | self-extracting installers (5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18) |
| `user/Wind-Drivers-6.6.ko` | prebuilt 6.6 module |
| `user/1.sh` | installer helper |

Minimal flow

```cpp
#include "wind.hpp"

c_wind &d = c_wind::instance(target_pid);   // netlink unit 31 by default
int v = d.read<int>(base + 0x10);
d.write<int>(base + 0x10, 123);

d.frame_begin();                            // same address reads once per frame
uint64_t hp = 0;
d.frame_read(base + 0x100, hp);

d.snap_start(base + 0x200, 60);             // background 60 Hz refresh
uint32_t cur = d.snap_value<uint32_t>();
d.snap_stop();
```

`sizeof(c_wind)` is about 48 KB, of which 47 KB is the frame cache (2048 slots)
Allocate local instances on the heap

Call `bp_all_off()` before switching tracking mode, otherwise the previous family still holds BRP/WRP

```cpp
d.bp_all_off();        // 0x3F, tears down all five families
```

---

## Build

### A — GitHub Actions (recommended)

```
Actions -> build-wind-ko -> Run workflow -> pick kernel -> download artifact
```

Six rows in the matrix, all on the zero-CRC route (`zero_versions: "1"`)

```
android12-5.10.198
android13-5.15.149
android14-6.1
android15-6.6.66
android16-6.12.58-device      device config aligned
android17-6.18.21
```

### B — Local cross build

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LLVM=1 LLVM_IAS=1
make -j"$(nproc)" M=kernel modules
```

clang 14 or newer (5.x needs unprefixed LLVM tools), or aarch64-linux-gnu-gcc
The kernel builds with `-mgeneral-regs-only`, so FP/SIMD in inline asm must be written as `.inst`; only gcc accepts the mnemonics

### Helper tools

| Tool | Purpose |
|---|---|
| `tools/dump_kcrc.py` | `dump` / `harvest` / `merge` / `check` / `diff` / `count` on kernel CRC tables |
| `tools/fix_versions.py` | rewrite CRCs in `__versions` from a symvers table |
| `tools/fix_vermagic.py` | move `.modinfo` to the end of the file and write the target vermagic |
| `tools/extract_installer_ko.py` | pull the embedded ko and CRC table out of a `.sh` installer |
| `tools/zero_versions.py` | zero the version section lengths (`-n` dry run, in-place edits keep a `.bak`) |
| `tools/harvest_kcrc.sh` | harvest CRC tables from on-device vendor modules |
| `tools/windbench.cpp` | transport benchmark and heat comparison |
| `tools/btf_offsets.py` | struct offsets from BTF |
| `tools/pad_info.sh` / `pad_pack.sh` / `pad_probe.sh` | device info, packing, probing |
| `tools/diag_lockup.sh` | capture a lockup scene |
| `extract_symvers.py` | extract symvers from a ko |

---

## Tree

```
Wind_driver/
├── kernel/                 module source (8067 lines)
│   ├── entry.c             entry, transport setup, ioctl dispatch
│   ├── memory.c            read/write, page-table walk, page map cache, mmap
│   ├── process.c           module base, process helpers
│   ├── hide.c              file and process hiding (kprobe)
│   ├── breakpoint.c        five breakpoint families + UDP + shadow page
│   ├── gyro.c              gyro hook (exec bp -> WRP steady state)
│   ├── kgsl.c              GPU kgsl hook
│   ├── ace.c               ACE anti-detection camouflage (10 kprobes)
│   ├── nlcomm.c            netlink transport and op whitelist
│   ├── sym.c               runtime resolution for 32 out-of-whitelist symbols
│   ├── comm.h              opcodes and protocol structs
│   ├── compat.h            kernel compat layer (5.10 -> 6.18)
│   ├── memory.h / breakpoint.h / gyro.h / nlcomm.h / process.h / kgsl.h / sym.h
│   ├── Kconfig
│   └── Makefile
├── user/                   client header, samples, installers
├── tools/                  build and device helpers
├── .github/workflows/      CI cross build
├── CHANGES.md              round-by-round changes and device evidence
├── CHANGELOG.md            early milestones
├── HOOKPTE-对照.md         hook implementation cross-reference
├── README.md               Chinese
├── README.en.md            English (this file)
├── README.ja.md / README.ru.md / README.ko.md
└── LICENSE
```

---

## Troubleshooting

| Symptom | Look at |
|---|---|
| `Unknown symbol ... (err -2)` on load | the symbol is not exported; route it through `sym.c` runtime resolution |
| `disagrees about version of symbol` | CRC mismatch; use the zero-CRC route or rebuild |
| `version magic ... should be` | vermagic mismatch |
| No transport after `insmod` | `dmesg` for `transport`, `misc_register`, `netlink` |
| Freeze or reboot after opening the transport | use `transport=2`, avoid `allow_fops_hook=1` |
| Reads return stale data | switch to `MODE_SAFE`, check for GPU / ION buffers |
| Hiding does nothing | is `CONFIG_KALLSYMS_ALL` on, is `filldir64` visible |
| Gyro does nothing | is the target the process reading sensors, did `sendObjects` resolve, is the WRP armed in `dmesg` |
| Breakpoints refuse to install | call `bp_all_off` first, then check BRP/WRP headroom in `OP_BP_DIAG` |
| Target crashes after a shadow-page hit | check `KH_PTE_PA_MASK` / `KH_PHYS_TO_PTE` against the device PA width |

Log keywords

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

## Known limits

- Hiding depends on symbol visibility; without `CONFIG_KALLSYMS_ALL` `filldir64` is not found
- The `/dev/null` fops transport has freeze-reboot reports on some devices; off by default
- The zero-CRC route skips symbol CRCs only, not the vermagic tail, not CFI, not struct layout
- Zero CRC does not cross kernel series; a 6.1 ko on 6.12 is rejected with `bad vermagic`
- `brkbp` records only, no register write-back
- `hwbp` / `ptebp` are not TID-scoped yet, hits are process wide
- arm64 exec breakpoints accept `LEN_4` only, instructions must be 4-byte aligned
- 4 BRP and 2 WRP are hardware limits; all five families at once will contend
- `ptebp` covers up to 8 pages per slot; call `OP_PTEBP_REFRESH` after a page flip
- UDP breakpoints are IPv4 only, IPv6 paths are not hooked
- Artifacts before 6.12.58 depend on the device config and CRC table in `tools/device/`; a different model needs a fresh harvest

---

## History

Last eight rounds (50 → 57)

| Round | Content |
|---|---|
| 50 | Keep client wired to every new capability |
| 51 | stack corruption when switching tracking mode |
| 52 | duplicated dispatch block in `entry.c` plus three compile/logic defects |
| 53 | `breakpoint.c` build errors and missing symbols |
| 54 | arm64 kernel forbids FP registers, switched to raw `.inst` encodings |
| 55 | porting gaps across 5.10 / 6.12 / 6.18 (`kmap_local_page`, `PTE_ADDR_MASK`) |
| 56 | `include/linux/version.h` gone after 6.10, now includes the generated header |
| 57 | `kthread_use_mm` / `kthread_unuse_mm` not exported, now resolved at runtime |

See `CHANGES.md` for the full round-by-round log and device evidence
Benchmarks and heat comparison in `tools/windbench.cpp`
