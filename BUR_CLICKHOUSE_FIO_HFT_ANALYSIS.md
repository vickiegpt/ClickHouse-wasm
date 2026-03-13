# B UR / ClickHouse / fio / Synthetic HFT 分析与补齐记录

## 1. 先说结论

这份仓库里实际上并行存在两条路线：

1. `UBR/` 里的内核扩展路线  
   关键词是 `IORING_OP_UNIFIED_OPS`、`io_uring_sched_hints`、priority inheritance、NVMe/XDP pipeline。

2. 仓库根目录的 `clickhouse_zerocopy*.c` 路线  
   关键词是 `LD_PRELOAD`、标准 `io_uring`、`sendfile/splice`、BPF/DMA prototype。

这两条路线在“目标”上是一致的，都是试图缩短 `storage <-> compute <-> network` 的路径，减少 copy、syscall、调度切换；但在“实现连接”上目前并没有真正打通：

- `UBR/` 这条线改了内核接口和调度器。
- 根目录的 `clickhouse_zerocopy_v2.c` 并没有调用 `IORING_OP_UNIFIED_OPS`，也没有使用 `io_uring_sched_hints`。
- 所以当前仓库更像是：
  - 一套论文 / 内核方向原型
  - 再加一套可直接 `LD_PRELOAD` 到用户程序上的 ClickHouse/fio/HFT 侧实验原型

这一点必须讲清楚，否则很容易把“论文能力”和“当前真正跑起来的代码路径”混为一谈。

## 2. 论文里的设计，和当前代码到底对应到哪里

### 2.1 论文里的核心问题

论文 `osdi26-paper270.pdf` 的核心点可以压缩成三句话：

1. 传统 Linux I/O 把 `read -> compute -> send` 看成互相独立的点操作，内核看不到完整 pipeline。
2. 因为看不到整体关系，内核无法：
   - 复用 buffer
   - 合并调度
   - 继承应用优先级
   - 避免无意义的用户态来回搬运
3. 结果就是：
   - 多次 copy
   - 多次 syscall
   - completion 处理和用户线程优先级脱节
   - 尾延迟和 CPU 消耗都变差

### 2.2 论文里的三个机制

论文对应的三个机制，在代码里分别可以对到：

1. **Computational Chains**  
   目标：把 `read -> compute -> send` 作为一个整体提交。  
   代码落点：
   - `UBR/io_uring/sched_hints.h`
   - `UBR/io_uring/sched_hints.c`

2. **Unified Buffer Registry (UBR)**  
   目标：在存储和网络两边共享统一 buffer 命名和生命周期。  
   当前仓库里最接近的接口是：
   - `UBR/include/uapi/linux/io_uring.h`
   - `UBR/io_uring/unified_ops.c`

3. **I/O-aware Scheduler**  
   目标：让内核 completion 路径继承提交线程的优先级，减少 inversion。  
   代码落点：
   - `UBR/io_uring/sched_hints.c`

### 2.3 当前仓库里最关键的现实差距

当前代码里最重要的现实差距有两个：

1. `UBR/io_uring/unified_ops.c` 里的 `READ` 并不是零拷贝。  
   代码在 `io_unified_do_read()` 里先 `kernel_read()` 到内核 buffer，再 `copy_to_user()` 回用户态。  
   也就是说，这里仍然有一次内核到用户的 copy。

2. `UBR/io_uring/unified_ops.c` 的 `CALC` 也不是 in-place shared buffer compute。  
   它先 `copy_from_user()` 把数据搬进内核，再算和，再 `copy_to_user()` 写回结果。  
   所以这更像“统一 opcode + 共享统计结构”的功能原型，不是论文中最强意义上的 unified zero-copy pipeline。

结论：  
`UBR/` 目录里的代码表达了设计方向，但从严格意义上说，当前仓库里的实现还没有把论文中的“统一 buffer、跨域零拷贝”完全做实。

## 3. UBR 最近 5 次提交做了什么

我按 `git -C UBR log --oneline -n 5` 读了这 5 个提交：

