# Wind Driver — Android ARM64 カーネルモジュール (wind.ko)

[中文](README.md) | [English](README.en.md) | **日本語** | [Русский](README.ru.md) | [한국어](README.ko.md)

Android / Linux ARM64 向けカーネル空間メモリアクセス・デバッグ基盤
単一モジュール `wind.ko`、11 サブシステム、1 系統のオペコード、3 つの通信路
ゼロ CRC 方式、5 系統のブレークポイントが同時共存、ホワイトリスト外シンボルはすべて実行時解決

---

## 作者と連絡先

| 項目 | 情報 |
|---|---|
| 作者 | 阿念-川上富江Tomie |
| 謝辞 | 小黑 — JiagNight |
| 連絡先 | xuboyuan763@gmail.com |
| 不具合報告 | issue を推奨。カーネル版、機種、`dmesg` 末尾 200 行を添付 |

作者名義は `阿念-川上富江Tomie` のみ
本プロジェクトは学習・研究目的の公開であり、商用利用や違法用途を想定しない

---

## 対応範囲

| カーネル | ビルド対象 | 状態 |
|---|---|---|
| 5.10.198 | wind.ko | 対応済み、ゼロ CRC 方式 |
| 5.15.149 | wind.ko | 対応済み、ゼロ CRC 方式 |
| 6.1 | wind.ko | 対応済み、ゼロ CRC 方式 |
| 6.6.66 | wind.ko | 実機回帰済み、ゼロ CRC 方式 |
| 6.12.58 | wind.ko | 実機 config 整合、ゼロ CRC 方式 |
| 6.18.21 | wind.ko | 対応済み、ゼロ CRC 方式 |

アーキテクチャ aarch64 / arm64
形態 ローダブルカーネルモジュール、`insmod` で読込・`rmmod` で解除
成果物 単一 `wind.ko`、全サブシステムを 1 モジュールに統合、依存モジュールなし
ソース `kernel/` は 8067 行、`.c` / `.h` 計 19 ファイル

---

## この版の変更点

| コード | 旧 | 現 |
|---|---|---|
| `0x26..0x2B` | ETM/PTM 分岐追跡 | **WRP データ監視点** `wrpbp` |
| `0x38..0x3D` | 予約 | 廃止、定義しない |
| `0x50..0x56` | — | **ACE 検出回避カモフラージュ**、10 feature bit / 11 kprobe |
| `0x0C / 0x0D` | ETM の残骸 | **BRK ソフトブレーク**旧インタフェース `brk_set` / `brk_clear` を復活 |
| `0x33` | — | `OP_STATS` キャッシュ統計 |
| `0x34 / 0x37` | — | `HWBP_SET_REGS` / `HWBP_REFRESH` |
| `0x35 / 0x36` | — | `PTEBP_SET_REGS` / `PTEBP_REFRESH` |
| `0x3E / 0x3F` | — | `BP_DIAG` / `BP_ALL_OFF` |
| ジャイロ | 実行ブレークポイント常駐 | 実行ブレークポイントで位置特定 → **WRP 定常状態へ移行**、X/Y のみ書換、Z は窓に入れない |
| シンボル | 直接参照 | ホワイトリスト外 32 個を `register_kprobe` で実行時解決 |
| 互換 | コード各处に `#ifdef` | `compat.h` に集約、5.10 → 6.18 を 1 ソースで |

ETM は削除した。Coresight は SoC 側でクロックと funnel を開く必要があり、機種のカバー率が低く、データ自体も観測できない
`0x26..0x2B` は WRP に割当。構造体サイズは旧 `ETMBP_POINT_CONFIG` と同じ 24 バイトなので既存バイナリでも位置ずれしない

---

## クイックスタート

```bash
# 1 ビルド（GitHub Actions で ko を生成）
#    Actions -> build-wind-ko -> カーネル選択 -> artifact を取得

# 2 読み込み
su
insmod wind.ko

# 3 まずログを見る（シンボル解決結果はここに出る）
dmesg | grep -i wind

# 4 解除
rmmod wind
```

成果物パス `kernel-src/drivers/wind/wind.ko`

読み込み直後のログ例

```
wind: sym use_mm=ffffffffxxxxxxxx/ffffffffxxxxxxxx
wind: sym vmap=... filp=... hwbp=... misc=...
wind: netlink unit=31
```

`user/Wind-Drivers-6.6.ko` に 6.6 用のビルド済みモジュール、`user/Wind-Drivers-*.sh` に 6 版の自己解凍インストーラ

---

## 通信路

3 つの通信路が同一オペコードと同一ディスパッチャ `wind_dispatch_ioctl` を共有する
読み込み時に `transport` パラメータと検出結果で選択

