# cpp-echo-client

Windows 原生 TCP/UDP Echo 压测客户端。工程只实现一种数据通路：注册 I/O（RIO）负责数据收发，RIO 完成队列通过 IOCP 通知工作线程。TCP 建连使用 `ConnectEx`，不存在普通 `send`/`recv`、数据面事件轮询或其他回退后端。

## 架构

- 会话按工作线程分片，每个工作线程独占一个 RIO CQ、一个 IOCP 和一块预注册内存。
- TCP 用 `ConnectEx` 建连；连接成功后创建 RIO RQ，预投递带 `RIO_MSG_WAITALL` 的整批接收并用 RIO 发送批次，正确处理部分发送。整批完成后仍以完整 `memcmp` 判定回显，哈希不替代字节比较。
- UDP 建立 connected UDP socket 后，所有负载数据仍只通过 RIO 收发。
- CQ 唤醒：IOCP 只表示“RIO CQ 可读”；线程批量 `RIODequeueCompletion` 排空 CQ 后调用 `RIONotify` 重装通知。
- 超时、节拍和重连由工作线程的有界状态机管理；每个会话最多占一个预分配索引堆节点，IOCP 直接等待最近截止时间，不做固定 10 ms 全会话扫描。有限 `/n` 由每个会话独立扣减，失败和重连不补回已发起的额度；共享原子计数只用于汇总。
- 工作线程的数据面等待由 IOCP 通知和最近截止时间驱动；主协调器仅以最长 10 ms 间隔采样线程终止、停止和统计控制状态。因此不能把整个进程描述为“无轮询”，但该控制面采样不处理负载数据。
- 正常停止会先关闭 socket 请求取消，再继续回收所有 `ConnectEx` 与 RIO 终态完成；注册内存、请求上下文、RQ 和 CQ 保持有效直至 `outstanding == 0`。
- `RIONotify` 只有 `ERROR_SUCCESS` 被接受；返回的其他状态直接作为原生错误报告。`RIO_CORRUPT_CQ`、必需的通知失败和不可缺少的 IOCP 控制投递失败均以退出码 4 确定性终止，不重试、不轮询 CQ，也不切换到普通 Winsock 负载路径。

## 构建

依赖 Windows、Visual Studio MSVC x64 工具集、CMake 3.28+、Ninja 和 clang-format。脚本通过 `vswhere` 进入 MSVC 开发环境，只检查源码格式，使用静态 CRT，执行干净构建和全部测试。构建前后比较源码内容快照；已有未提交修改可以正常构建，构建或测试自身改写源码则报错。

```powershell
.\build_debug.ps1
.\build_release.ps1
```

需要调整格式时显式运行 `.\format.ps1`；该脚本才会改写源码。格式检查失败时先运行它，审阅差异后再构建。

MSVC 使用 `/std:c++latest`，即已安装工具集所支持的最新 C++ 特性集；这不宣称工具集已经完整实现最终 C++26 标准。

## 运行

```powershell
.\build\release\cpp-echo-client.exe 127.0.0.1 /p tcp /r 7000 /c 256 /threads 8 /n 1000000 /k 8 /z 4096 /stats
.\build\release\cpp-echo-client.exe 127.0.0.1 /p udp /r 7000 /c 128 /threads 8 /n 1000000 /z 1200 /report 1 /stats
```

参数：首个位置参数是目标主机；`/p tcp|udp`；`/r` 远端端口；`/l` 固定本地端口（仅 `/c 1`）；`/n` 每会话 echo 尝试数（0 为无限）；`/t` 操作超时；`/i` 间隔毫秒；`/d` 文本负载；`/z` 二进制计数负载；`/zt` 可打印计数负载；`/k` TCP 批深；`/c` 并发会话；`/threads` 工作线程；`/w` 总运行秒数；`/rc [seconds]` 重连；`/report` 周期统计；`/b` socket 缓冲区；`/cq` 每 CQ 容量；`/memory` 注册内存上限；`/q` 静默；`/stats` 最终统计。

有限工作量在启动前检查 `/n × /c` 是否能用 64 位无符号整数表示，溢出返回退出码 1。当前 TCP `/k` 是批深，每会话实际 RQ 保留 1 个接收和 1 个发送；TCP 与 UDP 的 CQ 容量均至少为最大 worker 会话分片数的两倍。自动 worker 上限为 64。

`/d` 文本在解析时复制到选项自身存储，不借用命令行指针。固定 `/l` 与 `/c 1` 的约束由参数解析器统一验证，并在 Winsock 初始化前以退出码 1 拒绝冲突。TCP 还明确拒绝同时使用自动重连 `/rc` 与固定本地端口 `/l`，避免结果依赖 `TIME_WAIT`；UDP 允许该组合。未知开关和空值严格返回退出码 1。

有限 `/n` 示例不隐含 `/w`：未显式提供 `/w` 时，进程只在完成所请求的 echo、发生终态失败或收到控制台停止信号时结束。

`/h` 只豁免必需的 host 和 protocol，仍检查已提供参数的冲突、载荷与资源容量。节拍、连接和请求超时、重连共享 QPC 截止时间；`/i` 非零时配对请求和释放 1 ms Windows 计时分辨率。等待取整和线程调度仍会增加实际间隔，不承诺实时唤醒；计时设施初始化失败归内部错误。

统计明确报告会话数、echo/s、MiB/s、p50、p99、p999 和近似最大延迟；`~` 表示对数直方图桶的近似下界，`latency_sample=batch` 明确延迟直方图的每个样本对应一次完成的尝试/批次。延迟换算避免乘法溢出，超过整数范围时饱和；最后一个桶仍返回真实下界 `2^63` 微秒。TCP 一个批次包含 `1..k` 个逻辑 echo，有限 `/n` 的最后一批可能少于 `/k`；UDP 每批固定为一个 echo。`echoed` 与 `echo_per_sec` 始终按逻辑 echo 计数，因此不能把批次延迟样本数当作 echo 数。回环吞吐主要反映本机协议栈、调度和内存路径，不代表真实网络或目标 NIC 上限；极限值应结合目标 CPU、NUMA、NIC/RSS 队列和实际尾延迟测量调优。

测试中的 finite-attempt accounting 模型只描述全部尝试已终态的有限场景，不能当作生产主动停止时仍有未完成尝试的 invariant；本实现未在这轮新增 cancelled 统计模型。
