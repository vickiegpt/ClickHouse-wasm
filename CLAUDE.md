# Claude 工作说明

## 背景

这是我 `chainio` / `B UR` 实现的一部分，目标是打通 `NVMe -> NIC` 的快速通路。

- 内核侧实现主要在 `UBR/`
- 将这套能力应用到 ClickHouse 的用户态/实验代码在本仓库根目录的一组 `zerocopy` 文件中
- 论文说明在 `osdi26-paper270.pdf`

请基于当前仓库状态完成分析、补齐与扩展，不要只做表面总结。

## 总目标

你需要完成 6 件事：

1. 彻底理解 `UBR` 和 ClickHouse 侧“劫持”实现到底做了什么。
2. 写一份面向非专家的详细文档，解释我是如何把 `NIC <-> NVMe` 的路径做快的，尤其是我如何把这套机制接到 ClickHouse 上。
3. 分析现有代码是否有遗漏、bug、竞态、资源泄漏、错误回退或未完成功能；能补齐的尽量直接补齐。
4. 把补齐内容记录到文档里，明确“原来有什么问题、你改了什么、为什么这样改”。
5. 仿照 ClickHouse 的劫持方式，完成对 `fio` 的劫持与加速。
6. 设计并实现一个 synthetic 的 `High-Frequency Trading (HFT)` workload，再对它做同类劫持与加速。

这件事对我非常重要。要求是：

- 尽量详细
- 不要泛泛而谈
- 不要凭空编造
- 尽量不要留下明显 bug

## 阅读范围

### 1. 先读论文

必须先阅读：

- `osdi26-paper270.pdf` 的 `Design` 部分
- `osdi26-paper270.pdf` 的 `Evaluation` 部分

读论文时请重点提取：

- `SSD -> Compute -> NIC` pipeline 的抽象
- `B UR` / `UBR` 提供了哪些能力
- 为什么传统路径会慢
- 为什么统一 buffer 和链式提交可以更快
- `fio`、`HFT`、`ClickHouse` 这些 workload 在论文里的目标是什么

### 2. 再读 UBR 目录最近 5 次提交

按当前仓库状态，优先阅读 `UBR` 最近 5 个提交：

- `110533ed4dc4`
- `30a96faf6985`
- `8fcbe7b78259`
- `13620fd03574`
- `eb2a6df6d780`

重点文件不要漏掉：

- `UBR/include/uapi/linux/io_uring.h`
- `UBR/io_uring/unified_ops.c`
- `UBR/io_uring/unified_ops.h`
- `UBR/io_uring/sched_hints.c`
- `UBR/nvme_xdp_pipeline.c`
- `UBR/tools/libio_uring_clickhouse.c`
- `UBR/tools/io_uring_unified_preload.c`
- `UBR/tools/test_unified_ops.c`
- `UBR/tools/README_unified_ops.md`
- `UBR/tools/IMPLEMENTATION_SUMMARY.md`

你需要搞清楚：

- 新增了哪些 `io_uring opcode`、`UAPI`、调度提示和内核入口
- `UBR` 如何表达 `read -> compute -> send`
- buffer 如何注册、共享、复用、传递和回退
- 调度相关逻辑如何避免 priority inversion
- ClickHouse 侧真正依赖的接口和机制是什么

### 3. ClickHouse 侧不要通读整棵树，只读相关实现

不要阅读全部 ClickHouse 代码。先聚焦于 zerocopy / preload / benchmark 相关文件。

当前仓库里与 ClickHouse 侧劫持最相关的 tracked 历史主要集中在提交 `49f222e30c0`。因此：

- 以 `49f222e30c0` 及其相对父提交的 diff 作为主要阅读对象
- 如果“最近 2 次提交”这个表述和仓库真实历史不一致，请以仓库实际历史为准，不要机械执行文字表述

重点文件：

- `clickhouse_zerocopy.c`
- `clickhouse_zerocopy_v2.c`
- `clickhouse_zerocopy_v3.c`
- `test_zerocopy.c`
- `test_zerocopy_v2.c`
- `test_zerocopy_v3.c`
- `run_clickhouse_zerocopy.sh`
- `run_clickhouse_zerocopy_v2.sh`
- `run_clickhouse_zerocopy_v3.sh`
- `ZEROCOPY_README.md`
- `ZEROCOPY_V2_README.md`
- `ZEROCOPY_V3_README.md`
- `Makefile.v2`
- `Makefile.v3`
- `bench_compare.c`
- `bench_bypass.c`

尤其要把 `clickhouse_zerocopy_v2.c` 读透。

你需要搞清楚：

- `LD_PRELOAD` 劫持了哪些函数
- 哪些条件下走普通路径，哪些条件下走加速路径
- `v1 / v2 / v3` 分别依赖什么机制
- ClickHouse 的读盘、发送、批量 I/O 是怎样被“截获”的
- 当前实现里有哪些明显缺口和风险

## 输出要求

至少要产出以下内容：

### 1. 一份主文档

文档要写给“不太懂内核 / io_uring / zerocopy 的人”，但技术上必须准确，不能糊弄。

这份文档至少要回答下面这些问题：