| transport | 通信路 | 仕組み | 備考 |
|---|---|---|---|
| 3 | netlink 私設プロトコル | 私設 `AF_NETLINK` ユニットを登録、ユーザ空間は `socket(AF_NETLINK, SOCK_RAW, 31)` で `WIND_NL_MSG` を送る | 既定。`/dev` と `/proc` にノードを作らず、カーネルオブジェクトを一切書換えない |
| 2 | 私設 misc キャラクタデバイス | `misc_register` で登録 | `/dev/null` も rodata も触らない。互換性は最良、代償はデバイスノード 1 つ |
| 1 | `/dev/null` fops 差し替え | `inode->i_fop` をモジュール内コピーへ向ける | 最後の手段。`allow_fops_hook=1` が必要、一部機種でフリーズ再起動の記録あり |

netlink 定数

```
既定ユニット 31            WIND_NL_DEFAULT_UNIT
メッセージ魔数 0x574E4C31  WIND_NL_MAGIC  ("WNL1")
オペコード基底 0x574E0000  WIND_OP_BASE
受付範囲                  WIND_OP_BASE + 0x01 .. WIND_OP_BASE + 0x3F
```

範囲外の op は応答なしで破棄し、想定外の入力が dispatch で深く参照されないようにする

### モジュールパラメータ

| パラメータ | 型 | 既定 | 役割 |
|---|---|---|---|
| `transport` | int | 3 | `3` netlink、`2` misc、`1` /dev/null i_fop |
| `install_hook` | bool | 1 | 読み込み時に通信路 hook を設置。`0` はシンボル解決のみ |
| `allow_fops_hook` | bool | 0 | `/dev/null` の `inode->i_fop` 差し替えを許可 |
| `hook_level` | int | — | 切り分け用 `0` off、`1` fops のみ、`2` fops + ioctl/mmap、`3` 元の `.open` 維持 |

```bash
insmod wind.ko transport=3 install_hook=1
insmod wind.ko transport=0            # 通信路なしで読み込みのみ、調査用
```

---

## オペコード一覧

上位 16 ビットは固定 `0x574E`（ASCII "WN"）、下位 16 ビットが連番
一般的な `0x800` 系サンプルドライバとは衝突しない

### セッションとメモリ

| コード | 名前 | 引数 | 機能 |
|---|---|---|---|
| 0x01 | `OP_BIND` | pid | 対象プロセスをバインドし `task_struct` をキャッシュ |
| 0x02 | `OP_UNBIND` | 0 | 解除してキャッシュ解放 |
| 0x03 | `OP_READ_MEM` | `COPY_MEMORY*` | 単発読み |
| 0x04 | `OP_WRITE_MEM` | `COPY_MEMORY*` | 単発書き |
| 0x05 | `OP_READ_BATCH` | `PBATCH_REQUEST` | バッチ読み、1 syscall で複数アドレス |
| 0x06 | `OP_WRITE_BATCH` | `PBATCH_REQUEST` | バッチ書き |
| 0x07 | `OP_SET_MODE` | `MODE_REQUEST*` | 読み取りモード FAST / SAFE 切替 |
| 0x08 | `OP_MODULE_BASE` | `MODULE_BASE*` | 対象プロセス内モジュールのロード基底 |
| 0x33 | `OP_STATS` | `wind_map_stats*` | ページマップ・翻訳キャッシュ統計 |

### 隠蔽

| コード | 名前 | 機能 |
|---|---|---|
| 0x09 | `OP_HIDE_FILE` | basename 一致でファイルとディレクトリ項目を隠す、最大 16 件 |
| 0x0A | `OP_HIDE_PROCESS` | プロセス項目を隠す、最大 64 pid |
| 0x0B | `OP_UNHIDE_ALL` | 隠蔽をすべて解除 |
| 0x0E | `OP_KGSL_HIDE` | kgsl 側で pid を隠す |
| 0x0F | `OP_KGSL_HOOK` | kgsl hook を設置 |

### ハードウェアブレークポイント BRP/WRP

| コード | 名前 | 機能 |
|---|---|---|
| 0x0C | `OP_BRK_SET` | 旧版の単点ハードウェアブレークポイント |
| 0x0D | `OP_BRK_CLEAR` | 旧版の単点を解除 |
| 0x14 | `OP_HWBP_SET` | ブレークポイント／監視点グループ設定、16 スロット |
| 0x15 | `OP_HWBP_REMOVE` | 全解除し debug レジスタを返却 |
| 0x16 | `OP_HWBP_GET_CONFIG` | 現在の設定と空き BRP/WRP 数 |
| 0x17 | `OP_HWBP_GET_RECORDS` | 集約したヒット PC を分割読み |
| 0x18 | `OP_HWBP_SET_RECORD` | 記録と 2-bit レジスタ書換マスクを書き戻す |
| 0x19 | `OP_HWBP_CLEAR_RECORDS` | 記録消去、`-1` で全消去 |
| 0x34 | `OP_HWBP_SET_REGS` | ヒット時に `s3`/`s4`/`s5` を書換、`.inst` の生エンコード |
| 0x37 | `OP_HWBP_REFRESH` | アドレス移動後に再登録 |

### PTE シャドウページ実行ブレークポイント