- `110533ed4dc4`
- `30a96faf6985`
- `8fcbe7b78259`
- `13620fd03574`
- `eb2a6df6d780`

其中和当前主题直接相关的是前 4 个。

### 3.1 `13620fd03574`

这是第一批真正把“unified ops”框架立起来的提交。

关键内容：

- 在 `UBR/include/uapi/linux/io_uring.h` 里新增 `IORING_OP_UNIFIED_OPS`
- 增加 `IO_UNIFIED_OP_READ / SEND / CALC`
- 增加 `io_uring_unified_shared` 共享统计结构
- 新增：
  - `UBR/io_uring/unified_ops.c`
  - `UBR/io_uring/unified_ops.h`
  - `UBR/tools/io_uring_unified_preload.c`
  - `UBR/tools/test_unified_ops.c`

这一步本质上是：

- 把多个逻辑动作合到一个新的 io_uring opcode 入口
- 再用用户态 preload 把普通 `read()` / `send()` 导到这个入口

### 3.2 `8fcbe7b78259`

这次是在已有 unified ops 路线上做兼容性和边角修补：

- 继续修改 `tools/io_uring_unified_preload.c`
- 继续调整测试

更像把原型从“刚能跑”往“少一点粗糙”推进。

### 3.3 `30a96faf6985`

这是和论文主题最贴近的一次。

关键内容：

- 新增 `UBR/nvme_xdp_pipeline.c`
- 新增 `UBR/tools/libio_uring_clickhouse.c`
- 新增 `UBR/io_uring/sched_hints.c`
- 继续扩展 UAPI

这一步的意义是：

- 不再只做“统一 opcode”
- 开始把调度 hints、NVMe、XDP/网络 pipeline 和 ClickHouse preload 联系起来

### 3.4 `110533ed4dc4`

这一步继续增强 `sched_hints`：

- 扩大 `UBR/io_uring/sched_hints.c`
- 增加 `UBR/io_uring/sched_hints.h`
- 继续触碰 `UBR/io_uring/unified_ops.c`

它强调的是“优先级继承、chain dependency、调度统计”的方向。

### 3.5 当前 UBR 代码的真实状态

从实现成熟度上看：

- `sched_hints` 的链式调度结构已经写得比较完整
- 但 syscall 接口并没有把注释里宣称的 `CHAIN_ADD / CHAIN_DONE / CHAIN_CANCEL` 真的接出来

具体看：

- `UBR/io_uring/sched_hints.c:261-400` 已经有：
  - `io_sched_chain_add()`
  - `io_sched_chain_complete()`
  - `io_sched_chain_cancel()`
- 但 `UBR/io_uring/sched_hints.c:615-630` 的 `SYSCALL_DEFINE3(io_uring_sched_hints, ...)`
  只实现了：
  - `op=0` set hints
  - `op=1` get hints
  - `op=2` record op
- 注释里说支持 `3/4/5`，实际没有接线

这是一个明确的“设计已写到注释里，但接口没打通”的缺口。

## 4. ClickHouse 侧“劫持”到底是怎么做的

## 4.1 历史现实

根目录这组 zerocopy 文件基本都来自一次集中提交：

- `49f222e30c0`

也就是说，“读最近 2 次 ClickHouse 提交”这个说法在当前仓库快照里并不准确；真实情况是：  
ClickHouse 侧实验代码主要在这一提交里一起落地。

### 4.2 v1 / v2 / v3 的分工

#### v1: `clickhouse_zerocopy.c`

思路：

- `LD_PRELOAD` 劫持 `send/read/write/recv/sendto/recvfrom`
- 大块数据时优先走：
  - `splice`
  - `sendfile`
- 本质上是“尽量使用 Linux 现成 zero-copy 接口”

优点：

- 简单
- 对 file->socket 比较直接

限制：

- 一旦有 compute 阶段，纯 `sendfile/splice` 就不够用了
- 对复杂 readv/writev / 应用态处理帮助有限

#### v2: `clickhouse_zerocopy_v2.c`

思路：

