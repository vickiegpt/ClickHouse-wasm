# 2026-03-13 代码修改说明与测试边界

## 1. 这份文档的目的

这份文档只说明我在本轮中实际修改了哪些代码、修改意图是什么、如何验证、验证边界在哪里。

它和 `BUR_CLICKHOUSE_FIO_HFT_ANALYSIS.md` 的区别是：

- `BUR_CLICKHOUSE_FIO_HFT_ANALYSIS.md` 偏整体分析、论文映射、UBR 与 ClickHouse/fio/HFT 的关系。
- 本文档偏“代码变更说明书”，方便逐项审查具体改动。

## 2. 先说明测试边界

### 2.1 我没有重新编译和安装 UBR 内核

这意味着本轮**没有**验证下面这些内容：

- `UBR/` 目录中的自定义内核改动是否能在新内核里成功编译、安装、启动
- `IORING_OP_UNIFIED_OPS` 的内核侧执行路径
- `io_uring_sched_hints` 的 syscall 接口和调度逻辑在真实重启后的内核中是否按论文预期工作
- 论文里更强意义上的 `SSD -> Compute -> NIC` 内核级 unified pipeline

### 2.2 我实际验证的是什么

本轮验证的是**用户态原型路径**，也就是仓库根目录这组 `LD_PRELOAD` / `io_uring` 实验代码是否：

- 能编译
- 能被单独 `LD_PRELOAD`
- 能正确拦截目标 libc 接口
- 在没有 UBR 自定义内核支持的前提下，正确回退或走标准 `io_uring` 路径
- 能在 `fio` 和 synthetic `HFT` workload 上实际跑起来

换句话说，这些测试的意义是：

- 证明“用户态劫持层 + workload 扩展 + 标准内核 io_uring 路径”是可运行、可观测、可比较的
- 暴露当前通用 preload 方案对不同 workload 的收益和退化
- 为后续真正切到 UBR 内核提供一套基线和冒烟验证工具

这些测试**不等于**论文方案已经被我完整复现。

## 3. 修改总览

本轮纳入提交的文件分成 4 类：

### 3.1 直接修正现有 ClickHouse v2 preload 的文件

- `clickhouse_zerocopy_v2.c`
- `Makefile.v2`
- `Makefile.v3`
- `test_zerocopy_v2.c`

### 3.2 新增 workload / wrapper / fio 配置

- `synthetic_hft.c`
- `run_synthetic_hft_zerocopy_v2.sh`
- `run_fio_zerocopy_v2.sh`
- `fio_zerocopy_v2.fio`

### 3.3 新增分析文档

- `BUR_CLICKHOUSE_FIO_HFT_ANALYSIS.md`
- `CODE_CHANGE_NOTES_20260313.md`

### 3.4 没有纳入本次提交的内容

下面这些文件或目录不属于我这轮代码提交的范围：

- `UBR/`：未修改，且未做内核重新编译安装
- `osdi26-paper270.pdf`：只用于阅读，不应混入代码提交
- `.claude/`：用户环境文件，不应混入提交
- `CLAUDE.md`：这是任务说明文档，不是本轮代码交付的一部分
- `synthetic_hft`、`test_zerocopy_v2`：本地编译产物，不应提交

## 4. 具体代码修改与意图

## 4.1 `clickhouse_zerocopy_v2.c`

这是本轮的主要修复点。

### 4.1.1 补齐被劫持的接口覆盖面

新增或显式维护了以下原始函数指针和包装路径：

- `pread`
- `pwrite`
- `sendto`
- `recvfrom`
- `sendmsg`
- `recvmsg`
- `readv`
- `writev`

修改意图：

- 让 `fio` 的 `psync` 路径能被真正覆盖，因为它典型会走 `pread/pwrite`
- 让 UDP/TCP 以及 `msg`/`iovec` 形式的收发也能进入统一 preload 路径
- 减少“只有 `read/write/send/recv` 被拦截，其它常见入口漏掉”的假阴性

### 4.1.2 修正 io_uring completion 的错误处理

新增统一的 `normalize_cqe_result()` 和 `submit_and_wait()` 处理逻辑。

修改前的问题：

- `cqe->res` 为负时，旧代码大量直接把负值当返回值传出
- 这会导致包装函数没有正确设置 `errno`
- 某些失败路径上行为和常规 libc/syscall 语义不一致

修改后的行为：