| コード | 名前 | 機能 |
|---|---|---|
| 0x1A | `OP_PTEBP_SET` | PTE 属性を反転して fault を起こす、16 スロット、debug レジスタ不使用 |
| 0x1B | `OP_PTEBP_REMOVE` | 全解除し PTE を復元 |
| 0x1C | `OP_PTEBP_GET_CONFIG` | 現在の設定 |
| 0x1D | `OP_PTEBP_GET_RECORDS` | ヒット記録を分割読み |
| 0x1E | `OP_PTEBP_SET_RECORD` | 記録を書き戻す |
| 0x1F | `OP_PTEBP_CLEAR_RECORDS` | 記録消去、`-1` で全消去 |
| 0x35 | `OP_PTEBP_SET_REGS` | ヒット時にシャドウトランポリン経由でレジスタ書換 |
| 0x36 | `OP_PTEBP_REFRESH` | シャドウページと PTE を再整合 |

### BRK ソフトウェアブレークポイント

| コード | 名前 | 機能 |
|---|---|---|
| 0x20 | `OP_BRKBP_SET` | 命令を `BRK` に書換、16 スロット |
| 0x21 | `OP_BRKBP_REMOVE` | 全解除し元の命令バイトを復元 |
| 0x22 | `OP_BRKBP_GET_CONFIG` | 現在の設定 |
| 0x23 | `OP_BRKBP_GET_RECORDS` | ヒット記録を分割読み |
| 0x24 | `OP_BRKBP_SET_RECORD` | 記録を書き戻す |
| 0x25 | `OP_BRKBP_CLEAR_RECORDS` | 記録消去、`-1` で全消去 |

### WRP データ監視点

| コード | 名前 | 機能 |
|---|---|---|
| 0x26 | `OP_WRPBP_SET` | 対象 VA に書込み監視点を設置、`LEN_8` 窓 |
| 0x27 | `OP_WRPBP_REMOVE` | 解除し WRP レジスタを返却 |
| 0x28 | `OP_WRPBP_GET_CONFIG` | 現在の窓設定 |
| 0x29 | `OP_WRPBP_GET_RECORDS` | ヒット記録を分割読み |
| 0x2A | `OP_WRPBP_SET_VEC` | 2-bit マスク付きでベクタを書き戻す（X/Y を個別に） |
| 0x2B | `OP_WRPBP_CLEAR_RECORDS` | 記録消去、`-1` で全消去 |

### UDP ブレークポイント

| コード | 名前 | 機能 |
|---|---|---|
| 0x2E | `OP_UDPBP_SET` | UDP スロットを設置・更新、8 スロット |
| 0x2F | `OP_UDPBP_REMOVE` | スロット解除、`-1` で全解除 |
| 0x30 | `OP_UDPBP_GET_CONFIG` | スロット表 |
| 0x31 | `OP_UDPBP_GET_RECORDS` | 分割読み、1 スロット 256 件 |
| 0x32 | `OP_UDPBP_CLEAR_RECORDS` | 記録消去、`-1` で全消去 |

### ジャイロ

| コード | 名前 | 機能 |
|---|---|---|
| 0x10 | `OP_GYRO_INSTALL` | 対象 pid にジャイロ hook を設置 |
| 0x11 | `OP_GYRO_REMOVE` | hook を解除 |
| 0x12 | `OP_GYRO_SET` | 目標角速度 x/y/z（rad/s、IEEE 754 ビットパターン） |
| 0x13 | `OP_GYRO_ENABLE` | 書換の有効・無効 |
| 0x2C | `OP_GYRO_STOP` | 書換を停止、hook は維持 |
| 0x2D | `OP_GYRO_RESUME` | 書換を再開、点が失われていれば再バインド |

### 診断と全体スイッチ

| コード | 名前 | 機能 |
|---|---|---|
| 0x3E | `OP_BP_DIAG` | 5 系統のスロットとヒット数を一括取得 |
| 0x3F | `OP_BP_ALL_OFF` | 全系統を一括解除、追跡方式の切替前に呼ぶ |

`0x38..0x3D` は廃止。クライアント側で予約しないこと

### ACE 検出回避カモフラージュ

| コード | 名前 | 引数 | 機能 |
|---|---|---|---|
| 0x50 | `OP_ACE_SET` | tgid | カモフラージュ対象を指定、0 で解除 |
| 0x51 | `OP_ACE_GET_STAT` | `ACE_STAT*` | 10 項目のヒット数を読む |
| 0x52 | `OP_ACE_VERBOSE` | 0 / 1 | モジュール自身のログ切替、既定はオフ |
| 0x53 | `OP_ACE_INTE` | 0 / 1 | OnePlus `inte` ノードの接収 / 摘除のみ |
| 0x54 | `OP_ACE_SET_FEATURES` | ビットマスク | 10 個のスイッチを書く、下位 10 ビット |
| 0x56 | `OP_WRPBP_SET_FREGS` | `WRPBP_FREG_REQUEST*` | WRP 浮動小数点レジスタ書き戻し (S0..S31) |

詳細は `README.md` の「8 ACE 反检测伪装」を参照


---

## 内部動作

### 1 セッション