- 仍然是 `LD_PRELOAD`
- 但主要依赖标准 `io_uring`
- 拦截：
  - `read/pread`
  - `write/pwrite`
  - `send/sendto/sendmsg`
  - `recv/recvfrom/recvmsg`
  - `readv/writev`（目前只对 regular file 走 io_uring）

这是当前最实用的一版，因为：

- 不要求自定义内核 opcode
- 可直接对 ClickHouse、fio、synthetic HFT 做 preload

#### v3: `clickhouse_zerocopy_v3.c`

思路：

- 在 preload 基础上继续引入：
  - DMA buffer
  - fd tracking
  - BPF bypass

但从代码状态看，v3 还更像 prototype：

- 有未使用变量
- 有未使用函数
- build 警告更多

所以当前真正适合继续扩展的基线是 v2。

## 5. `clickhouse_zerocopy_v2.c` 的完整数据路径

下面按代码实际执行顺序解释。

### 5.1 初始化

初始化发生在：

- `clickhouse_zerocopy_v2.c:223-274`

这里做了 4 件事：

1. `dlsym(RTLD_NEXT, ...)` 取回原始 libc 函数入口  
   见 `:232-243`

2. 读取环境变量  
   见 `:252-265`
   - `ZEROCOPY_DEBUG`
   - `ZEROCOPY_THRESHOLD`
   - `ZEROCOPY_QUEUE_DEPTH`
   - `ZEROCOPY_ASYNC`

3. 通过 `pthread_once` 创建 thread-local ring 的析构 key  
   见 `:267`

4. 构造函数 `early_init()` 会在库加载时直接调用  
   见 `clickhouse_zerocopy_v2.c:846-861`

### 5.2 每线程 ring

每个线程第一次进入大 I/O 路径时，会创建自己的 `io_uring`：

- `clickhouse_zerocopy_v2.c:147-182`

关键点：

- thread-local `struct io_uring *thread_ring`
- flags 默认：
  - `IORING_SETUP_COOP_TASKRUN`
  - `IORING_SETUP_SINGLE_ISSUER`
- 如果开了 `ZEROCOPY_ASYNC=1`
  - 会给 SQE 打 `IOSQE_ASYNC`
  - 并尽量设置 `IORING_SETUP_DEFER_TASKRUN`

注意：

- 这个“async mode”只是让 io_uring 更偏异步调度
- 它没有改变 preload 函数本身“同步返回结果”的 API 语义
- 所以它不是完整意义上的“fully async user API”

### 5.3 真正的劫持点

具体劫持点在：

- `read()` `:564-579`
- `pread()` `:581-590`
- `write()` `:592-607`
- `pwrite()` `:609-618`
- `send()` `:620-629`
- `sendto()` `:631-652`
- `recv()` `:654-663`
- `recvfrom()` `:665-690`
- `sendmsg()` `:692-701`
- `recvmsg()` `:703-712`
- `readv()` `:714-749`
- `writev()` `:751-786`

它们共同遵循的规则是：

1. 先 `ensure_init()`
2. 统计计数器加一
3. 如果小于阈值，则直接走原始 libc
4. 如果达到阈值，则：
   - socket 路径尽量走 `send/recv/sendmsg/recvmsg`
   - regular file 路径尽量走 `io_uring_prep_read/write/readv/writev`
5. ring 不可用时，回退到原始 syscall

### 5.4 `send/recv` 路径

socket 的主要优化函数在：

- `uring_send()` `:282-310`
- `uring_recv()` `:312-340`

流程是：

1. 取 thread-local ring
2. 取 SQE
3. `io_uring_prep_send` / `io_uring_prep_recv`
4. `submit_and_wait()`
5. 更新统计

这条路径减少的不是“数据绝对零拷贝”本身，而是：

- syscall 包装开销
- 统一提交/完成路径的固定成本

### 5.5 file 路径

regular file 的主要优化函数在：

- `uring_file_read()` `:342-371`
- `uring_file_write()` `:373-402`

`pread/pwrite` 通过显式 offset 进入这两条路径。  
这对 fio 特别关键，因为 fio 的 `psync` engine 常常就是 `pread/pwrite`。

