#!/system/bin/sh
# wind 驱动刷入器
#
# 用法: sh wind_flash.sh [ko路径]
#   默认 ko 路径 /data/local/tmp/wind.ko
#   配置存 /data/local/tmp/wind.cfg, 下次可复用
#
# 所有隐藏/反检测项在这里一次决定, 驱动加载后 UI 不再提供任何切换入口。

set -u

KO="${1:-/data/local/tmp/wind.ko}"
CFG=/data/local/tmp/wind.cfg
PROCNAME_FILE=/data/local/tmp/wind_proc_name
MOD=wind

say()  { printf '%s\n' "$*"; }
sep()  { say "-------------------"; }

ask01() { # $1=提示 $2=变量名(回显) $3=默认
    _d="$3"
    say ""
    say "  0  关闭"
    say "  1  开启"
    say ""
    printf '请选择 (0 或 1) [%s]: ' "$_d"
    read _v
    [ -z "$_v" ] && _v="$_d"
    case "$_v" in 0|1) ;; *) _v="$_d";; esac
    eval "$2=\$_v"
}

# ---- root ----
if [ "$(id -u)" != "0" ]; then
    say "[!] 需要 root, 尝试 su"
    exec su -c "sh $0 $KO"
fi

# ---- ko ----
if [ ! -f "$KO" ]; then
    say "[x] 找不到 $KO"
    exit 1
fi

say "wind 驱动刷入器"
say "目标: $KO"
sep

# ---- 上次配置 ----
USE_OLD=0
if [ -f "$CFG" ]; then
    . "$CFG" 2>/dev/null || true
    say "检测到上次配置："
    say "  隐藏驱动=$hide_driver  pid隐藏=$hide_pid  GPU隐藏=$hide_gpu"
    say "  oppo后门=$oppo_backdoor  强制加载=$force_load  陀螺仪=$gyro"
    say "  进程名=${proc_name:-无}  目标=${target_comm}"
    printf '[!] 是否使用 [1 使用 / 0 重新选择]: '
    read _u
    [ "$_u" = "1" ] && USE_OLD=1
fi

if [ "$USE_OLD" != "1" ]; then
    # ---- 是否加载 ----
    say ""
    say "是否加载："
    say "(1 : 加载)"
    say "(2 : 不加载)"
    sep
    printf '请输入选择 (1 或 2): '
    read _load
    [ "$_load" = "2" ] && { say "已取消"; exit 0; }

    hide_driver=1
    say ""
    say "是否隐藏驱动"
    ask01 "" hide_driver 1

    oppo_backdoor=1
    say ""
    say "删除一加/Oppo 的 proc 后门接口"
    ask01 "" oppo_backdoor 1

    force_load=1
    say ""
    say "强制加载"
    ask01 "" force_load 1

    say ""
    say "[!] 以下三项与游戏进程相关，可单独关闭。"

    gyro=1
    say ""
    say "陀螺仪控制"
    ask01 "" gyro 1

    hide_pid=1
    say ""
    say "pid 隐藏"
    ask01 "" hide_pid 1

    hide_gpu=1
    say ""
    say "GPU 隐藏"
    ask01 "" hide_gpu 1

    # ---- 进程名伪装 ----
    say ""
    say "进程名伪装（直接回车 = 不伪装）"
    printf '进程名: '
    read proc_name

    # ---- 目标进程 ----
    say ""
    say "目标进程 comm（直接回车 = com.tencent.tmgp.pubgmhd）"
    printf 'target_comm: '
    read target_comm
    [ -z "${target_comm:-}" ] && target_comm=com.tencent.tmgp.pubgmhd

    cat > "$CFG" <<CFGEOF
hide_driver=$hide_driver
oppo_backdoor=$oppo_backdoor
force_load=$force_load
gyro=$gyro
hide_pid=$hide_pid
hide_gpu=$hide_gpu
proc_name=$proc_name
target_comm=$target_comm
CFGEOF
    say ""
    say "[+] 配置已存 $CFG"
fi

[ -z "${target_comm:-}" ] && target_comm=com.tencent.tmgp.pubgmhd

# ---- 卸载旧的 ----
if lsmod 2>/dev/null | grep -q "^$MOD "; then
    say "[*] 卸载已加载的 $MOD"
    rmmod "$MOD" 2>/dev/null || {
        say "[!] rmmod 失败, 可能仍在使用, 继续尝试加载"
    }
fi

# ---- 进程名伪装落盘, Keep 启动时读取 ----
if [ -n "${proc_name:-}" ]; then
    printf '%s' "$proc_name" > "$PROCNAME_FILE"
    chmod 644 "$PROCNAME_FILE"
    say "[*] 进程名伪装: $proc_name"
else
    rm -f "$PROCNAME_FILE"
    say "[*] 进程名伪装: 关闭"
fi

# ---- 加载 ----
say ""
say "[*] insmod ..."
insmod "$KO" \
    hide_driver="$hide_driver" \
    hide_pid="$hide_pid" \
    hide_gpu="$hide_gpu" \
    oppo_backdoor="$oppo_backdoor" \
    force_load="$force_load" \
    gyro="$gyro" \
    target_comm="$target_comm" \
    ko_path="$KO"
rc=$?

if [ "$rc" != "0" ]; then
    say "[x] insmod 失败 rc=$rc"
    say "    dmesg 末尾:"
    dmesg | tail -8
    exit "$rc"
fi

say "[+] insmod 成功"
sleep 1

# ---- 自检 ----
if lsmod 2>/dev/null | grep -q "^$MOD "; then
    say "[+] 模块在线"
else
    say "[!] 模块不在 lsmod 里（可能 hide_driver 生效，属正常）"
fi

say ""
sep
say "内核日志:"
dmesg | grep -E "wind-cfg|wind-hide|ace|gyro-sock|kgsl" | tail -12
sep
say "自动绑定目标: $target_comm"
say "若游戏还没启动，内核会每 5 秒扫一次，最多 5 分钟。"
say "验证: dmesg | grep wind-cfg | tail -5"