`open` 後に 1 度 `OP_BIND` を呼ぶと `task_struct` 参照を `driver_session` にキャッシュする
以降は `find_get_pid` + `get_pid_task` を通らず、pid ハッシュ探索 1 回と参照カウント操作 2 回を節約
`close` 時に `session_destroy` が参照を解放
`mm` はキャッシュしない。毎回 `get_task_mm` で取得するので、`exec` による `mm` 差し替え後も宙ぶらりんにならない

### 2 メモリアクセス

**単発読み書き** 1 回の ioctl で連続領域を扱い、ページ境界ごとに翻訳

**バッチ** 1 回の ioctl に N 個の離散アドレスを載せ、N 回の点アクセスを 1 syscall にまとめる
`mm` 取得はバッチ内で 1 回だけなので、N 回の `mmgrab`/`mmput` が 1 組になる。多スレッド高頻度読みで CPU とロック競合が明確に減る

**mmap ゼロコピー** 対象の物理ページを `remap_pfn_range` で呼び出し側に写像し、以降はポインタ直接アクセス。ioctl 不要で大規模サンプリング向け

**2 つの読み取りモード**

| モード | 実装 | 利点 | 欠点 |
|---|---|---|---|
| `MODE_FAST` | `phys_to_virt` + `memcpy` | 高速・低発熱 | キャッシュ側チャネルで検出され得る |
| `MODE_SAFE` | `ioremap_nocache` + `__flush_dcache_area` | 非キャッシュで側チャネル耐性 | 低速・発熱 |

**ページマップキャッシュ** 物理ページの既存写像を再利用し、ヒット／追出し回数を `OP_STATS` で読める

**VA→PA 翻訳キャッシュ** 同一仮想アドレスの再読みでページテーブル走査を省く。統計は `tr_hits` / `tr_misses`

**swap フォールバック** `!pte_present`（swap entry か未写像）では `get_user_pages_remote(FOLL_GET)` で読み fault として入れてから再翻訳
`FOLL_WRITE` ではなく `FOLL_GET` を使い、読み取り専用写像で COW の副作用を起こさない

**ページ跨ぎ** `PAGE_SIZE - (addr & (PAGE_SIZE-1))` で分割し、区間ごとに独立翻訳する

**キャッシュ別名の修正** 対象メモリ書込み後に GPU / ION バッファが古い値を返すと画面が黒くなる
通常 RAM はリニアマップ直読み（プロセスページテーブルと同じ WB キャッシュ、ARM64 は PIPT で物理索引のため一致）とし、書込み後に `__flush_dcache_area` で主記憶へ戻す

### 3 モジュール基底

`OP_MODULE_BASE` はモジュール名で対象 `mm` の `vm_area_struct` を走査し ELF ロード基底を返す
6.1 未満はリスト手書き走査、6.1 以降は maple tree の VMA イテレータ。`compat.h` の `KH_HAVE_VMA_ITER` で分岐
`get_module_base` はカーネル内部からも使う

### 4 隠蔽

**ファイル隠蔽** `filldir64`（不可なら `filldir`）を hook し、`pre_handler` で対象項目名を `.`、`namlen=1` に書換える
basename 一致、最大 16 件

**プロセス隠蔽** 同じ hook で純数字の PID 項目を照合、最大 64 件

**kgsl 隠蔽** GPU 側 kgsl で pid を隠す。`kgsl_hook_install` と併用

**全解除** `OP_UNHIDE_ALL` で両表を一括消去

kprobe を使う理由 Android GKI 5.x 以降は kCFI と読み取り専用 rodata を持ち、関数先頭の書換えは CFI 検査か書込み保護で落ちる
kprobe はブレークポイント機構でコールバックを登録するだけでテキストを書かないため、構造上 CFI を踏まない

シンボルの遅延登録 `hide_*` の初回呼び出し時に候補表（`filldir64` → `filldir`）で登録するので、シンボルが見えなくても読み込みは失敗しない

### 5 5 系統のブレークポイント

「スロット + リングバッファ記録 + 分割読み戻し」の骨格を共有し、`0x3E` 診断と `0x3F` 全体スイッチも共通
実行ブレークポイント同士は排他。arm64 の BRP は 4 本、WRP は 2 本しかない

| 系統 | 仕組み | 消費 | 記録から得られるもの | 用途 |
|---|---|---|---|---|
| `hwbp` | BRP 実行／WRP 監視、perf イベント | BRP 4 + WRP 2 | ヒット PC とレジスタ | 関数入口、読み書き点 |
| `ptebp` | PTE 属性反転 + 32 バイトシャドウトランポリン | debug レジスタ不使用 | ヒット点と制御可能レジスタ | 十数点を同時に監視 |
| `wrpbp` | WRP データ監視点、`LEN_8` 窓 | WRP 2 | 書込み時刻とベクタ | 単一フィールドの書換監視 |
| `brkbp` | 命令を `BRK` に書換、kprobe 例外処理 | ハード不使用 | ヒット PC と汎用レジスタ | 記録型の観測 |
| `udpbp` | UDP 経路への kprobe | ハード不使用 | パケット内容のプレビュー | 通信リズムの観測 |