### 5.6 file -> socket 路径

direct API 在：

- `uring_sendfile()` `:492-562`
- `zerocopy_uring_sendfile()` `:788-792`

这里现在的做法是：

1. 用 `pipe2()` 建临时 pipe
2. `io_uring splice`：
   - file -> pipe
   - pipe -> socket
3. 循环直到完成

这比原来“直接 `splice(file, socket)`”是正确的。  
因为 Linux `splice` 语义要求至少有一端是 pipe。

### 5.7 为什么这能“劫持 ClickHouse”

不是因为直接改了 ClickHouse 源码，而是因为：

- ClickHouse 最终还是会走 libc / syscall 层
- preload 抢在 libc 真实实现之前拿到控制权

只要 ClickHouse 的热路径里出现：

- 大块 `read/pread`
- 大块 `write/pwrite`
- 大块 `send/sendmsg`

这个库就能把这些调用改道到 io_uring 路径。

## 6. 这套实现和 UBR 内核线到底是什么关系

当前仓库里最容易误判的一点就是这里。

### 6.1 根目录 v2 并没有用上 `IORING_OP_UNIFIED_OPS`

证据很直接：

- `clickhouse_zerocopy_v2.c` 只包含标准 `liburing` 头
- 只用：
  - `io_uring_prep_send/recv/read/write/readv/writev/splice`
- 没有：
  - `IORING_OP_UNIFIED_OPS`
  - `__NR_io_uring_sched_hints`
  - `io_uring_unified_shared`

所以根目录 v2 是“标准 io_uring 版本的用户态劫持器”，不是“UBR 内核 opcode 的用户态接入层”。

### 6.2 `UBR/tools/libio_uring_clickhouse.c` 才更接近“论文路线”

对应代码：

- 初始化 ring：`UBR/tools/libio_uring_clickhouse.c:157-219`
- 记录 read/compute/send 序列：`:276-308`
- 设 scheduler hints：`:379-392`
- 拦截 `read/pread/write/pwrite/send/recv/readv/writev`：`:425-726`
- 标记 compute：`:728-737`

但它也有两个明显问题：

1. 它没有真正调用 `IORING_OP_UNIFIED_OPS`，而是仍然提交标准 `IORING_OP_READ/WRITE/SEND/...`
2. 它对 small read 才走 io_uring、large read 反而回退，这和根目录 v2 的策略完全相反

所以它更像是“往论文方向靠”的另一个实验分支，而不是当前主用实现。

## 7. 我确认并补掉的问题

### 7.1 已修复：v2 `.so` 单独 `LD_PRELOAD` 会缺 `liburing` 符号

问题位置：

- `Makefile.v2:5-15`

原问题：

- 原先库的链接顺序把 `-luring` 放在源文件前面
- 结果测试程序因为自己也链接了 `-luring`，所以看起来能跑
- 但真正 `LD_PRELOAD` 到别的程序时，库本身没有正确记录对 `liburing.so` 的依赖
- 实际现象就是：
  - `./synthetic_hft` baseline 能跑
  - `LD_PRELOAD=./libclickhouse_zerocopy_v2.so ./synthetic_hft` 直接报
    `undefined symbol: io_uring_queue_init_params`

修复：

- 把 `Makefile.v2` 改成：
  - `LDFLAGS = -shared`
  - `LDLIBS = -ldl -pthread -luring`
  - 链接时把 `$(LDLIBS)` 放在对象后面

顺手也把：

- `Makefile.v3`

做了同样修复。

### 7.2 已修复：原 v2 直接把负的 `cqe->res` 当返回值抛给应用

问题位置：

- 现在修后的标准化逻辑在 `clickhouse_zerocopy_v2.c:127-134`
- 提交/等待统一在 `:197-221`

原问题：

- io_uring CQE 负值表示 `-errno`
- 但 POSIX 包装函数应该返回 `-1`，并设置 `errno`
- 原实现直接返回负数，会破坏调用者错误处理

修复：

