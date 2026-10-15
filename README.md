# cpp-echo-client

Windows 原生 TCP/UDP Echo 压测客户端。工程只实现一种数据通路：注册 I/O（RIO）负责数据收发，RIO 完成队列通过 IOCP 通知工作线程。TCP 建连使用 `ConnectEx`，不存在普通 `send`/`recv`、数据面事件轮询或其他回退后端。

## 架构

`src` 平铺 8 个 `.cpp` 源文件，**没有头文件**：7 个业务模块各自是一个 C++20 命名模块（named module），把声明与定义合并在同一个 `.cpp` 中；`main.cpp` 不是模块，直接实现 `wmain` 并 `import` 所需模块。每个模块的职责与其原头文件/实现文件一一对应：

| 源文件 | 模块名 | 职责 |
| --- | --- | --- |
| `main.cpp` | 普通 TU（非模块） | 命令行入口、控制台停止处理器、退出码 |
| `contract.cpp` | `cec.contract` | 参数解析、错误优先级和跨字段验证 |
| `types.cpp` | `cec.types` | Options、Event、Action、Completion、Lifecycle、算术边界与错误分类；保留 MSVC 编译约束 |
| `payload.cpp` | `cec.payload` | 负载构造、计数模式和重复填充 |
| `metrics.cpp` | `cec.metrics` | 原子统计、额度账本、延迟直方图和统计行 |
| `native.cpp` | `cec.native` | Win32/Winsock/RIO 所有者、原生 Slot、请求投递与完成所有权核算 |
| `internal.cpp` | `cec.internal` | 独立 Session / Worker Core、状态机、纯索引 Timer、Event → Action |
| `engine.cpp` | `cec.engine` | RioTransport、静态泛型 WorkerRuntime、完成调度、Worker 创建/停止/Join/最终汇总 |

模块依赖保持原有分层方向：`cec.types` 为叶子，`cec.metrics`／`cec.payload`／`cec.contract` 依赖它，`cec.native` 依赖 `cec.metrics`，`cec.engine` 依赖 `cec.internal` 与 `cec.native`，纯引擎模块（`types`／`metrics`／`internal`）不得 import 原生模块。头文件里原本对外可见的依赖用 `export import` 保持传递可见性，仅实现内部使用的依赖保持私有 `import`。Windows/Winsock SDK 头放在每个模块的 `module;` 全局模块片段中。

执行链为 `Native Transport → Completion → Runtime → Event → Pure Core → Action → Runtime → Transport`。Core 不读取 socket、RQ、OVERLAPPED 或 provider outstanding；Runtime 仅在 Transport 已核算全部完成后发布 `generation_drained`。`cec_worker_runtime<Transport, Clock>` 由编译器静态专门化，测试可替换 Transport 和时钟，生产热路径不增加虚表或每次调用分配。

- 会话按工作线程分片，每个工作线程独占一个 RIO CQ、一个 IOCP 和一块预注册内存。
- TCP 用 `ConnectEx` 建连；连接成功后创建 RIO RQ，预投递带 `RIO_MSG_WAITALL` 的整批接收并用 RIO 发送批次，正确处理部分发送。整批完成后仍以完整 `memcmp` 判定回显，哈希不替代字节比较。
- UDP 建立 connected UDP socket 后，所有负载数据仍只通过 RIO 收发。
- CQ 唤醒：IOCP 只表示“RIO CQ 可读”；线程批量 `RIODequeueCompletion` 排空 CQ 后调用 `RIONotify` 重装通知。
- 超时、节拍和重连由工作线程的有界状态机管理；每个会话最多占一个预分配索引堆节点，IOCP 直接等待最近截止时间，不做固定 10 ms 全会话扫描。有限 `/n` 由每个会话独立扣减，失败和重连不补回已发起的额度；共享原子计数只用于汇总。
- 工作线程的数据面等待由 IOCP 通知和最近截止时间驱动；主协调器仅以最长 10 ms 间隔采样线程终止、停止和统计控制状态。因此不能把整个进程描述为“无轮询”，但该控制面采样不处理负载数据。
- 正常停止会先关闭 socket 请求取消，再继续回收所有 `ConnectEx` 与 RIO 终态完成；注册内存、请求上下文、RQ 和 CQ 保持有效直至 `outstanding == 0`。
- `RIONotify` 只有 `ERROR_SUCCESS` 被接受；返回的其他状态直接作为原生错误报告。`RIO_CORRUPT_CQ`、必需的通知失败和不可缺少的 IOCP 控制投递失败均以退出码 4 确定性终止，不重试、不轮询 CQ，也不切换到普通 Winsock 负载路径。

## 构建

依赖 Windows 与 **Visual Studio 2026**（MSVC v145 x64 工具集、C++20 命名模块、clang-format）。整个实现就是一个工程 cpp-echo-client.vcxproj，同一个工程文件提供四套配置：Debug/Release 产出产品 cpp-echo-client.exe（编译 src/*.cpp），DebugTests/ReleaseTests 产出测试 cpp-echo-client-tests.exe（src/*.cpp + tests/*.cpp，入口 tests/cec_test_entry.cpp）。测试可执行文件按套件运行：cpp-echo-client-tests.exe core|runtime|engine|contracts|fault <mode>。build.ps1 用 vswhere 定位 MSBuild 与 VS 自带 clang-format，先做格式检查（--dry-run --Werror），再依次构建 Debug 与 DebugTests（Release 同理），然后运行四个单元套件、命名模块源策略、fail-fast 边界、进程契约、配额契约和 reset storm；构建前后比较源码内容快照，构建或测试自身改写源码即报错。模块接口单元标记 CompileAsCppModule 并开启 ScanSourceForModuleDependencies；编译选项对齐原约束（/std:c++latest、/W4 /WX、/permissive-、/Zc:__cplusplus、/Zc:preprocessor、/EHs-c-、/GR-、UNICODE、_WIN32_WINNT=0x0A00），链接 ws2_32、winmm。故障与 reset storm 子进程使用有界等待，原始输出保存在对应构建目录的 artifacts 中。