**hwbp** arm64 の実行ブレークポイントは `HW_BREAKPOINT_LEN_4` のみ
ヒット時に設定どおり PC を記録し分割で読み戻す。2-bit レジスタマスク付きで書き戻し可能
`OP_HWBP_SET_REGS` はヒットコールバックから `s3`/`s4`/`s5` を直接書く

**ptebp** PTE 属性ビットを反転して fault を起こす。16 スロット、1 スロット最大 8 ページ
debug レジスタを使わないため `hwbp` と共存できる
元命令はトランポリンで実行し、トランポリンは対象 `mm` 内に RWX 1 ページとして開く
PTE ビット域（`PTE_UXN` / `PTE_PXN` / `PTE_RDONLY` / `PTE_VALID` / `PTE_AF`）を直接読み書きでき、変更後は TLB を更新

**wrpbp** `arch_install_hw_breakpoint` の `LEN_8` データ窓
ヒット時に `perf_event` の値を記録へ写し、`OP_WRPBP_SET_VEC` が 2-bit マスク付きで書き戻す
ジャイロの定常状態はこの層に載っている

**brkbp** 命令を `BRK` に書換え、例外ハンドラで受ける。16 スロット
ヒット時にカーネル空間から `wind_read_process_va` / `wind_write_process_va` で引数取得と戻り値書換を行い、`__user` を経由しない
現状は記録のみで、レジスタ書き戻しは未実装

**udpbp** カーネル UDP 経路への kprobe

```
TX  udp_send_skb  ->  ip_send_skb  ->  ip_local_out   解決できた最初のものを使う
RX  __udp4_lib_rcv
```

TX は `udp_send_skb` を優先する。GKI の LTO ビルドが `ip_local_out` をインライン化するため
pid、ポート、方向で絞り込み、ヒットはリングバッファへ。分割読みで取得
8 スロット、1 スロット 256 件、1 件 32 バイトのプレビュー
IPv4 のみ。IPv6 の `__udp6_lib_rcv` は未接続

**旧版 brk** `OP_BRK_SET` / `OP_BRK_CLEAR` は `register_user_hw_breakpoint` による単点実行ブレークポイント。互換のため残置

**FP レジスタ書換** カーネルは `-mgeneral-regs-only` でビルドされ、clang の統合アセンブラは `stp q3,q4,[x9]` のようなニーモニックを拒否する
`hwbp_fp_reg_direct` は `.inst` で生エンコードを書き、基底レジスタは `x9` 固定

| 命令 | エンコード |
|---|---|
| `stp q3, q4, [x9]` | `0xad001123` |
| `str q5, [x9]` | `0x3d800125` |
| `ldp q3, q4, [x9]` | `0xad401123` |
| `ldr q5, [x9]` | `0x3dc00125` |

### 6 ジャイロ

ユーザ空間の `/dev/uinput` 注入ではなく、カーネル空間からシステムライブラリの BitTube 出口を hook する

```
対象シンボル android::BitTube::sendObjects(sp<BitTube> const&, void const*, size_t, size_t)
候補モジュール libsensor.so -> libsensorservice.so -> libui.so
```

位置特定 `filp_open` + `kernel_read` で対象 ELF を読み、`.dynsym` からシンボルオフセットを取り、対象プロセス内の該当モジュールのロード基底を足して実行時アドレスにする
`sendObjects` の引数（arm64）`x1` = events 配列、`x2` = count。`x0` は tube でデータ定位には使わない

2 段階

1. **位置特定段階** `sendObjects` に実行ブレークポイントを置き、初回ヒットで events バッファのアドレスを学習する
2. **定常段階** ただちに実行ブレークポイントを外し、`data[0..1]` への `LEN_8` 書込み監視点に切り替える

定常状態では WRP 1 本のみで、system_server 内に実行ブレークポイントは残らない。ヒット頻度は「sensor バッチごと」から「書換えごと」に下がる
自分の overflow ハンドラから perf イベントを解除するとデッドロックするため、切替は `gyro_set_velocity`（プロセス文脈、毎フレーム 1 回）で行う

イベント配置（`sensors_event_t`、0x68 バイト）

```
0x00 version | 0x04 sensor | 0x08 type | 0x10 timestamp
0x18 float data[16] | 0x58 flags
```

type は `SENSOR_TYPE_GYROSCOPE`(4) と `SENSOR_TYPE_GYROSCOPE_UNCALIBRATED`(16) を認識
どちらも x/y/z は `data[0..2]` で書換方法は同一
NDK の `ASensorEvent` は type を `0x04` に置くため、コードは `0x08` を先に試し、次に `0x04` を見る

**Z が動かないのは構造的** パラメータ制御ではない。実行ブレークポイントは `+0x18` に 8 バイト書き、WRP 窓 `LEN_8` は `data[0..1]` を覆い、クライアントのマスクは `0x3`
`+0x20`（z）はどの書込み窓にも入らない

WRP 再帰の安全性 自前の `copy_to_user_nofault` が監視点を再触发する
`user_mode(regs)` と per-CPU `gyro_wrp_busy` / `wrpbp_busy` の二重ガードで止める