- 新增 `normalize_cqe_result()`
- 统一把负的 CQE 结果转换成 `errno = -res; return -1;`

### 7.3 已修复：原 v2 缺少 `pread/pwrite/sendto/recvfrom`

问题位置：

- 新增的拦截入口：
  - `pread()` `clickhouse_zerocopy_v2.c:581-590`
  - `pwrite()` `clickhouse_zerocopy_v2.c:609-618`
  - `sendto()` `clickhouse_zerocopy_v2.c:631-652`
  - `recvfrom()` `clickhouse_zerocopy_v2.c:665-690`

影响：

- fio 的 `psync` workload 主要依赖 `pread/pwrite`
- synthetic HFT 的 UDP ingress 需要 `recvfrom`
- 没有这些，库只能算 ClickHouse 局部 demo，不足以扩展到 fio/HFT

修复：

- 补齐上述 4 个入口
- regular file 走 `io_uring_prep_read/write`
- UDP/TCP 地址型 socket 走 `sendmsg/recvmsg` 封装

### 7.4 已修复：原 v2 的 `uring_sendfile` 直接 `splice(file, socket)` 语义不对

问题位置：

- 修后的实现：`clickhouse_zerocopy_v2.c:492-562`

原问题：

- 直接 `splice(in_fd=file, out_fd=socket)` 不满足标准 `splice` 约束
- 真正可工作的模型应该是：
  - file -> pipe
  - pipe -> socket

修复：

- 现在 `uring_sendfile()` 用临时 pipe 把两段 splice 串起来
- ring 不可用或建 pipe 失败时回退到 `sendfile()`

### 7.5 已修复：原 v2 没有 thread-local ring 析构

问题位置：

- `clickhouse_zerocopy_v2.c:88-107`
- `clickhouse_zerocopy_v2.c:176-177`

原问题：

- 线程退出时 ring 没有显式清理
- 长时间运行、多线程场景下会积累资源

修复：

- 增加 `pthread_key` + destructor

### 7.6 已修复：测试程序本身有多处误判风险

问题位置：

- `test_zerocopy_v2.c`

原问题：

1. 少了 `<sys/uio.h>`，直接编译不过  
   见 `test_zerocopy_v2.c:14-16`

2. fixture 文件创建走被拦截的 `write()`，污染统计  
   现在改为 `raw_write_all()`，见 `:39-52`

3. 原 file->socket 测试用 `AF_UNIX socketpair`，会把 `sendfile/splice` 路径测成“不支持”  
   现在改成 loopback TCP pair，见 `:54-95` 和 `:194-260`

4. 子进程用 `exit()`，会把 preload 析构统计重复打印  
   现在改为 `_exit()`，见多个 child 分支

## 8. 我确认但暂时没有继续硬修的问题

### 8.1 `UBR/io_uring/unified_ops.c` 不是严格 zero-copy

证据：

- `io_unified_do_read()` `UBR/io_uring/unified_ops.c:105-136`
- `io_unified_do_calc()` `UBR/io_uring/unified_ops.c:170-209`

原因：

- 这里本质上还是：
  - `kernel buffer <-> userspace`
  - `copy_to_user/copy_from_user`

这不是一个小补丁能补完的事情，因为它牵涉 UBR 真正的 buffer 注册、pin、DMA 映射和生命周期。

### 8.2 `io_uring_sched_hints` 注释里宣称支持 chain syscall，但实际没接出来

证据：

- chain 结构和逻辑已经在：
  - `UBR/io_uring/sched_hints.c:261-400`
- 但 syscall 只实现了 0/1/2：
  - `UBR/io_uring/sched_hints.c:615-630`

原因：

- 这属于内核接口缺口
- 没有完整 kernel build / boot / regression 验证条件下，我没有直接改 syscall 分发表

### 8.3 `readv/writev` 的 socket io_uring 路径不稳定

现状：

- `clickhouse_zerocopy_v2.c:714-786`
- 现在只对 regular file 走 io_uring
- socket 上的 `readv/writev` 回退到原始 syscall

原因：

