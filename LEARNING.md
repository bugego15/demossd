# ssdsim 学习指南 —— SSD 固件模拟器逐层解析

> 配套代码：本仓库（C 语言，约 5000 行，9 个源文件）。
> 本文档按"先看全局、再看结构、最后看流程"的顺序组织，适合用来学习
> 企业级 SSD 固件的核心设计：FTL 映射、写缓存、GC/磨损均衡、NAND 调度、
> 掉电保护（PLP）与日志式持久化、掉电恢复、完整性校验。

---

## 目录

- [0. 建议的阅读顺序](#0-建议的阅读顺序)
- [1. 整体设计架构](#1-整体设计架构)
- [2. 基础约定（读代码前必须懂）](#2-基础约定读代码前必须懂)
- [3. 数据结构大全（逐字段）](#3-数据结构大全逐字段)
- [4. 软件执行流程](#4-软件执行流程)
- [5. 关键函数调用链](#5-关键函数调用链)
- [6. 关键不变量与设计决策](#6-关键不变量与设计决策)
- [7. 配置参数速查](#7-配置参数速查)
- [8. 动手实验建议](#8-动手实验建议)

---

## 0. 建议的阅读顺序

1. 本文档第 1、2 章，建立全局概念；
2. `include/types.h` —— 所有共享类型与原型，是全项目的"地图"；
3. `src/main.c` —— 初始化顺序 + 主事件循环（软件骨架）；
4. 按数据流读：`host.c` → `ftl.c`（写缓存 + 分配）→ `journal.c`（持久化）→
   `nand.c`（时序）→ `gc.c` → `stats.c`；
5. 对照本文档第 5 章的调用链，在代码里逐条走一遍；
6. 做第 8 章的实验，改代码观察行为变化。

---

## 1. 整体设计架构

### 1.1 分层图

```
┌─────────────────────────────────────────────────────────────────┐
│  workload.c   负载生成器（fill / seq / rand / zipf / trace 回放）  │
│               维护 seq_ctr（写序号）、ack_seq[]（已确认账本）        │
└──────────────────────────┬──────────────────────────────────────┘
                           │ host_submit(type, lba, nlb, seq)
┌──────────────────────────▼──────────────────────────────────────┐
│  host.c       主机命令层：命令对象池、按页拆分、完成计数(left)、     │
│               掉电时清除未确认命令(host_purge)                      │
└──────────────────────────┬──────────────────────────────────────┘
                           │ ftl_read / ftl_write / ftl_trim_cmd
┌──────────────────────────▼──────────────────────────────────────┐
│  ftl.c        FTL 核心                                            │
│   ├─ 映射表 l2p / map_seq / 反向映射 ppa2lpn / OOB seq_nand        │
│   ├─ 块管理：blks[] 状态机、每 node 空闲池 pool[]、active[] 追加块   │
│   ├─ 写缓存 write-back cache（LRU）+ 背压等待队列 wait_cache        │
│   ├─ 等待队列 wait_jnl / wait_alloc（FIFO）                        │
│   └─ 脏表 dirty_bit[]（供检查点快照）                               │
└───────┬──────────────────────────────┬──────────────────────────┘
        │ ftl_map_commit → jnl_append   │ flash_submit
┌───────▼───────────────────────┐ ┌────▼───────────────────────────┐
│  journal.c  日志/检查点/PLP    │ │  gc.c  垃圾回收+静态磨损均衡     │
│  ├─ WAL 缓冲 JBUF → 日志段     │ │  ├─ 受害者选择（贪心最少有效页）  │
│  ├─ 段=数据页+提交页(CRC)      │ │  ├─ 搬迁流水线(读→写, ≤8 并发)  │
│  ├─ 双槽检查点 A/B + 提交页    │ │  ├─ 日志屏障后才允许擦除          │
│  ├─ PLP 断电序列/硬切          │ │  └─ 擦除完成 → 块回池 + 唤醒等待者 │
│  └─ 恢复：载入检查点+重放日志   │ └────────────────────────────────┘
└───────────────┬───────────────┘
                │ flash_submit(FOP_READ/PROG/ERASE, ppa, cb, arg)
┌───────────────▼──────────────────────────────────────────────────┐
│  nand.c       NAND 时序模型                                        │
│   ├─ dev_free[ch][die][plane]：介质忙到何时（虚拟 ns 时间轴）         │
│   ├─ ch_free[ch]：通道总线忙到何时（数据传输串行化）                  │
│   ├─ fop_list：所有在途操作（掉电时统一作废）                         │
│   └─ "时间序贪心"调度：谁先提交谁先占用介质，完成事件入堆               │
└───────────────────────────┬──────────────────────────────────────┘
                            │ ev_push(完成时间, 回调)
┌───────────────────────────▼──────────────────────────────────────┐
│  event.c      最小堆事件队列（离散事件仿真的心脏）                     │
│   g_now 只在 ev_run_one 弹出事件时推进 —— 没有事件就没有时间流逝        │
└──────────────────────────────────────────────────────────────────┘

横切模块：
  stats.c   全部计数器/延迟直方图/磨损扫描/最终报告/SMART
  types.h   全部共享类型、配置 cfg_t、统计 stats_t、函数原型
  main.c    初始化序列、主循环、1ms 后台定时器、drain 状态机、掉电注入
```

### 1.2 两个世界：易失态 vs 持久态

理解本模拟器（以及真实 SSD 固件）最重要的一点：**代码里的一切 RAM 状态都是
易失的，只有 NAND 上的内容才是持久的**。掉电恢复的全部工作就是"用持久态重建
易失态"：

| 易失（掉电即失） | 持久（NAND 上） | 恢复方式 |
|---|---|---|
| `l2p[]` / `map_seq[]` 映射表 | 检查点快照 + 日志段重放 | `jnl_recover()` |
| `blks[].free_ptr`（块写到哪了） | 每页 OOB 序号 `seq_nand[]` | 全盘 OOB 扫描 |
| `blks[].state` / `active[]`（哪些块是打开的） | 同上 | 扫描后重推（部分块重新开放为 active） |
| `pool[]` 空闲块池 | 同上 | fp==0 的块重新入池 |
| `ppa2lpn[]` 反向映射 | 同上 | 由 `l2p` 反推 |
| 写缓存 `cache[]` 脏页 | 无（若未刷写） | PLP 断电前刷写；否则丢弃 |
| 日志内存缓冲 `J.buf[]` | 未提交的日志段 | 丢弃（未提交=未发生） |
| `dirty_bit[]` 脏表 | — | 无需恢复（由 l2p 重算/重放结果决定） |

### 1.3 各源文件职责一览

| 文件 | 行数量级 | 核心职责 |
|---|---|---|
| `src/main.c` | ~430 | CLI/配置解析、初始化、主事件循环、1ms 后台定时器(bg_timer)、drain、掉电注入 |
| `src/event.c` | ~130 | 最小堆事件队列：push/cancel/pop/purge/run_until |
| `src/nand.c` | ~230 | NAND 阵列时序模型、fop 引擎、掉电作废(nand_purge)、诊断 |
| `src/ftl.c` | ~940 | 映射/分配/读写路径/写缓存/等待队列/恢复重建，最大的模块 |
| `src/gc.c` | ~300 | GC 与静态 WL 状态机：受害者选择、搬迁、擦除、回池 |
| `src/journal.c` | ~830 | WAL 日志、双槽检查点、PLP 断电序列、硬掉电、恢复、完整性校验 |
| `src/host.c` | ~130 | 主机命令对象池、页拆分、完成、掉电清除 |
| `src/workload.c` | ~230 | 五种负载生成器、fill 阶段、ack 记账（seq 分配） |
| `src/stats.c` | ~260 | 延迟直方图、磨损扫描、报告与 SMART 输出 |

### 1.4 全局状态（4 个跨模块变量）

| 变量 | 定义处 | 含义 |
|---|---|---|
| `cfg` | main.c | 配置副本（启动时从文件+CLI 载入，运行期只读） |
| `st` | stats.c | 全部统计计数器（只增） |
| `g_now` | event.c | 虚拟时钟，单位纳秒；只在事件弹出时推进 |
| `sim_state` | main.c | `ST_RUNNING/ST_PLP/ST_OFF/ST_DRAIN/ST_DONE`，大量门控依赖它 |

另有两个 drain 握手标志 `g_drain`（0=未开始 1=排空中 2=日志已提交）与
`g_flushing`（drain 的异步子步骤进行中），定义在 journal.c、由
`power_hard_cut()` 复位。

## 2. 基础约定（读代码前必须懂）

### 2.1 PPA 编码与索引换算

物理页地址（PPA）是一个 32 位打包整数：

```
 bit 31..28  bit 27..24  bit 23..12   bit 11..0
┌──────────┬──────────┬────────────┬──────────┐
│  channel │   die    │   block    │   page   │
└──────────┴──────────┴────────────┴──────────┘
   4 bit      4 bit      12 bit      12 bit
```

- `PPA_INVALID = 0xFFFFFFFF` 表示"无映射"。
- **plane 隐含在 block 号最低位**：`plane = blk & 1`（每 die 2 个 plane）。
- **node** = `ch * dies + die`，即"die 级并行单元"，是分配/GC 的并行粒度。
- 线性下标（用于大数组）：
  - `pidx_of(ppa)` = `((node * blocks_per_die + blk) * pages_per_block + page)`，
    索引 `ppa2lpn[]`、`seq_nand[]`；
  - `bidx_of(ppa)` = `node * blocks_per_die + blk`，索引 `blks[]`；
  - `blk_base_of(bidx)` 反向把 bidx 变成 page0 的 PPA。

### 2.2 块状态机（`blks[].state`）

```
              开始写入第一页                写满 pages_per_block
   BLK_FREE ────────────────► BLK_ACTIVE ───────────────────► BLK_CLOSED
      ▲                          │   ▲                            │
      │       GC 擦除完成         │   │ 恢复时重新开放为追加块        │
      └──────────────────────────┘   └──────────────────────────────┘
                                    （ftl_free_block）    （ftl_rebuild_from_map）

   BLK_META  元区块（日志+检查点，ch0/die0，永不回池）
   BLK_BAD   坏块（按 bad_block_pct 随机制造，永不使用）
```

- `BLK_CLOSED` 是 GC 受害者的**唯一候选**（有数据但不再追加）。
- `BLK_ACTIVE` 表示某 (流, node) 正在此块尾部追加，见 `active[]`。

### 2.3 时间模型：离散事件仿真

- 没有真实时钟、没有 sleep。`g_now` 是虚拟纳秒。
- 一切未来的事情 = `ev_push(完成时刻, 回调, 参数)` 入最小堆。
- 主循环每次弹出**堆顶（最早）**的事件执行，`g_now` 直接跳到该时刻。
- NAND 占用：`flash_submit()` 立即计算"这个操作几点能做完"，把对应
  plane/通道的 `dev_free/ch_free` 时间戳向后推（**时间序贪心**：先到先服务），
  完成回调按完成时刻入堆。因此不需要真实的"NAND 线程"。

### 2.4 序号模型（正确性的骨架）

三个序号贯穿全项目，含义不同：

| 名字 | 作用域 | 含义 |
|---|---|---|
| `seq`（写序号） | `wl_next_seq()` 每次主机写/trim 单调 +1 | 给每次写一个全序，用于比较新旧 |
| `lsn`（日志序号 `J.lsn`） | 日志记录的逻辑序号 | 日志记录顺序、`covered_lsn` 覆盖判断 |
| `seg_seq`（段号 `J.seg_seq`） | 每个日志段 +1 | 恢复时按段序重放 |

持久化侧的三个数组（每页/每逻辑页一份）：

- `ack_seq[lpn]`：**主机确认账本**（workload 侧）。写被缓存 ack 时记下 ——
  "固件承诺这一页的最新内容序号是它"。
- `map_seq[lpn]`：映射表认为该页应有的序号（RAM，掉电可失，靠日志重建）。
- `seq_nand[pidx]`：**NAND 页 OOB 里的真实序号**（持久，0=从未编程）。

**校验规则**（`verify_scan()`）：对每个 `ack_seq>0` 的逻辑页，若
`seq_nand[当前映射页] < ack_seq` 则该页 **LOST**（已确认但没落盘）。
PLP 正确 = 掉电后 LOST==0；关掉 PLP 的对照组 LOST>0。

### 2.5 元区（meta region）布局

元数据固定放在 **ch0/die0 的前 `meta_blocks` 个块**（`ftl_format()` 会按容量
自动核算并扩张）：

```
block:  [0 ........ jnl_blocks)  [jnl_blocks .......)  [........ +ckpt_blks)  [........ +ckpt_blks)
        └──── 日志区（环形）────┘   检查点槽 A            检查点槽 B
```

- 日志区按"段"追加（写满一个块先擦除 → `block_safe()` 保证被覆盖的记录已被
  检查点覆盖）；
- 检查点双槽轮换（`slot = ckpt_count & 1`），**新槽完整落盘（含提交页）之前
  旧槽一直有效** —— 这就是掉电后"挑 slot_seq 更高的槽"的依据。

---

## 3. 数据结构大全（逐字段）

### 3.1 配置 `cfg_t`（types.h:51，实例 `cfg`，main.c 载入）

| 字段 | 类型 | 含义 |
|---|---|---|
| `channels` / `dies_per_ch` / `blocks_per_die` / `pages_per_block` | int | 几何：4×4×64×512 |
| `page_size` / `sector_size` | int | 页 16KB / 扇区 512B（每页 32 扇区） |
| `op_ratio` | double | 过 provision 比例（0.07 → 用户容量 = raw×0.93） |
| `meta_blocks` | int | 元区块数（日志+检查点，位于 ch0/die0） |
| `bad_block_pct` | double | 格式化时随机判坏的概率 |
| `tprog_ns` / `tr_ns` / `tbers_ns` | uint64 | 编程 800µs / 读 70µs / 擦除 3ms |
| `bus_mts` | int | NAND 总线速率 MT/s（数据搬运时间=页大小/速率） |
| `cmd_ns` | uint64 | 每条命令开销 |
| `cache_lat_ns` | uint64 | SRAM 缓存访问延迟（5µs，写 ack 时刻） |
| `cache_pages` | int | 写缓存容量（页数，0=直写） |
| `cache_flush_pct` | int | 脏页超过容量的这个百分比就后台刷 |
| `queue_depth` | int | 主机队列深度（在途命令上限） |
| `flush_out_max` | int | 缓存→NAND 后台刷写在途上限（背压阈值） |
| `plp` / `plp_budget_ns` | int/uint64 | 掉电保护开关 / 电容预算（50ms） |
| `ecc_rate` | double | 每次读触发 read-retry 的概率 |
| `gc_trigger_pct` | int | 空闲块低于数据块总数这个百分比时启动 GC |
| `gc_victim_max_pct` | int | 贪心选择时优先有效页比例低于此值的受害者 |
| `gc_max_out` | int | GC 搬迁在途操作上限（流水线深度） |
| `wl_threshold` | int | 最大/最小 PE 差超过此值触发静态 WL |
| `endurance` | int | SMART 用的寿命（PE 次数） |
| `ckpt_interval` | uint64 | 两次检查点之间允许的日志记录数（节流） |
| `jnl_flush_at` | uint64 | 日志缓冲攒够这么多记录就写一段 |
| `reserve_blocks` | int | 预留块（host 分配止步线，给 GC 留余量） |
| `seed` | uint64 | 随机种子（RNG 独立于 libc rand） |
| `workload`/`trace_path`/`io_sectors`/`read_ratio`/`zipf_s` | — | 负载参数 |
| `fill`/`fill_pct` | int/double | 是否预填充 / 填充比例 |
| `n_ios` | long | 稳态 IO 数 |
| `verify` | int | 跑完做完整性校验 |
| `quiet` | int | 抑制启动横幅 |
| `trace_v` | int | `-v` 详细诊断 |
| `power_cuts`/`cut_interval_ns` | — | 掉电注入次数/基础间隔（实际=间隔/次数） |

### 3.2 统计 `stats_t`（types.h:110，实例 `st`）

| 组 | 字段 | 说明 |
|---|---|---|
| host | `host_reads/writes/trims`, `host_r/w_bytes` | 命令数与字节数 |
| | `cmds_aborted` | 掉电时被清除的未确认命令 |
| | `rd/wr_lat_n/sum`, `rd/wr_hist`, `*_lat_max` | 延迟直方图素材（报告算 avg/p50/p99/max） |
| nand | `nand_read/prog/erase/retry` | 介质操作数（retry=read-retry 次数） |
| | `host_wr_pages`, `cache_wr_pages` | host 路径编程页数 / 被缓存吸收的写 |
| gc/wl | `gc_count/moved/erased/alloc_fail`, `wl_count/moved` | GC 与 WL 计数 |
| | `gc_busy_ns` | GC 累计耗时 |
| cache | `cache_rd_hit/miss`, `cache_wr_hit/new` | 读命中/未命中、写命中已有条目/新条目 |
| | `cache_evict/flushed/dirty_peak` | 驱逐数/刷写数/脏页峰值 |
| journal | `jnl_records/segments/bytes` | 日志记录/段/字节 |
| | `ckpt_count/pages` | 检查点次数/页数 |
| power | `power_cuts` | 掉电次数 |
| | `plp_flushed` | PLP 序列中刷写的缓存页 |
| | `plp_dropped` | 断电丢失的脏缓存页 |
| | `plp_budget_hits` | 电容预算耗尽（退化为硬切）次数 |
| | `recovery_count/ns/pages` | 恢复次数/估时/扫描页数 |
| integrity | `verified_ok/lost_writes/corrupted`, `lost_first[10]` | 校验结果 + 前 10 个丢失 LPN |
| wear | `pe_min/max/sum`, `pe_hist[16]` | 磨损统计（直方图桶=十进制数量级） |
| | `bad_blocks` | 坏块数 |
| timing | `sim_ns` | 仿真总时长 |

### 3.3 事件 `ev_t`（event.c 引擎）

| 字段 | 含义 |
|---|---|
| `t` | 触发时刻（虚拟 ns） |
| `id` | 全局递增序号（同刻事件的稳定排序键） |
| `cancelled` | 懒取消标记：弹出时才真正 free |
| `fn` | 回调 `void (*)(void*)` |
| `arg` | 回调参数（常是 fop/pio/cmd 指针） |

堆实现：`heap[]` 数组最小堆 + `heap_up/heap_down`；`ev_purge()` 掉电时
把堆里所有事件 free 掉（**持有事件句柄的模块必须自己失效句柄**，
见 `bg_ev_invalidate()`）。

### 3.4 NAND 层（nand.c）

**`fop_t`（在途 flash 操作）**

| 字段 | 含义 |
|---|---|
| `type` | FOP_READ / FOP_PROG / FOP_ERASE |
| `ppa` | 目标物理页（ERASE 用其 block 基址） |
| `cb` | 完成回调 `void (*)(int err, void *arg)`；err<0 表示被掉电作废 |
| `arg` | 回调上下文（gc_move_t* / page_io 上下文等） |
| `ev` | 完成事件句柄（nand_purge 时 cancel） |
| `next`/`prev` | 在途链表双向指针（完成时 O(1) 摘除） |

**调度状态**（数组，非结构体）：

- `dev_free[node*2 + plane]`：该 plane 忙到什么时刻（tR/tPROG/tBERS 占用）；
- `ch_free[ch]`：该通道总线忙到什么时刻（读数据/写数据的搬运串行化）；
- `fop_list`：所有在途 fop；`purging`：掉电清场中标记（清场中不调用户回调）；
- `node_prog[64]`：每 die 编程计数（负载均衡诊断，`-v` 的 `[nodes]` 行）。

时序计算规则（`flash_submit`）：
- READ = `tR`（介质）+ `页大小/总线速率 + cmd_ns`（总线搬运），read-retry 再加一次 tR；
- PROG = 总线搬运 + `tPROG`；
- ERASE = `tBERS`（无总线搬运）。
介质占用 `dev_free` 只算阵列时间，总线占用 `ch_free` 算搬运时间 —— 
同一 die 的阵列操作不能重叠，同一通道的数据搬运不能重叠。

### 3.5 FTL 层（ftl.c + types.h）

**块元数据 `blk_meta_t`（types.h:205，数组 `blks[bidx]`）**

| 字段 | 含义 |
|---|---|
| `erase_cnt` | 擦除次数（PE 磨损账本，恢复后仍在 —— 真实固件放 SLC 元数据区） |
| `valid` | 该块中仍被 `l2p` 引用的用户页数（GC 贪心的依据） |
| `free_ptr` | 下一个可编程页号；恢复时由 OOB 扫描重建（=最后已编程页+1） |
| `state` | BLK_FREE/ACTIVE/CLOSED/META/BAD |

**全局映射/验证表**（ftl.c:8-13 定义，types.h:212-217 声明）

| 数组 | 索引 | 含义 |
|---|---|---|
| `l2p[lpn]` | 逻辑页 | 逻辑→物理映射（核心，掉电可失，日志重建） |
| `map_seq[lpn]` | 逻辑页 | 该映射对应的写序号（重放时做新旧裁决） |
| `ack_seq[lpn]` | 逻辑页 | 主机已确认的最新序号（校验基准，仅 RAM） |
| `ppa2lpn[pidx]` | 物理页 | 反向映射（GC 判断有效性用；恢复时由 l2p 重建） |
| `seq_nand[pidx]` | 物理页 | OOB 序号（持久；0=从未编程；恢复扫描的数据源） |
| `dirty_bit[lpn]` | 逻辑页 | 检查点脏表：自上次检查点快照以来改过的页 |

**分配子系统**（静态变量）

| 变量 | 含义 |
|---|---|
| `pool[node]` / `pool_n[node]` | 每 node 一个空闲块栈（LIFO，bidx） |
| `free_total` | 全局空闲块数（= Σpool_n，host 止步线的判断依据） |
| `reserve_blocks` | 预留块数（host 类分配 `free<=它` 即失败） |
| `active[slot*node_count+node]` | 每(流槽,node)一个追加块基址；槽=STREAM_COLD/HOT/SLOT_GC |
| `rr_node` | 主机分配的 node 轮转游标（写流量均匀铺到所有 die） |

**`page_io_t`（页级 I/O 对象，贯穿读/写/刷写路径）**

| 字段 | 含义 |
|---|---|
| `type` | CMD_READ / CMD_WRITE |
| `lpn` | 目标逻辑页 |
| `seq` | 该页所属主机写的序号 |
| `partial` | 首/尾页未整页覆盖（需 RMW） |
| `rmw` | 编程前先做一次读（RMW 的读半段） |
| `bg` | 后台刷写页（无宿主命令；计入 `bg_out` 背压） |
| `cf` | 属于 flush-all 批次（PLP/drain 用；计入 `cf_out`） |
| `gc_dest` | 允许 GC 式分配（绕过预留检查） |
| `ppa` | 已分配的目的物理页 |
| `cmd` | 所属主机命令（后台刷写为 NULL） |

**等待队列 `wait_ent_t`**

| 字段 | 含义 |
|---|---|
| `pio` | 等待中的页 I/O |
| `next` | 链表下一跳 |

三条 FIFO 队列（**新来者入队尾，失败重试者回队首**，见 §6.1）：
- `wait_jnl`：日志缓冲快满，等日志腾出空间；
- `wait_alloc`：空闲块不足（`free<=reserve`），等 GC 腾块；
- `wait_cache`：缓存满且背压到顶（`bg_out>=flush_out_max`），等后台刷写腾位。

**刷写批次跟踪**：`cf_done/cf_arg`（flush-all 完成回调）、`cf_out`
（批次在途数）、`bg_out`（全部后台刷写在途数，背压依据）。

**写缓存条目 `cent_t`**

| 字段 | 含义 |
|---|---|
| `used` | 槽位占用 |
| `lpn` | 缓存的逻辑页 |
| `seq` | 该页最新写序号 |
| `last` | LRU 时间戳（全局 `cache_tick` 递增取号） |

`cache[]` 采用**哈希桶 + 桶内链式探测**（`cache_hash`），容量 `cache_cap`
（=cache_pages），全满且无法驱逐时 `ftl_cache_insert()` 返回 -1（背压）。

### 3.6 GC 层（gc.c）

**GC 全局状态 `G`**

| 字段 | 含义 |
|---|---|
| `busy` | 有周期在跑（同一时刻最多一个） |
| `mode` | 0=垃圾回收 1=静态磨损均衡 |
| `victim` | 受害者块基址 PPA |
| `scan` | 扫描游标：下一个要看的页下标（0..pages_per_block） |
| `moves_out` | 在途搬迁（读+写）数，流水线上限 `gc_max_out` |
| `t0` | 周期开始时刻（统计 busy 时长） |

**`gc_move_t`（单页搬迁）**

| 字段 | 含义 |
|---|---|
| `lpn` | 要搬的逻辑页 |
| `src` | 源物理页 |
| `dst` | 目的物理页（读完成时才分配） |
| `seq` | 该页 OOB 序号（搬迁后写回 OOB） |
| `next` | pending 链表指针 |

`pending`：读完成但**分配不到目的页**的搬迁暂存链（系统活着的条件下
应当为空；卡住=死锁征兆）。

### 3.7 日志/检查点层（journal.c）

**`jrec_t`（16 字节日志记录：一次映射变更）**

| 字段 | 含义 |
|---|---|
| `lpn` | 逻辑页 |
| `ppa` | 新物理页 |
| `seq` | 写序号（重放裁决依据） |

**`pg_hdr_t`（48 字节页头，每张元数据页都有）**

| 字段 | 含义 |
|---|---|
| `magic` | JSEG=日志数据页 / JCMT=日志段提交页 / CKPT=检查点数据页 / CKCM=检查点提交页 |
| `count` | 本页记录/条目数 |
| `seg_seq` | 所属段号（数据页与提交页必须一致才能认定完整段） |
| `lsn0` | JSEG: 首记录 lsn；CK*: 覆盖到的 lsn |
| `aux` | 备用（CKCM 里放覆盖 lsn） |
| `crc` | 头+载荷 CRC32（`hdr_ok()` 校验） |
| `pad` | 对齐 |

**日志写入状态 `J`**

| 字段 | 含义 |
|---|---|
| `buf[JBUF]/nbuf` | 内存日志缓冲（8192 条）与条数 |
| `writing` | 有一段正在写 NAND（同一时刻一段） |
| `seg/seg_n/seg_pages/cur` | 当前段快照、记录数、页数、写到第几页 |
| `seg_seq/lsn` | 当前段号 / 全局日志序号（下一条记录取号） |
| `wr_blk/wr_page` | 区内写游标（哪个块哪个页） |
| `erase_done` | 当前块本轮已擦除（进入新块时擦一次，之后顺序写） |
| `covered_lsn` | **已被持久检查点覆盖的日志水位**（只有检查点提交页落盘后才发布） |
| `wait_ckpt` | 段写因 `block_safe()` 不满足而挂起，等检查点完成 |
| `gen` | 世代号：每次易失复位 +1，作废旧的回调等待链 |

**检查点状态 `C`**

| 字段 | 含义 |
|---|---|
| `busy` | 有检查点在跑 |
| `snap/nsnap` | 脏表快照（jrec 数组）与条数 |
| `pages/cur` | 快照总页数 / 写到第几页 |
| `lsn/slot_seq` | 快照时的日志水位 / 槽版本号（恢复时比大小） |
| `slot` | 写哪个槽（0/1 轮换） |
| `erase_left/erased` | 槽擦除阶段状态 |
| `gen` | 世代号（同 J.gen 用途） |

**等待链 `cbw_t`**（callback waiter）：`{cb, arg, next}`，两个链表
`jnl_w`（等日志空闲：GC 擦除屏障、PLP/drain 的落盘屏障）与
`ckpt_w`（等检查点完成）。`cbw_run()` 执行时校验世代号，复位后旧链直接作废。

**元数据映像 `meta_img`**：整个元区在 RAM 里的镜像（`pg_ptr(blk,page)`
直达某页）。写元数据页 = 改镜像 + 提交一个 PROG fop —— 掉电时未提交的
改写天然丢失，与真实 NAND 语义一致。

### 3.8 主机/负载层（host.c / workload.c）

**`host_cmd_t`（主机命令）**

| 字段 | 含义 |
|---|---|
| `type` | CMD_READ/WRITE/TRIM |
| `lba`/`nlb` | 扇区起址/扇区数 |
| `submit_ns` | 提交时刻（延迟统计起点） |
| `seq` | 写/trim 序号（workload 分配） |
| `first_lpn`/`last_lpn` | 折算后的逻辑页范围（含） |
| `partial` | 首尾页非整页 |
| `left` | 还没完成的 page_io 数（减到 0 即命令完成） |
| `acked` | 已确认标记 |
| `next` | 在途链表/空闲链表指针 |

host.c 其余状态：`out_list`（在途）、`free_list`（对象池）、`out_n`
（深度计数）、`stopped`（PLP/掉电时停发）。

**workload.c 状态**：`generated/target`（已发/目标命令数）、`phase`
（0=fill 1=稳态 2=完成）、`seq_ctr`（写序号发号器）、`fill_cursor`
（填充游标）、`seq_cursor`（顺序负载游标）、`trace_f`（回放文件）、
`rng`（独立 xorshift）、`zipf_cdf`（冷门/热门页分布表）。

### 3.9 数据结构关系总图

```
 workload.c ──发出──► host_cmd_t ──拆页──► page_io_t ──► (三选一)
                                                     │
              ┌──────────────────────────────────────┤
              ▼                                      ▼
        写缓存 cent_t (LRU)                    直通/刷写路径
              │ 驱逐                                  │
              ▼                                      │
        page_io_t(bg=1) ◄────────────────────────────┘
              │  等待三队列: wait_jnl / wait_alloc / wait_cache
              ▼
        ftl_alloc ──► active[] / pool[] ──► blks[free_ptr++]
              ▼
        flash_submit ──► fop_t ──► dev_free/ch_free(时序)
              ▼ 完成
        ftl_map_commit ──► l2p/map_seq 更新
              │            └──► dirty_bit 置位
              ▼
        jnl_append ──► J.buf ──(攒够 jnl_flush_at)──► start_segment
                                    │
                                    ▼
                        meta_img 上的 JSEG 页 + JCMT 提交页
                                    │ (covered_lsn 在此之前不动)
        ckpt_start(快照 dirty_bit) ─► CKPT 页 + CKCM 提交页
                                    │ 提交完成 → 发布 covered_lsn、清脏位
        GC: victim(blks CLOSED) ──读──► gc_move_t ──写──► l2p 更新
                              └── jnl_flush 屏障 ──► ERASE ──► ftl_free_block(回池)

 掉电: cut_ev ─► plp_start(刷缓存→等bg_out→jnl_flush) ─► power_hard_cut
                          └ 超时: plp_deadline_check ─► 硬切
       power_hard_cut ─► nand_purge(fop 全废,cb(-1)) + gc_abort + host_purge
                       + ev_purge + bg_ev_invalidate + jnl_reset_volatile(gen++)
                       + 缓存丢弃 + ftl_drop_waits
                       ─► ftl_boot_recover ─► jnl_recover(检查点+重放)
                                           ─► ftl_rebuild_from_map(OOB 扫描)
```

## 4. 软件执行流程

### 4.1 启动初始化序列（main.c）

```
set_defaults() → 解析 CLI 找 -c → load_config(文件) → 应用 CLI 覆盖
srand(seed)
ev_init()        事件堆
stats_init()     统计清零
nand_init()      dev_free/ch_free 分配
ftl_format()     blks/pool/active 初始化、坏块注入、用户容量核算、meta 区自扩张
ftl_cache_init() 缓存槽分配
jnl_init()       元区布局核算(epp=1021 条/页)、meta_img、检查点槽定位
gc_init()        GC 状态清零
host_init()      命令池
wl_init()        负载初始化（是否 fill、trace 打开等）
sim_state = ST_RUNNING
打印横幅 → bg_timer_reschedule()（1ms 后台定时器）
schedule_next_cut()（若有掉电注入）→ wl_kick()（发第一批命令）
```

> 真实固件 boot 也大致是这个顺序：硬件初始化 → FTL 元数据定位 → 
> 映射表恢复（本模拟器把恢复只放在掉电后，开机默认是空盘）。

### 4.2 主事件循环（main.c:387）

```
for (;;) {
    wl_kick();                       // 队列有空就发新命令（workload 自己有节奏）
    if (未排空 && 负载发完 && 主机在途为0 && 无刷写批次) {
        if (cfg.verify) { g_drain=1; drain_step(); continue; }  // 优雅排空
        break;                       // 不校验直接结束
    }
    if (g_drain==2 && !g_flushing) break;   // 排空完成
    if (堆空) {
        if (g_drain==1) drain_step();       // 排空自驱动
        if (堆仍空) { 负载完? break : 报告"stall"并退出; }
        continue;
    }
    ev_run_one();                    // 弹出最早事件执行，g_now 跳到该时刻
}
```

仿真结束条件二选一：负载完成（+可选排空校验）或事件耗尽。

### 4.3 1ms 后台定时器（bg_timer，主循环之外的"心跳"）

每 1ms（模拟时间）触发一次，只在 `ST_RUNNING` 做正事：

```
ftl_cache_bgflush();  // 脏页超 cache_flush_pct% 就驱逐刷写，
                      // 但受 bg_out < flush_out_max 背压限制
gc_kick("bg");        // 空闲块低于 trigger 就开 GC（排空期间门控）
wl_check();           // PE 散布超阈值就做静态 WL（排空期间禁止）
ckpt_kick();          // 日志水位够高就做检查点（PLP/掉电期间禁止）
(-v 时打印 [t]/[nodes] 诊断行)
bg_timer_reschedule(); // 重新武装
```

`-v` 的 `[t]` 行各字段：`st` 状态、`ev` 事件总数、`out` 主机在途、
`wr/prog` 主机写/编程数、`ckpt/jnlseg` 检查点/日志段数、`dirty` 脏表、
`cache_d` 缓存脏页、`bg_out` 后台刷写在途、`waitj/waita/waitc` 三条等待队列
长度、`free/pool` 空闲块、`gcb` GC 忙、`jnl` 日志状态位（bit0 writing、
bit1 wait_ckpt、bit2 检查点忙、bit3 有屏障等待者、bit4 缓冲非空）、
`planes` 忙介质数、`backlog` 最深排队时间。

### 4.4 排空 drain 状态机（跑完后为校验收尾）

```
g_drain: 0 ──(负载完+主机空)──► 1 ──(全部落盘+日志提交)──► 2(结束)
                g_flushing 在每个异步子步骤置位/复位做握手

drain_step():
  ① 缓存有脏页? → flush_all(全量刷写, 回调 drain_cache_done) 异步等
  ② bg_out>0 或 gc_busy? → 1ms 后自轮询（GC 周期必须收尾，报告才完整）
  ③ jnl_flush(drain_jnl_done) → 提交日志 → g_drain=2
```

注意门控配合：排空期间 host 已停，`gc_kick` 只在"分配饥饿"
（`free<=reserve+1`）时才允许 GC 起新周期 —— 否则 GC 会自我续跑、排空永不收敛。

### 4.5 掉电注入与断电路径

```
schedule_next_cut(): 间隔 = cut_interval_ns / power_cuts，加 50%~100% 抖动
cut_ev(): st.power_cuts++
    ├─ cfg.plp && ST_RUNNING ─► plp_start()        优雅断电序列
    └─ 否则 ─► power_hard_cut()                     直接硬切
恢复完成后 main_on_cut() → schedule_next_cut() 排下一次掉电
```

**PLP 序列**（电容还在，力争数据不丢）：

```
plp_start():
  sim_state=ST_PLP; host_stop(); 推 deadline 事件(电容预算)
  ftl_cache_flush_all(plp_cache_done)     全量刷缓存（cf 批次）
plp_cache_done → plp_progress:
  bg_out>0? 每 50µs 轮询等待（已驱逐未编程的页也要落盘）
  jnl_flush(plp_jnl_done)                 日志提交（屏障）
plp_jnl_done → power_hard_cut()           一切持久化后立即断电
deadline 先到 → st.plp_budget_hits++；缓存是 RAM 一并丢弃 → power_hard_cut()
```

**硬切**（`power_hard_cut()`，恢复的入口）：

```
sim_state=ST_OFF; host_stop()
nand_purge()         所有在途 fop 作废（cb(err=-1)，完成事件作废）
gc_abort()           GC 周期状态清零（pending 搬迁丢弃）
host_purge()         未确认命令清除（计入 cmds_aborted）
ev_purge()           事件堆清空
bg_ev_invalidate()   bg 定时器句柄失效（防止悬垂指针写内存——曾经的堆损坏 bug）
jnl_reset_volatile() 日志易失态复位：writing=0、wait_ckpt=0、gen++、等待链作废
(非 PLP 时) ftl_cache_drop_all()   脏缓存丢失（plp_dropped++）
ftl_drop_waits()     三条等待队列连同 pio 一起释放
ftl_boot_recover()   ★ 重建，见 4.6
sim_state=ST_RUNNING; host_resume(); wl_kick(); gc_kick(); bg_timer_reschedule()
```

### 4.6 恢复流程（ftl_boot_recover → jnl_recover + ftl_rebuild_from_map）

```
① jnl_recover()  —— 重建映射表
   扫描两个检查点槽，挑 slot_seq 更高的有效槽，重放其快照 → l2p/map_seq
   （此时 J.lsn/covered_lsn 恢复到该检查点水位）
   扫描日志区所有 JCMT 提交页，按 seg_seq 排序
   对每段：验证其全部 JSEG 数据页完好（magic/seg_seq/count 匹配）——
           不完好=撕裂段（提交页在但数据没写完的组合不存在，见 §6.4），跳过
   逐条重放：仅当 e.seq > map_seq[lpn] 才更新 l2p（新旧裁决）
   记录最后一个提交段的位置 → 推进 J.lsn/covered_lsn/写游标到安全点

② ftl_rebuild_from_map() —— 重建全部易失分配态
   OOB 扫描每个数据块：free_ptr = 最后已编程页+1；fp==0 → BLK_FREE 入池，
                        否则 BLK_CLOSED（GC 可回收）
   由 l2p 反推 ppa2lpn[] 和每块 valid 计数
   active[] 清空后，把每个 node 最空的至多 NSLOTS 个部分块重新开放为 active
                        （空闲空间不再搁置；GC 在池空时也有落脚点）
```

`st.recovery_pages/ns` 记录扫描量与估时（报告里的 est_time）。

### 4.7 结束与报告

排空/结束 → `verify_scan()`（逐 lpn 比对三序号，得 OK/LOST）→
`wear_scan()`（全块 PE 扫描、直方图）→ `print_report()`（host/NAND/FTL/
缓存/日志/掉电/完整性/磨损+SMART 分节输出）。退出码：校验有丢失=1。

---

## 5. 关键函数调用链

以下均为"从上到下"的完整链路，箭头=调用，缩进=从属阶段。

### 5.1 主机写路径（write-back cache）

```
main 循环 wl_kick()
 └► host_submit(CMD_WRITE, lba, nlb, seq)          host.c
     └► ftl_write(lpn, seq, stream, partial, cmd)  每页一次
         └► cache_admit(pio)                       ftl.c
             ├─ ftl_cache_insert()
             │   ├─ 已有同 lpn → 更新 seq（cache_wr_hit）→ 返回 0
             │   ├─ 有空槽 → 占用（cache_wr_new++）→ 返回 0
             │   ├─ 满 & bg_out>=flush_out_max → 返回 -1（背压）
             │   └─ 满 & 有预算 → LRU 驱逐:
             │        cache_evict → ftl_cache_flush_page(lpn,seq,cf?)
             │        → 生成 bg pio（bg_out++/cf_out++）→ ftl_direct_start()
             │        → 然后占槽成功
             ├─ 成功 → ev_push(g_now+cache_lat_ns, pio_complete_ev)
             │        （5µs 后模拟 SRAM 访问完成：主机命令 left--，减到0 →
             │          host_cmd_done → wl_on_complete → wl_ack_range
             │          → ack_seq[lpn]=seq（确认账本）→ wl_kick 续发）
             └─ 失败(-1) → wait_push(&wait_cache)（FIFO 队尾）
                          由 pio_finish(后台页完成) 或 bgflush 里的
                          cache_waiters_retry() 逐个重试

后台页落盘（被驱逐的脏页，ftl_direct_start）：
 ftl_direct_start(pio)
  ├─ 日志缓冲快满 → wait_push(&wait_jnl)（日志提交后 ftl_jnl_retry_waiters 唤醒）
  ├─ ftl_alloc(...): active 块有位→直接用；否则从 node 池弹块（free<=reserve 失败）
  │    失败 → wait_push(&wait_alloc)（GC 擦除回池 ftl_free_block 时唤醒）
  ├─ partial → 先提交 READ（RMW 读半段），读完再 PROG
  └─ flash_submit(FOP_PROG, ppa, pio_prog_cb, pio)
       └─ (nand.c) 算时序→完成事件入堆
            └─ pio_prog_cb: ftl_map_commit(lpn, ppa, seq, is_gc=0)
                 ├─ l2p/map_seq 更新；dirty_bit 置位；旧页 invalid
                 └─ jnl_append(lpn, ppa, seq)        见 5.4
               pio_finish(pio): bg_out--; cache_waiters_retry();
                                cmd->left--（直写路径）; cf_out--; cf_check()
```

### 5.2 主机读路径

```
host_submit(CMD_READ) └► ftl_read(lpn, cmd)
  ├─ ftl_cache_probe() 命中 → 直接 5µs 后完成（cache_rd_hit++）
  └─ 未命中 → pio(bg=0, cmd)
       └► ftl_direct_start → （同上等待/分配）
            └► flash_submit(FOP_READ) → 完成 → pio_finish（cache_rd_miss++）
读不产生日志：映射表在 RAM 里（掉电重建），读只动 NAND 时序。
```

### 5.3 后台刷写（bg_timer 的 cache 部分）

```
ftl_cache_bgflush():
  while 脏页 > cache_capacity*cache_flush_pct/100 且 bg_out < flush_out_max:
      按 LRU 驱逐一页 → ftl_cache_flush_page → (5.1 的后台落盘链)
  随后 cache_waiters_retry()（有预算就放 wait_cache 的主机写进来）
```

### 5.4 日志写入与段提交（journal.c 的心脏）

```
jnl_append(lpn, ppa, seq):     J.buf[nbuf++]=记录; J.lsn++; pump()
pump():
  writing 中 → 返回；wait_ckpt → 返回（等检查点完成的唤醒）
  nbuf>=jnl_flush_at 或有 jnl_w 等待者 → start_segment()
  nbuf==0 且有 jnl_w → cbw_run(jnl_w)     ← 屏障在"日志完全空闲"时放行
start_segment():
  快照 buf→J.seg；nbuf=0；seg_seq++；writing=1；
  ftl_jnl_retry_waiters()（缓冲腾出，唤醒 wait_jnl）
  write_seg_page()
write_seg_page():（写当前页）
  新块第一页 → block_safe()?（块内所有 lsn <= covered_lsn 才允许覆盖）
      否则 J.wait_ckpt=1; ckpt_force(); return   ← 挂起等检查点
      是 → flash_submit(FOP_ERASE) → seg_erase_cb → 继续
  提交 PROG(JSEG 页) → seg_page_cb → cur++ → 下一页
  全部数据页完成 → 提交 JCMT 页（含 seg_seq/count）→ seg_commit_cb:
      writing=0; st.jnl_segments++; pump()   ← 屏障等待者在此被放行
```

### 5.5 检查点（ckpt_kick / ckpt_force → ckpt_start）

```
ckpt_kick(): 距上次检查点的记录数 >= ckpt_interval 才做（节流）
ckpt_force(): 日志回绕强制（绕过节流；ST_OFF/ST_PLP 或已有检查点则放弃）
ckpt_start():
  数 dirty_bit → n==0 直接返回（无可快照）
  快照 → C.snap；清 C 状态；slot 轮换；slot_seq++；erase 两个槽
  写 CKPT 数据页(每页≤ckpt_epp 条) → 写 CKCM 提交页(定长最后一页)
  ckpt_commit_cb:
      J.covered_lsn = C.lsn   ★ 水位发布（此刻起旧日志段才可被覆盖）
      ftl_dirty_cleared_all() 清脏表
      J.wait_ckpt=0; pump()      ← 唤醒被 block_safe 挂起的段写
      cbw_run(ckpt_w)            ← 唤醒等检查点的等待者
```

### 5.6 GC 周期（gc.c）

```
gc_kick(why)（来源：bg 每 1ms / 分配失败 / 擦除完成续跑 / 排空）
  门控: busy? sim_state!=RUNNING? free>trigger? 排空中且非饥饿? → 放弃
  victim_select(mode): 只在 BLK_CLOSED 里选
      GC 模式: 有效页最少（优先低于 gc_victim_max_pct 的）
      WL 模式: PE 最老
  gc_start: busy=1, t0 → gc_submit_moves()
gc_submit_moves():
  while moves_out<gc_max_out && scan<ppb:
      该页有效?（ppa2lpn 且 l2p 仍指向它）否→跳过
      flash_submit(READ) → moves_out++
gc_read_cb():  ftl_alloc_gc(G.victim,...)  目的地固定在受害者同 node
      成功 → flash_submit(PROG)
      失败 → 入 pending 暂存（等 ftl_free_block 唤醒 gc_on_free）
gc_prog_cb():
      l2p 仍指向 src? → ftl_map_commit(is_gc=1)（GC 也走日志！）+ gc_moved++
                        否则该页期间被覆写 → ftl_discard_page(dst)
      scan 走完且无在途无 pending → gc_after_moves()
      否则继续 gc_submit_moves()
gc_after_moves → jnl_flush(gc_after_jnl)      ★ 日志屏障：搬迁记录必须先持久
gc_after_jnl → flash_submit(ERASE, victim)
gc_erase_cb: 统计; G.busy=0; ftl_free_block(victim)（清零、回池、唤醒
             wait_alloc）→ gc_on_free(消化 pending) → gc_kick("cycle") 续跑
```

### 5.7 完整性校验（journal.c verify_scan）

```
for lpn in 全部用户页:
  if ack_seq[lpn]==0 → 跳过（从未确认过）
  p = l2p[lpn]
  if p==INVALID || seq_nand[p] < ack_seq[lpn] → lost_writes++（记录前10个）
  else → verified_ok++
LOST>0 → 进程退出码 1
```

### 5.8 调用关系速查（谁唤醒谁）

| 等待者 | 等什么 | 谁唤醒 |
|---|---|---|
| `wait_cache` | 缓存/背压腾位 | `pio_finish`（bg 页完成）、`ftl_cache_bgflush` |
| `wait_jnl` | 日志缓冲腾位 | `start_segment`（缓冲快照走后） |
| `wait_alloc` | 空闲块 | `ftl_free_block`（GC 擦除回池） |
| `pending`(GC) | 目的页 | `gc_on_free`（同上） |
| `jnl_w` 屏障 | 日志完全空闲 | `pump`（writing/wait_ckpt 都清后） |
| `ckpt_w` 等待 | 检查点完成 | `ckpt_commit_cb` |
| `J.wait_ckpt` | 检查点覆盖水位 | `ckpt_commit_cb`（covered_lsn 发布） |
| `cf_done`(flush-all 批次) | 批次清零 | `pio_finish → cf_check` |
| 主机命令(left) | 全部页完成 | `pio_finish` |

---

## 6. 关键不变量与设计决策

### 6.1 等待队列必须 FIFO
历史上队列是 LIFO（新来者插队首），结果最老的等待者永远轮不到 ——
出现过单条命令 16 秒的延迟离群值。现在：**新等待者入队尾**（`wait_push`
追加），**重试失败者回队首**（`wait_requeue`，保住排队位）。

### 6.2 写缓存背压（`flush_out_max`）
脏页驱逐产生的后台页如果无界堆积，设备队列会失控。规则：
`bg_out >= flush_out_max` 时禁止驱逐、`ftl_cache_insert` 返回 -1，
主机写转 `wait_cache`。缓存饱和时主机吞吐自动贴合 NAND 消化速度，
这是真实 SSD 的行为而不是 bug。

### 6.3 空闲块水位的三层防线（防 GC 死锁的核心）
- **host 类分配**止步于 `free <= reserve_blocks`（默认 4）——给 GC 留工作余量；
- **PLP 落盘**可越过预留（断电时必须写出去），但止步于 `free<=0`；
- **GC** 每周期"弹出≤1 块 / 擦除归还+1 块"，净零。关键实现：
  **GC 目的地用 `pin_node` 固定在受害者所在 node** —— 否则按 node 轮转
  分配会在恢复后（active 全清）一个周期突发弹出最多 16 块，抽干空闲池，
  造成 pending 永远等不到目的页的死锁。
- 配套：**恢复时把部分块重新开放为 active**（§4.6②），既回收被搁置的
  空间，又保证池空时 GC/主机仍有落脚点。

### 6.4 `covered_lsn` 只在检查点提交页落盘后发布
日志段覆盖旧块之前必须确认旧记录已被检查点固化（`block_safe()`）。
检查点自身的完整性靠"数据页 + 定长提交页(CKCM)"两段式：**提交页没写完
的检查点视为不存在**，恢复时挑 `slot_seq` 大的完整槽即可，天然容忍
"新槽写一半断电"。日志段同理：**只有 JCMT 提交页存在的段才重放**，
且数据页逐张校验（magic/seg_seq/count），撕裂段被跳过。

### 6.5 GC 搬迁记录也写日志，擦除前必须过屏障
`gc_prog_cb → ftl_map_commit(is_gc=1) → jnl_append`。受害者擦除前调用
`jnl_flush(gc_after_jnl)`：如果搬迁后的映射只在 RAM，掉电恢复会把映射
指回已擦除的块 = 数据丢失。屏障在"日志完全空闲"（无写段、无缓冲、无挂起
检查点）时才放行（`pump` 的条件）。

### 6.6 世代号（gen）防止复位后的旧回调捣乱
`J.gen`/`C.gen` 在易失复位时 +1；`cbw_run()` 执行等待链前比对世代，
不匹配整链作废 —— 掉电清场后，掉电前排队的屏障回调不会再执行。

### 6.7 事件句柄悬垂 = 堆损坏
`ev_purge()` 直接 free 事件，但模块可能还攥着句柄（如 `bg_ev`）。
曾导致 `ev_cancel(bg_ev)` 写已释放内存、malloc 时 unlink 崩溃。
规则：**purge 之后所有持句柄者必须失效自己的句柄**
（`bg_ev_invalidate()`），或改用"懒取消"（ev_cancel 只打标记）。

### 6.8 pio_finish 先完成主机命令、后触发批次回调
`cf_check` 可能引发掉电序列（其回调链里有 power_hard_cut →
host_purge 释放 cmd）。若先减 cf_out 再减 cmd->left，cmd 可能已被释放 =
UAF。所以顺序固定：`bg_out-- → 主机 left--/完成 → cf_out--/cf_check`。

### 6.9 drain 必须等 GC 收尾
否则报告里 GC 计数不完整（出现过 runs=0 但 moved>0 的矛盾统计）。
排空期间用 `g_drain` 门控 GC/WL 起新周期（仅饥饿时豁免），保证收敛。

### 6.10 检查点节流
每次日志提交都做检查点会把元区写爆。`ckpt_interval` 限频，
`ckpt_force` 只留给日志回绕和 `block_safe` 挂起这两种"必须现在覆盖"的场景。

---

## 7. 配置参数速查

默认值见 `ssd.cfg`。最常动的参数与其效果：

| 参数 | 默认 | 调大会怎样 / 调小会怎样 |
|---|---|---|
| `cache_pages` | 256 | 大：写延迟低（更多 ack 走 SRAM）；小/0：接近直写 |
| `flush_out_max` | 128 | 大：后台队列深、缓存更满；小：背压更早、写吞吐贴 NAND |
| `gc_trigger_pct` | 5 | 大：GC 提前跑、更平滑；小：GC 突发、写延迟抖动 |
| `gc_victim_max_pct` | 50 | 小：只选很空的块（WAF 低但候选少）；大：选择快、WAF 略高 |
| `reserve_blocks` | 4 | 大：GC 余量足（防死锁）但吃用户容量；小：危险 |
| `ckpt_interval` | 65536 | 小：恢复快（重放少）但元区写放大；大：反之 |
| `plp_budget_ns` | 50ms | 小：budget_hits↑（退化为硬切丢数据）；大：容错多 |
| `power_cuts` / `cut_interval_ns` | 0/20s | 实际掉电间隔=间隔/次数；用 `-P N` 一行注入 |
| `fill_pct` | 70 | 大：更快进入 GC 高压区；小：几乎无 GC |
| `meta_blocks` | 8 | 日志区=meta-2×检查点块数；长跑日志回绕需要足够大 |

---

## 8. 动手实验建议

按"改一处 → 跑 → 看报告哪变了"的方式加深理解：

1. **直写模式**：`cache_pages = 0`，看写延迟从 p50≈1.6ms(队列) 变成
   贴近 NAND 直出，`wr new/hit` 统计变化。
2. **无背压**：`flush_out_max = 1000000`，观察缓存不再饱和、
   但 `bg_out` 深度和写延迟尾部的变化。
3. **GC 风暴**：`fill_pct = 95`，WAF 和 GC busy 占比大幅上升；
   对比 `gc_trigger_pct=20` 的平滑效果。
4. **预留块危险区**：`reserve_blocks = 1` + `-P 5` 长跑，体会 §6.3 的
   防线（正常应当依然 LOST=0 —— 如果你改坏了分配逻辑就会挂死，
   这正是当年 bug 的形态）。
5. **PLP 边界**：`plp_budget_ns = 2000000`（2ms），掉电时
   `budget_hits>0` 且 LOST>0 —— 电容不够就是会丢数据。
6. **恢复重放规模**：`ckpt_interval = 4096` + `-P 3`，检查点变多、
   日志重放变短，对比 `recovery est_time`。
7. **trace 回放**：写一个 `my.trace`（每行 `W 0 32` 这种），用
   `-w trace -t my.trace` 跑真实负载形状。
8. **调试入口**：`-v` 看 `[t]` 行内部状态；卡死时用 `debug.sh` 抓栈；
   valgrind 跑 `-c t.cfg --no-fill -P 5` 保证改动后内存依然干净。
9. **加一个自己的统计**：在 `stats_t` 加字段、在 `ftl_direct_start`
   计数、在 `print_report` 输出 —— 熟悉全链路的最短路径。

---

## 附：一句话总结每个模块的"存在理由"

- **event.c**：没有它就没有"时间"——一切并发与时序都是事件的排列。
- **nand.c**：把"先到先得 + 介质/总线双资源"的时序约束压缩成两次时间戳比较。
- **ftl.c**：易失映射世界的全部：谁在哪（l2p）、还能写哪（pool/active）、
  写不过去时排哪条队（三条 FIFO）。
- **journal.c**：把易失世界的变化可持续地记在 NAND 上，并在断电后把它
  悄无声息地重建出来 —— "掉电保护"四个字的全部实现。
- **gc.c**：让有限的块永远够用，代价是写放大；顺手把磨损抹平。
- **host.c/workload.c**：像真实主机一样发命令、像裁判一样记账（ack_seq）。
- **stats.c**：把 25 秒虚拟时间里发生的一切变成一份能读懂的体检报告。