`gyro_stop` / `gyro_resume` は書換の停止と再開のみで hook は切らない
`gyro_remove` で初めて解除する
対象プロセス終了時 `gyro_resume` は `-ESRCH`、未設置時は `-ENODEV` を返し、どちらも冪等

### 7 シャドウページとカーネル書込み保護

**シャドウページ PTE 操作** `wind_shadow_pte_set` / `wind_shadow_pte_get`
対象 `mm` のページテーブルを辿って PTE を求め、`(old & ~mask) | (val & mask)` で書換えて TLB を更新。無痕ブレークポイントとページ権限調整に使う
物理アドレス域は `KH_PTE_PA_MASK` で取り出し、新しい PTE は `KH_PHYS_TO_PTE` で組む
52 ビット PA の機種では物理アドレス上位 2 ビットが PTE の `[9:8]` にあり、単純な `| page_to_phys()` はエントリを壊す

**カーネルイメージの書込み保護回避** `wind_force_kernel_page_rw` / `wind_kernel_page_ro_state`
`set_memory_rw` はイメージアドレスを受け付けないため、自前でページテーブルを辿って書込み可能にする
`ro_state` はページテーブル経路を検証したうえで葉の AP[2] を読むだけで、何も書かずに読み取り専用状態を判定する

---

## シンボル戦略：ゼロ CRC + 実行時解決

2 本柱。どちらが欠けても実機では読み込めない

### ゼロ CRC

ビルド後に `__versions` / `__version_ext_crcs` / `__version_ext_names` の 3 セクション長を 0 にする
カーネルの `check_version()` は CRC 照合を丸ごと飛ばし、`pr_warn_once` を 1 回だけ残す
`__versions` セクションが存在するため `same_magic(has_crcs=1)` は vermagic の最初の空白以降の末尾 `SMP preempt mod_unload modversions aarch64` だけを比較する

| カーネル | ファイル | 版テーブルが空のときの分岐 | `same_magic` 第 3 引数 |
|---|---|---|---|
| 5.10.198 | `kernel/module.c` | `Broken toolchain` → `return 1` | `info->index.vers` |
| 5.15.149 | `kernel/module.c` | 同上 | 同上 |
| 6.1 | `kernel/module/version.c` | 同上 | 同上 |
| 6.6.66 | `kernel/module/version.c` | 同上 | 同上 |
| 6.12.58 | `kernel/module/version.c` | 同上、ext テーブル分岐もあり | 同上 |
| 6.18.21 | `kernel/module/version.c` | 同上、ext テーブル分岐もあり | 同上 |

適用範囲 この方式が省くのは「シンボル CRC が端末ごとに一致している必要」であり、vermagic は省かない
末尾（`SMP preempt mod_unload modversions aarch64`）は対象カーネルと 1 バイト単位で一致が必要
したがって同一シリーズ・同一 config 特性の機種や `LOCALVERSION` 接頭辞の違いは 1 つの ko で賄える
シリーズを跨ぐ場合（6.1 の ko を 6.12 へ）は `bad vermagic` で拒否される
前提として config が実機と同構成であること。そうでなければ CFI 型ハッシュや構造体配置が食い違い、読み込めた後に落ちる

実機 vermagic

```
6.12.58-android16-6-g7704a1ae279b-ab15213644-4k SMP preempt mod_unload modversions aarch64
```

### 実行時シンボル解決

GKI は KMI を凍結し、vendor モジュールはホワイトリスト交差のシンボルしか参照できない
本ドライバが必要とする 32 個の関数はそこに無く、直接参照すると

```
wind: Unknown symbol kthread_use_mm (err -2)
```

`err -2` は ENOENT。エクスポート表にその名前が無く、`insmod` は即座に失敗する
そこで全て `sym.c` 経由にした。`register_kprobe`（ホワイトリスト内）で名前からアドレスを解決し、ビルド時には CRC 参照を一切生成しない

```
mm/プロセス  mmput  kthread_use_mm  kthread_unuse_mm  remap_pfn_range
vmap        __get_vm_area_caller  free_vm_area  vunmap  ioremap_page_range  iounmap
ページ表    set_memory_rw  set_memory_ro  contpte_ptep_get  __arch_clear_user
ファイル    filp_open  kernel_read  file_path
デバイス    misc_register  misc_deregister
netlink     netlink_kernel_create  __netlink_kernel_create  netlink_kernel_release
            __netlink_kernel_release  netlink_unicast  __alloc_skb
ブレーク    register_user_hw_breakpoint  unregister_hw_breakpoint
            register_wide_hw_breakpoint  unregister_wide_hw_breakpoint
浮動小数点  fpsimd_preserve_current_state
nofault     copy_from_user_nofault  copy_to_user_nofault
```

解決結果は読み込み時に dmesg へ出る。失敗時は `wind: ... 未解析到` を出す
該当機能は縮退するが読み込み自体は成功する

### 互換層

`compat.h` に集約し、5.10 から 6.18 まで 1 ソースで扱う