- 在当前测试中，socket vectored path 用 io_uring 容易出现不稳定 / 卡住
- 为了先保证 ClickHouse/fio/HFT 主路径的正确性，我没有继续把 socket readv/writev 强行走 io_uring

## 9. fio 劫持与验证

### 9.1 代码和运行入口

新增内容：

- `run_fio_zerocopy_v2.sh`
- `fio_zerocopy_v2.fio`

思路：

- fio 在 `ioengine=psync` 下大量使用 `pread/pwrite`
- 现在 v2 库已经补齐 `pread/pwrite`
- 所以可以直接：
  - `LD_PRELOAD=./libclickhouse_zerocopy_v2.so fio ...`

### 9.2 推荐运行方式

示例：

```bash
./run_fio_zerocopy_v2.sh fio_zerocopy_v2.fio
```

或者更明确地跑单项：

```bash
fio --name=seqread \
    --filename=/tmp/fio_smoke.dat \
    --size=64m \
    --rw=read \
    --bs=128k \
    --ioengine=psync \
    --runtime=3 \
    --time_based \
    --group_reporting=1

LD_PRELOAD=./libclickhouse_zerocopy_v2.so \
fio --name=seqread \
    --filename=/tmp/fio_smoke.dat \
    --size=64m \
    --rw=read \
    --bs=128k \
    --ioengine=psync \
    --runtime=3 \
    --time_based \
    --group_reporting=1
```

### 9.3 本机实测结果（2026-03-13）

说明：

- 这是当前机器上的 smoke benchmark
- 文件在 `/tmp`
- 不是论文中的 NVMe 环境
- 只能用来证明“fio 确实被 preload 劫持且能跑”，不能拿来替代论文图表

#### 顺序读，128 KiB，`psync`

- baseline:
  - `READ: bw=40.1 GiB/s`
  - `avg lat ~= 2957 ns`
- preload:
  - `READ: bw=43.7 GiB/s`
  - `avg lat ~= 2706 ns`

结果解释：

- 在这个非常短、缓存友好的本机场景下，preload 版顺序读略好
- 这更像“路径被成功改道”的信号，不足以说明真实 NVMe 上一定同样提升

#### randrw，16 KiB，70/30，`psync`

- baseline:
  - `READ bw=6931 MiB/s`
  - `WRITE bw=2973 MiB/s`
- preload:
  - `READ bw=5814 MiB/s`
  - `WRITE bw=2492 MiB/s`

结果解释：

- 小块随机混合 I/O 在当前实现里反而变慢
- 这和论文对 random R/W 的结论并不矛盾，因为这里不是论文环境，也没有真正的 UBR buffer/调度打通
- 当前 v2 本质上还是“每次调用都同步 submit+wait 的 io_uring preload”，对小块混合 I/O 未必划算

## 10. Synthetic HFT workload

### 10.1 我实现的 workload 是什么

新增文件：

- `synthetic_hft.c`
- `run_synthetic_hft_zerocopy_v2.sh`

pipeline：

1. feeder 线程用 raw `sendto()` 发 UDP tick  
   代码：`synthetic_hft.c:156-191`

2. 主线程：
   - `poll()` 等待 UDP 数据
   - `recvfrom()`
   - 做轻量计算（mid price / EMA / signal）
   - `pwrite()` 写审计日志
   - `send()` 通过 loopback TCP 转发  
   代码：`synthetic_hft.c:356-425`

3. sink 线程用 raw `read()` 把转发流量吃掉  
   代码：`synthetic_hft.c:193-219`

我故意把 feeder/sink 放在 raw syscall 上，是为了避免它们本身也被 preload 干扰；这样更接近“只测主路径”的目的。

### 10.2 为什么它可以代表 HFT 类问题

它保留了 HFT 里几个关键特征：

- 小消息、频繁到达
- 轻量计算，而不是重算子
- 对 end-to-end 延迟和 tail latency 敏感
- 同时经过：
  - 网络接收
  - 存储落盘
  - 网络转发

当然，它仍然只是 synthetic workload，不是生产级撮合 / 行情系统。