- 统一把负 `cqe->res` 转成 `-1`
- 同时将 `errno` 设置为对应正错误码

修改意图：

- 保证 preload 版本和常规系统调用语义一致
- 避免测试程序、fio、其它应用在错误路径上得到不符合预期的返回值

### 4.1.3 改成真正可清理的 thread-local ring 生命周期

新增：

- `pthread_key_t thread_ring_key`
- `pthread_once_t thread_ring_key_once`
- `destroy_thread_ring()`

修改前的问题：

- 旧实现只靠 `__thread` 变量和库析构函数收尾
- 多线程或线程频繁退出时，thread-local ring 的回收并不可靠
- 旧代码还有一组全局 ring pool，但这套共享池在当前同步等待模型里没有形成明确收益，反而增加复杂度

修改后的行为：

- 每个线程按需创建独立 `io_uring`
- 使用 `pthread_setspecific()` 绑定线程退出析构
- 删除未形成稳定收益的全局 ring pool 逻辑

修改意图：

- 降低资源泄漏风险
- 让多线程 preload 的资源生命周期更清晰
- 减少“看起来更复杂但没有被当前执行模型真正使用”的代码

### 4.1.4 增加 SQE 获取与提交重试逻辑

新增：

- `get_sqe_with_retry()`

修改前的问题：

- 旧代码多处各自处理 `io_uring_get_sqe()` 失败
- 行为重复且不一致
- queue 满时回退路径过于随意

修改后的行为：

- 先尝试取 SQE
- 取不到时先提交已挂起请求，再重取一次
- 仍失败则走 fallback

修改意图：

- 统一队列满时的处理语义
- 减少重复代码

### 4.1.5 把 `ZEROCOPY_ASYNC=1` 变成明确的 io_uring hint

新增：

- `maybe_mark_async()`

修改后的行为：

- 在可用内核/头文件上，为请求补 `IOSQE_ASYNC`
- 在支持 `IORING_SETUP_DEFER_TASKRUN` 的环境中，初始化 ring 时带上对应 flag

修改意图：

- 把环境变量从“只有名字，没有明确动作”改成有实际效果的 hint
- 仍然保持兼容：宏不存在时不会编译失败

### 4.1.6 修正 file I/O 与 socket I/O 的分流

新增统一内部函数：

- `uring_file_read()`
- `uring_file_write()`
- `uring_send()`
- `uring_recv()`
- `uring_sendmsg_internal()`
- `uring_recvmsg_internal()`

修改前的问题：

- 旧代码在 `read/write` 路径上把 socket 和 regular file 混在一起处理
- `pread/pwrite` 没有覆盖
- `sendmsg/recvmsg` 逻辑与普通 send/recv 路径重复

修改后的行为：

- `read/write` 先按阈值筛掉小 I/O
- 再根据 fd 类型决定走 socket 路径还是 regular file 路径
- `pread/pwrite` 独立支持偏移读写
- `sendmsg/recvmsg` 与 `sendto/recvfrom` 复用统一内部实现

修改意图：

- 让路径分工更清楚
- 让 `fio`、UDP workload、普通 TCP workload 都能落到正确入口

### 4.1.7 修正 `uring_sendfile()` 的 file->socket 数据路径

这是一个关键 bug fix。

修改前的问题：

- 旧实现直接尝试把 `splice` 从 regular file 送到 socket
- 这种路径在很多场景下并不成立，Linux 常见可行路径通常是 `file -> pipe -> socket`
- 导致测试里“看起来有 sendfile/splice 路径”，但实际不够稳妥

修改后的行为：

- `uring_sendfile()` 改成分块执行
- 路径变为：
  `regular file -> pipe -> socket`
- 使用 `io_uring_prep_splice()` 两段搬运
- 失败且尚未发送任何数据时，回退到标准 `sendfile()`

修改意图：

- 让 file-to-socket 路径符合 Linux 常见可工作模型
- 提高测试和 preload 实现的一致性

### 4.1.8 限制 `readv/writev` 只对 regular file 走 io_uring

修改前的问题：

- 旧代码对 `readv/writev` 的路径选择过于激进
- 在 socket 上直接套 io_uring vectored I/O 的稳定性不够好，且现有测试也更适合先验证 regular file

修改后的行为：

- `readv/writev` 只有在 total size 超阈值且 fd 是 regular file 时才尝试 io_uring
- 否则回到原始 `readv/writev`