| 項目 | 境界 | 扱い |
|---|---|---|
| `mmap_lock` | 5.8 | `mmap_read_lock` ↔ `down_read(&mm->mmap_sem)` |
| `kmap_local_page` / `kunmap_local` | 5.11 | `kmap_atomic` / `kunmap_atomic` へ退避 |
| `ptep_get` | 5.10 | 無い場合は `*(ptep)` |
| maple tree VMA 反復 | 6.1 | `KH_HAVE_VMA_ITER` |
| `get_user_pages_remote` 引数数 | 5.9 / 6.5 | 3 分岐 |
| `__flush_dcache_area` | 5.11 | `dc civac` インライン列 |
| `arm64_use_ng_mappings` | 6.1 / 6.6 / 6.12 | 版ごとに値取得 |
| `PTE_ADDR_MASK` | 5.11 で削除 | `KH_PTE_PA_MASK` の 4 段階 |
| 物理アドレスの PTE 格納 | 52 ビット PA | `KH_PHYS_TO_PTE` でビット配置を組む |
| 版ヘッダ | 6.10 で `linux/version.h` 削除 | `generated/uapi/linux/version.h` を参照 |

---

## ユーザ空間側

| ファイル | 役割 |
|---|---|
| `user/Wind-对接头文件.h` | `c_wind` 1 クラスに全機能：読み書き／バッチ／フレームキャッシュ／スナップショット／隠蔽／kgsl／ジャイロ／5 系統ブレークポイント |
| `user/Wind-drivers-netlink_v1.3.0.zip` | netlink クライアント例 |
| `user/Wind-Drivers-*.sh` | 各カーネル版の自己解凍インストーラ（5.10 / 5.15 / 6.1 / 6.6 / 6.12 / 6.18） |
| `user/Wind-Drivers-6.6.ko` | 6.6 用ビルド済みモジュール |
| `user/1.sh` | インストール補助 |

最小手順

```cpp
#include "wind.hpp"

c_wind &d = c_wind::instance(target_pid);   // 既定 netlink unit 31
int v = d.read<int>(base + 0x10);
d.write<int>(base + 0x10, 123);

d.frame_begin();                            // 同一フレーム内で同アドレスは 1 回だけ読む
uint64_t hp = 0;
d.frame_read(base + 0x100, hp);

d.snap_start(base + 0x200, 60);             // バックグラウンド 60Hz 更新
uint32_t cur = d.snap_value<uint32_t>();
d.snap_stop();
```

`sizeof(c_wind)` は約 48 KB。うち 47 KB はフレームキャッシュ表（2048 スロット）
ローカル実体はヒープに確保する

追跡方式を切り替える前に `bp_all_off()` を呼ぶ。前の系統が BRP/WRP を握ったままだと新しい系統が入らない

```cpp
d.bp_all_off();        // 0x3F、5 系統を一括解除
```

---

## ビルド

### A — GitHub Actions（推奨）

```
Actions -> build-wind-ko -> Run workflow -> カーネル選択 -> artifact を取得
```

マトリクスは 6 行、**すべてゼロ CRC 方式**（`zero_versions: "1"`）

```
android12-5.10.198
android13-5.15.149
android14-6.1
android15-6.6.66
android16-6.12.58-device      実機 config 整合
android17-6.18.21
```

### B — ローカルクロスビルド

```bash
export ARCH=arm64
export CROSS_COMPILE=aarch64-linux-gnu-
export LLVM=1 LLVM_IAS=1
make -j"$(nproc)" M=kernel modules
```

clang 14 以上（5.x は接頭辞なし LLVM ツールが必要）、または aarch64-linux-gnu-gcc
カーネルは `-mgeneral-regs-only` でビルドされるため、インラインアセンブリの FP/SIMD は `.inst` で書く必要がある。ニーモニック形式を通すのは gcc のみ

### 補助ツール

| ツール | 用途 |
|---|---|
| `tools/dump_kcrc.py` | カーネル CRC 表の `dump` / `harvest` / `merge` / `check` / `diff` / `count` |
| `tools/fix_versions.py` | symvers 表に従って `__versions` の CRC を書換え |
| `tools/fix_vermagic.py` | `.modinfo` をファイル末尾へ移し、対象 vermagic を書込む |
| `tools/extract_installer_ko.py` | 自己解凍 `.sh` から内蔵 ko と CRC 表を取り出す |
| `tools/zero_versions.py` | ゼロ CRC 方式。版セクション長を 0 にする（`-n` は確認のみ、書換え時は `.bak` を残す） |
| `tools/harvest_kcrc.sh` | 実機 vendor モジュールから CRC 表を一括採取 |
| `tools/windbench.cpp` | 通信路の負荷試験と発熱比較 |
| `tools/btf_offsets.py` | BTF から構造体オフセットを取得 |
| `tools/pad_info.sh` / `pad_pack.sh` / `pad_probe.sh` | 機種情報採取、梱包、検出 |
| `tools/diag_lockup.sh` | ハング現場の採取 |
| `extract_symvers.py` | ko から symvers を抽出 |

---

## ディレクトリ構成