### 10.3 我为了让它稳定收敛做了什么

因为 loopback UDP 很容易被发送端淹没，我加了：

- 大 socket buffer
- feeder 周期性 `sched_yield()`
- 主线程 `poll()` 超时保护
- dropped tick 统计

对应代码：

- `synthetic_hft.c:162-186`
- `synthetic_hft.c:253-255`
- `synthetic_hft.c:283`
- `synthetic_hft.c:353-383`
- `synthetic_hft.c:458-472`

### 10.4 本机实测结果（2026-03-13）

命令：

```bash
./synthetic_hft -n 5000
ZEROCOPY_THRESHOLD=1 LD_PRELOAD=./libclickhouse_zerocopy_v2.so ./synthetic_hft -n 5000
```

#### baseline

- processed: `3824 / 5000`
- dropped: `1176`
- throughput: `0.13 M ticks/s`
- avg latency: `1.97 ms`
- p99 latency: `3.37 ms`

#### preload (`ZEROCOPY_THRESHOLD=1`)

- processed: `1075 / 5000`
- dropped: `3925`
- throughput: `0.03 M ticks/s`
- avg latency: `10.48 ms`
- p99 latency: `14.55 ms`

### 10.5 结果解读

这个结果非常重要，因为它说明：

- 当前这版通用 v2 preload **并不适合** 这种 tiny I/O、极端 latency-sensitive 的 HFT 路径
- 原因不是 preload 机制本身一定错误，而是当前实现策略是：
  - 每个小 `recvfrom/pwrite/send` 都同步 submit+wait
  - 对 tiny packet workload，这个固定成本太高

所以：

- 我完成了 synthetic HFT workload
- 也完成了同类劫持
- 但当前实现没有把它加速起来，反而变慢

这不是坏消息，恰恰说明：

- 当前仓库的用户态 v2 方案更适合大块 I/O
- 如果真要把 HFT 做快，需要进一步做：
  - 小 I/O batching
  - busy-poll 或更细的 ring batching
  - 更激进的网络侧 bypass
  - 或真正把论文里的 scheduler / chain / unified buffer 路径打通

## 11. 当前最值得继续做的下一步

如果继续推进，我建议优先级是：

1. 真正把根目录 v2 和 `UBR/io_uring/unified_ops.c` 接起来  
   现在两条线只是“概念一致”，不是“代码连通”。

2. 给 `io_uring_sched_hints` 真正补齐 chain syscall 入口  
   不然 priority inheritance 只停留在内部数据结构。

3. 给 HFT 做 tiny-I/O 专门策略  
   当前通用 preload 在 HFT 上已经证明不合适。

4. 再决定要不要把 socket `readv/writev` 的 io_uring 路径重新做稳  
   这不是当前最值钱的问题，先别在这里消耗太多时间。

## 12. 本次实际改动清单

### 已修改

- `clickhouse_zerocopy_v2.c`
- `test_zerocopy_v2.c`
- `Makefile.v2`
- `Makefile.v3`

### 已新增

- `synthetic_hft.c`
- `run_synthetic_hft_zerocopy_v2.sh`
- `run_fio_zerocopy_v2.sh`
- `fio_zerocopy_v2.fio`

## 13. 本次验证过的内容

### 已验证

- `make -f Makefile.v2 clean all test_zerocopy_v2`
- `LD_PRELOAD=./libclickhouse_zerocopy_v2.so ./test_zerocopy_v2`
- `./synthetic_hft -n 5000`
- `LD_PRELOAD=./libclickhouse_zerocopy_v2.so ZEROCOPY_THRESHOLD=1 ./synthetic_hft -n 5000`
- fio `psync/pread` 顺序读 smoke benchmark
- fio `psync/pread+pwrite` randrw smoke benchmark

### 未验证

- 完整 UBR 内核重新编译、安装、重启验证
- `IORING_OP_UNIFIED_OPS` 在当前内核上的完整端到端回归
- 真正 NVMe + 100/200GbE 硬件上的论文级复现

这部分我没有编造结果。
