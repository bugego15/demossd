# ssdsim — SSD 固件模拟器 (C)

一个离散事件驱动的 SSD 固件核心模拟器，覆盖企业级 SSD 固件的主要子系统：

- **FTL**：L2P 映射、块分配（按 node 的多流 active block）、OP 空间、预留块
- **写缓存**：write-back cache（LRU）、脏页驱逐、背压（backpressure）等待队列
- **GC / WL**：贪心有效页率回收、搬迁流水线（读-写并发上限）、磨损均衡钩子
- **NAND 调度**：虚拟纳秒时钟 + 最小堆事件队列，channel/die 级 tPROG/tR/tBERS 时序，
  时间序贪心仲裁
- **掉电保护 (PLP)**：电容预算、缓存落盘序列、日志提交屏障、硬掉电路径
- **日志 / 检查点**：16B 变更记录、日志段（数据页 + 提交页）、双槽检查点、CRC32、
  区块安全准入（erase-on-block-entry）、掉电恢复重放
- **SMART**：容量、带宽、延迟直方图、PE 磨损分布、掉电次数等
- **完整性校验**：`ack_seq / map_seq / seq_nand` 三元组验证"已确认的写是否真正落盘"

## 构建与运行

```sh
make                 # gcc, -O2 -g -Wall -Wextra
./ssdsim             # 默认配置 ssd.cfg, 全量负载, 输出统计报告
```

在 Windows 上可用 WSL：`wsl -e bash -lc "cd /mnt/d/test && make && ./ssdsim"`

## 命令行

```
-c FILE     配置文件（默认 ssd.cfg）
-o FILE     报告写入文件（默认 stdout）
-w X        负载: rand | seq | zipf | trace
-n N        稳态 IO 数
-q N        队列深度
-r F        读比例 0..1
-s N        IO 大小（扇区数，32 = 1 页）
-t FILE     回放 trace（每行 "R|W|T lba nlb"）
-P N        注入 N 次随机掉电（间隔 = cut_interval / N）
--no-plp    关闭掉电保护（掉电应丢失已确认写）
--no-fill   跳过预填充阶段
--no-verify 跳过最终完整性校验
--seed N    随机种子
-v          详细事件/状态 trace（[t]/[nodes]/progress 等诊断输出）
```

配置文件为 `key = value`，全部键及默认值见 `ssd.cfg`（几何结构、NAND 时序、
缓存/GC/PLP 参数、负载参数）。几何结构、meta 区域大小会按容量自动校验/扩张。

## 验证模型

每个逻辑页维护三个序号：

| 变量 | 含义 |
|------|------|
| `ack_seq[lpn]` | 主机已确认（写缓存 ack）的最新序号 |
| `map_seq[lpn]` | 映射表认为该页应处的序号 |
| `seq_nand[ppa]` | NAND 页 OOB 中实际记录的序号 |

运行结束后逐页比对：`ack_seq > seq_nand[当前映射页]` 即为
**LOST（已确认但未持久化）**——这是衡量掉电保护正确性的核心指标。

## 掉电保护流程

```
掉电事件 ── PLP 开启 ──> ST_PLP: host 停止 → 缓存全量落盘
                          → 等待后台写队列排空 → 提交日志（屏障）→ 立即断电
        │                （超时触发 deadline：电容耗尽，RAM 内容全部丢失）
        └── PLP 关闭 ──> 硬掉电: NAND 操作作废 → host 命令清除
                          → 事件队列清空 → 丢弃脏缓存 → 日志易失区复位 → 恢复
```

恢复流程：选取序号更高的检查点槽 → 收集已提交日志段 → 按序重放
（仅 `seq > map_seq` 的记录生效）→ OOB 全盘扫描重建 free_ptr/池/反向映射，
并把每个 node 最空的部分块重新开放为 active 块。

## 测试矩阵（均通过，LOST=0 为通过标准）

```sh
./ssdsim                       # 默认全量负载: LOST=0, WAF≈1.0
./ssdsim -P 3                  # PLP 开启 + 3 次掉电: cuts=3, budget_hits=0, LOST=0
./ssdsim -P 3 --no-plp         # PLP 关闭: 应检测出 LOST>0（演示丢数据检测）
./ssdsim -n 2500000 -P 3       # 长稳: 日志区回绕 8 次, 78 次检查点双槽交替, LOST=0
valgrind ./ssdsim -c t.cfg --no-fill -P 5   # 0 errors, 0 leaks
```

参考数据（默认配置，4CH×4die×64blk×512pg×16KB = 8GB，7.38GB 用户空间）：
读 ~90 MB/s、写 ~293 MB/s、WAF 1.004、PE 均匀、写延迟 p50≈1.6ms（缓存饱和背压下）。

## 代码结构

```
include/types.h   共享类型/配置/统计/原型
src/main.c        CLI、配置解析、主事件循环、后台定时器、drain
src/event.c       最小堆事件队列（取消/清空/推进）
src/nand.c        NAND 阵列模型：chip→die→plane→block→page、时序、仲裁
src/ftl.c         映射表、块分配/回收、写缓存、等待队列、恢复重建
src/gc.c          GC/WL：受害者选择、搬迁流水线、擦除与回池
src/journal.c     日志/检查点/PLP/掉电恢复
src/host.c        主机命令队列、页级拆分、完成路径
src/workload.c    负载生成器（rand/seq/zipf/trace/fill）与 ack 记账
src/stats.c       延迟直方图、磨损扫描、报告与 SMART
```

## 关键设计点

- **等待队列 FIFO**：新等待者入队尾，失败重试者回队首，避免 LIFO 饿死（曾导致
  单条命令延迟 16s 的离群值）。
- **写缓存背压**：后台写队列 `flush_out_max` 达上限时驱逐暂停，主机写进入
  `wait_cache` 队列等待，由完成/刷写路径逐个重试——脏页不会无界堆积。
- **GC 目的地固定在受害者所在 node**：一个 GC 周期最多从空闲池弹出 1 块
  （否则恢复后 active 全清，一个周期会突发弹出 16 块，直接抽干空闲池导致死锁）。
- **空闲池不变量**：host 类分配止步于 `reserve_blocks`（GC 有余量）；PLP 落盘可
  越过预留但止步于 0；GC 周期 `≤1 弹出 / +1 归还`，系统在低水位仍可恢复。
- **drain 语义**：排空阶段等缓存刷完、后台队列排空、**当前 GC 周期结束**，再提交
  日志，保证报告中 GC/PE 统计是最终值。
- **检查点节流**：`ckpt_interval` 限制频率，日志回绕时强制检查点；
  `covered_lsn` 只在检查点提交页落盘后发布。

## 调试工具

`debug.sh`：长时间无进展时抓取进程状态与 gdb 调用栈
（`wsl bash /mnt/d/test/debug.sh <参数>`）。诊断打印均用 `-v` 打开。