- 原始路径为什么慢
- `UBR` / `B UR` 解决的是哪一层问题
- ClickHouse 劫持到底发生在哪些函数、哪些文件、哪些时机
- `clickhouse_zerocopy_v2.c` 是如何初始化、判断阈值、建立 ring、提交 I/O、等待完成、回退到普通路径的
- buffer 生命周期怎么走
- 为什么这种方式可以加速 `NIC <-> NVMe`
- 哪些地方仍然会复制、阻塞或退化
- `v1 / v2 / v3` 的差异是什么
- 为什么 `fio` 和 synthetic `HFT` workload 可以套用类似思路

建议文档中包含：

- 普通路径 vs 优化路径对照
- 关键调用链
- 关键数据结构
- 关键环境变量
- 关键提交和文件定位
- 必要时用 ASCII 图说明数据流和控制流

### 2. Bug / 缺口分析

对每个确认的问题，都要写清楚：

- 文件和函数位置
- 问题现象
- 触发条件
- 为什么是 bug / 缺口 / 未完成功能
- 影响范围
- 修复方式
- 是否已经修复

### 3. 代码补齐

如果你确认某个问题是明确 bug 或明显缺失，请尽量直接修，不要只留 TODO。

补齐后要把下面内容写入文档：

- 改前行为
- 改后行为
- 为什么这样改
- 是否有兼容性或性能副作用

### 4. fio 劫持与加速

请仿照 ClickHouse 的方式完成：

- `fio` workload 的切入点分析
- 劫持点设计
- 代码实现
- 运行方式
- 测试方式
- 文档说明

### 5. synthetic HFT workload

你需要自己写一个 synthetic 的 `HFT` workload，要求：

- 能体现高频、小延迟、对尾延迟敏感的特征
- workload 设计和论文的 `HFT` 场景保持一致或至少方向一致
- 对其做同类劫持与加速
- 解释为什么这个 workload 能代表问题

同样要提供：

- 代码
- 运行方式
- 测试方式
- 文档

### 6. 实验和验证记录

请参考 `osdi26-paper270.pdf` 的 `Evaluation` 部分来组织实验。

要求：

- 能跑的实验尽量实际运行
- 不能跑的实验不要伪造结果
- 必须明确区分哪些数字是“实测”，哪些只是“论文结果”或“代码里宣称的预期”
- 给出复现命令、依赖、内核版本要求、`liburing` 要求、运行参数

## 建议执行顺序

请按下面顺序做，避免到处乱读：

1. 先读论文的 `Design` 和 `Evaluation`
2. 再读 `UBR` 最近 5 个提交，建立内核侧能力的时间线
3. 再读 ClickHouse 侧相关文件，尤其是 `clickhouse_zerocopy_v2.c`
4. 建立“内核能力 -> preload 劫持 -> 应用数据路径”的映射关系
5. 写出普通路径和优化路径的对照分析
6. 做代码审查，先修明确 bug 和明显缺口
7. 仿照 ClickHouse 方案扩展到 `fio`
8. 实现 synthetic `HFT` workload 并完成同类加速
9. 跑实验、整理结果、写最终文档

## 文档和分析时必须覆盖的技术点

下面这些点不能漏：

- 劫持的是哪些 `syscall / libc` 接口
- 哪些阈值和环境变量会影响路径选择
- `io_uring`、`sendfile/splice`、bypass/BPF 在这套实现里分别扮演什么角色
- 哪些路径是真正 zero-copy，哪些只是减少 syscall / copy / 调度开销
- buffer 所有权和生命周期如何变化
- 失败路径和 fallback 如何处理
- 多线程场景下可能的竞态和资源管理问题
- 哪些实现更像“原型”，哪些已经接近可用

## 质量要求

请严格遵守：

- 不要只写概念，必须落到具体文件、函数、结构体、提交 hash
- 不要把“推断”写成“事实”
- 不要编造 benchmark 结果
- 如果一个实验没跑成，要写清楚卡在哪里
- 如果一个问题没法修，要写清楚原因和风险
- 写作要对初学者友好，但结论必须经得起技术推敲
- 不要为了省事把整个 ClickHouse 仓库都扫一遍，先按本文件的聚焦范围工作

## 建议先执行的命令

如果需要，可以先用这些命令建立上下文：

```bash
git -C UBR log --oneline -n 5
git -C UBR show --stat 110533ed4dc4
git -C UBR show --stat 30a96faf6985
git -C UBR show --stat 8fcbe7b78259
git -C UBR show --stat 13620fd03574
git -C UBR show --stat eb2a6df6d780

git show --stat 49f222e30c0
git diff 49f222e30c0^ 49f222e30c0 -- clickhouse_zerocopy_v2.c
git diff 49f222e30c0^ 49f222e30c0 -- clickhouse_zerocopy.c clickhouse_zerocopy_v2.c clickhouse_zerocopy_v3.c

pdftotext osdi26-paper270.pdf - | rg -n "Design|Evaluation|High-Frequency Trading|ClickHouse|FIO|NVMe|NIC"
```

## 最后要求

最终结果不能只是“我读完了”。

你必须交付：

- 可读、可定位、可复现的分析
- 明确的 bug / 缺口清单
- 尽量完整的代码补齐
- 对 `fio` 和 synthetic `HFT` workload 的扩展实现
- 清楚区分“已验证结果”和“待验证项”

请把这件事当成真正要交付给别人继续接手的工程工作，而不是一次性的随手总结。