修改意图：

- 先保证行为正确和稳定
- 避免为了“覆盖更多函数”把不稳定路径也一并打开

### 4.1.9 更新统计字段语义

修改前的问题：

- 旧统计项命名与实际覆盖范围不再完全一致

修改后的行为：

- 输出改成：
  - `read-like calls`
  - `write-like calls`
  - `send-path calls`
  - `recv-path calls`

修改意图：

- 和新增拦截面保持一致
- 避免把 `pread/pwrite/sendmsg/...` 都记到旧名字里却让人误解

## 4.2 `test_zerocopy_v2.c`

这部分不是“功能扩展”，而是修测试本身的偏差和噪声。

### 4.2.1 创建测试文件时绕过 preload

新增：

- `raw_write_all()`

修改前的问题：

- 创建 fixture 文件时直接用 `write()`
- 在 preload 场景下，这一步本身也会被拦截
- 会把“准备测试数据”混进统计与路径判断里

修改后的行为：

- 改为 `syscall(SYS_write, ...)`
- 文件生成阶段不再污染 preload 统计

修改意图：

- 让基准准备阶段与正式测试阶段分离

### 4.2.2 把 file-to-socket 测试从 `AF_UNIX socketpair` 改成回环 TCP

新增：

- `create_tcp_pair()`

修改前的问题：

- 使用 `AF_UNIX socketpair` 时，无法很好代表目标 sendfile/splice 的 file->socket 路径
- 某些 zero-copy 路径在 UNIX 域 socket 上并不对应真实网络发送场景

修改后的行为：

- 测试改为 loopback TCP

修改意图：

- 让 `zerocopy_uring_sendfile()` 覆盖更接近真实 file->network 的场景

### 4.2.3 修正子进程退出方式和输出缓冲

修改点：

- 子进程退出改为 `_exit()`
- 关键路径前加 `fflush(stdout)` 或 `fflush(NULL)`
- `main()` 中启用行缓冲

修改前的问题：

- 使用 `exit()` 时，子进程会执行用户态析构逻辑
- 容易把 preload 的析构打印和父子进程缓冲输出搅在一起

修改后的行为：

- 子进程只做测试分支本身的收尾
- 避免重复析构输出和缓冲复制噪声

修改意图：

- 让测试输出更可读
- 避免把析构期副作用误判成 I/O 路径问题

### 4.2.4 提升 `readv/writev` 结果可诊断性

修改后的行为：

- `readv/writev` 出错会明确 `perror`
- 返回 0 也会显式打印

修改意图：

- 让 vectored I/O 失败时更容易定位，不再只有“吞掉细节”的结果

## 4.3 `Makefile.v2`

### 4.3.1 修正动态库链接顺序

修改前的问题：

- 旧写法把 `-luring` 等库放在源文件之前
- 在某些链接器行为下，这会导致生成的 `.so` 作为单独 `LD_PRELOAD` 使用时出现未解析符号

修改后的行为：

- 把库依赖拆到 `LDLIBS`
- 链接顺序改为：目标对象在前，库在后

修改意图：

- 保证 `libclickhouse_zerocopy_v2.so` 能作为独立 preload 库正常装载

### 4.3.2 新增 synthetic HFT 相关目标

新增：

- `synthetic_hft`
- `hft-compare`

修改意图：

- 让 HFT workload 的编译与基线/对比运行有统一入口

## 4.4 `Makefile.v3`

这里只做了和 v2 同类的链接顺序修正。

修改意图：

- 避免 v3 版本重复踩同一个 `.so` 链接顺序问题

## 4.5 `synthetic_hft.c`

这是本轮新增的 synthetic workload。

### 4.5.1 workload 结构

设计为：

- UDP `recvfrom()` 接收行情 tick
- 做轻量计算，模拟策略信号生成
- 用 `pwrite()` 写 audit 记录
- 用 TCP `send()` 发出 order/event

修改意图：

- 显式覆盖“网络接收 + 轻量计算 + 文件写入 + 网络发送”这条链
- 让 workload 同时触发 `recvfrom`、`pwrite`、`send`
- 尽量接近论文中 `HFT` 对尾延迟敏感的方向，而不是只做单一 I/O benchmark

### 4.5.2 为了减少测试偏差，刻意绕开 preload 的辅助路径

新增：

- `raw_sendto_once()`
- `raw_read_loop()`

修改意图：