```
Wind_driver/
├── kernel/                 モジュールソース（8067 行）
│   ├── entry.c             読込入口、通信路設置、ioctl ディスパッチ
│   ├── memory.c            読み書き、ページ表走査、ページマップキャッシュ、mmap
│   ├── process.c           モジュール基底、プロセス関連
│   ├── hide.c              ファイル・プロセス隠蔽（kprobe）
│   ├── breakpoint.c        5 系統ブレークポイント + UDP + シャドウページ
│   ├── gyro.c              ジャイロ hook（実行 bp → WRP 定常）
│   ├── kgsl.c              GPU kgsl hook
│   ├── ace.c               ACE 検出回避カモフラージュ（kprobe 10 個）
│   ├── nlcomm.c            netlink 通信路と op ホワイトリスト
│   ├── sym.c               ホワイトリスト外 32 シンボルの実行時解決
│   ├── comm.h              オペコードとプロトコル構造体
│   ├── compat.h            カーネル互換層（5.10 → 6.18）
│   ├── memory.h / breakpoint.h / gyro.h / nlcomm.h / process.h / kgsl.h / sym.h
│   ├── Kconfig
│   └── Makefile
├── user/                   クライアントヘッダ、サンプル、インストーラ
├── tools/                  ビルド・実機補助スクリプト
├── .github/workflows/      CI クロスビルド
├── CHANGES.md              ラウンドごとの変更と実機証跡
├── CHANGELOG.md            初期マイルストーン
├── HOOKPTE-対照.md         hook 実装の対照表
├── README.md               中国語
├── README.en.md            英語
├── README.ja.md            日本語（本ファイル）
├── README.ru.md / README.ko.md
└── LICENSE
```

---

## トラブルシュート

| 症状 | 確認先 |
|---|---|
| 読込時に `Unknown symbol ... (err -2)` | そのシンボルは未エクスポート。`sym.c` の実行時解決へ回す |
| `disagrees about version of symbol` | CRC 不一致。ゼロ CRC 方式にするか再ビルド |
| `version magic ... should be` | vermagic 不一致 |
| `insmod` 後に通信路が無い | `dmesg` の `transport`、`misc_register`、`netlink` |
| 通信路を開くとフリーズ再起動 | `transport=2` を使い、`allow_fops_hook=1` は避ける |
| 古い値が読める | `MODE_SAFE` へ切替、GPU / ION バッファか確認 |
| 隠蔽が効かない | `CONFIG_KALLSYMS_ALL` の有無、`filldir64` の可視性 |
| ジャイロが動かない | 対象が sensor を読むプロセスか、`sendObjects` が解決したか、`dmesg` で WRP が armed か |
| ブレークポイントが入らない | 先に `bp_all_off`、次に `OP_BP_DIAG` で BRP/WRP の残量確認 |
| シャドウページのヒット後に対象が落ちる | `KH_PTE_PA_MASK` / `KH_PHYS_TO_PTE` と実機 PA 幅の整合 |

ログのキーワード

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

## 既知の制限

- 隠蔽はカーネルのシンボル可視性に依存。`CONFIG_KALLSYMS_ALL` が無効だと `filldir64` が見つからない
- `/dev/null` fops 通信路は一部機種でフリーズ再起動の記録があり、既定では無効
- ゼロ CRC 方式が省くのはシンボル CRC のみ。vermagic 末尾、CFI、構造体配置の検査は省かない
- ゼロ CRC はカーネルシリーズを跨げない。6.1 の ko を 6.12 に置くと `bad vermagic` で拒否される
- `brkbp` は記録のみで、レジスタ書き戻しは未実装
- `hwbp` / `ptebp` は TID 限定が未実装で、ヒットはプロセス全体
- arm64 の実行ブレークポイントは `LEN_4` のみ。命令は 4 バイト境界に必要
- BRP 4 本・WRP 2 本はハード上限。5 系統を同時に使うと奪い合う
- `ptebp` は 1 スロット最大 8 ページ。ページが変わったら `OP_PTEBP_REFRESH`
- UDP ブレークポイントは IPv4 のみ。IPv6 経路は未接続
- 6.12.58 より前の成果物は `tools/device/` の config と CRC 表に依存する。機種を変える場合は再採取が必要

---

## 変更履歴

直近 8 ラウンド（第 50 → 第 57）

| ラウンド | 内容 |
|---|---|
| 50 | Keep クライアントを新機能へ全面接続 |
| 51 | 追跡方式切替時のスタック破壊 |
| 52 | `entry.c` の dispatch 重複とコンパイル／論理欠陥 3 件 |
| 53 | `breakpoint.c` のビルドエラーとシンボル欠落 |
| 54 | arm64 カーネルは浮動小数点レジスタを触れないため `.inst` 生エンコードへ |
| 55 | 5.10 / 6.12 / 6.18 の移植差（`kmap_local_page`、`PTE_ADDR_MASK`） |
| 56 | 6.10 以降に `include/linux/version.h` が無いため生成ヘッダへ |
| 57 | `kthread_use_mm` / `kthread_unuse_mm` 未エクスポートのため実行時解決へ |

詳細なラウンド記録と実機証跡は `CHANGES.md`
性能と発熱の比較は `tools/windbench.cpp`