- feeder 线程用原始 `sendto` syscall 发包，避免发送源本身也被 preload 干预
- sink 线程用原始 `read` syscall 吸收 TCP 输出，避免消费端也进入同一 preload 路径

这样做的目的是让实验更集中地观察“主处理线程”的受影响路径。

### 4.5.3 其它稳定性处理

包括：

- 更大的收发 buffer
- 周期性 `sched_yield()`
- `poll()` 超时与 dropped tick 统计
- 输出平均延迟与 P50/P99/P99.9

修改意图：

- 让这个 synthetic workload 更像可以重复运行和比较的实验工具，而不是一次性 demo

## 4.6 `run_synthetic_hft_zerocopy_v2.sh`

功能：

- 检查库和二进制是否已编译
- 统一注入：
  - `LD_PRELOAD`
  - `ZEROCOPY_THRESHOLD`
  - `ZEROCOPY_QUEUE_DEPTH`
  - `ZEROCOPY_DEBUG`

修改意图：

- 给 synthetic HFT 提供固定可复现的运行入口

## 4.7 `run_fio_zerocopy_v2.sh`

功能：

- 检查 `fio` 与 preload 库
- 缺省使用 `fio_zerocopy_v2.fio`
- 统一注入 preload 环境变量

修改意图：

- 给 `fio` 提供与 ClickHouse/HFT 同风格的 preload 封装方式

## 4.8 `fio_zerocopy_v2.fio`

内容：

- `psync` 引擎
- 顺序读
- 顺序写
- `randrw`

修改意图：

- `psync` 能稳定触发 `pread/pwrite`
- 这样可以直接验证 `clickhouse_zerocopy_v2.c` 对 `fio` 的覆盖是否真的生效

## 5. 实测验证

以下是我本轮实际跑过的命令类型：

- `make -f Makefile.v2 clean all test_zerocopy_v2`
- `LD_PRELOAD=./libclickhouse_zerocopy_v2.so ./test_zerocopy_v2`
- `./synthetic_hft -n 5000`
- `LD_PRELOAD=./libclickhouse_zerocopy_v2.so ZEROCOPY_THRESHOLD=1 ./synthetic_hft -n 5000`
- `./run_synthetic_hft_zerocopy_v2.sh -n 1000`
- `fio ... --ioengine=psync ...`
- `./run_fio_zerocopy_v2.sh ...`

### 5.1 这些测试证明了什么

它们证明：

- 新版 v2 preload 能被单独编译、链接、加载
- `pread/pwrite/sendto/recvfrom` 等新增覆盖面可以进入统一拦截层
- `zerocopy_uring_sendfile()` 在 loopback TCP 场景可工作
- fio wrapper 与 synthetic HFT wrapper 可直接运行
- 当前这版通用 v2 preload 对不同 workload 的效果并不一致

### 5.2 这些测试不能证明什么

它们不能证明：

- UBR 自定义内核 opcode 已经正确生效
- UBR 调度 hints 和 priority inheritance 已经在真实内核里验证通过
- 论文里的所有性能提升都在当前环境中被复现

## 6. 目前得到的结论

### 6.1 明确修复并验证过的点

- v2 preload 的链接顺序问题
- `pread/pwrite` 缺失问题
- `sendto/recvfrom` 缺失问题
- `cqe->res` 错误码语义问题
- thread-local ring 回收问题
- file->socket 路径建模问题
- `test_zerocopy_v2.c` 的测试偏差与输出噪声问题

### 6.2 明确观察到但没有粉饰的问题

- 当前这版通用 `io_uring` preload 对 tiny I/O 的 synthetic HFT workload 明显变慢
- `fio` 的 cached sequential read smoke test 有改善，但 `randrw` 退化

这说明：

- 这套用户态 preload 方案可以作为实验与对比框架
- 但不能把“能运行”直接等同于“对所有 workload 都带来收益”

## 7. 未完成和后续建议

如果下一步要验证论文主张，而不是只验证用户态原型，优先级应是：

1. 重新编译并安装 `UBR/` 对应内核
2. 验证 `IORING_OP_UNIFIED_OPS` 与 `io_uring_sched_hints` 的实际系统调用路径
3. 用当前这套 `fio` / synthetic `HFT` / ClickHouse preload 工具做“stock kernel vs UBR kernel”对照
4. 再决定哪些 preload 路径应该继续保留，哪些应该让位于真正的内核链式提交接口
