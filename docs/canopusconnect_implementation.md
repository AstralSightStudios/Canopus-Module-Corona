# CanopusConnect 精简协议的两端实现方案

状态：设备端已接入生产构建并通过 host 验证；AstroBox 发送端仍按下文计划另行实现。设备端实现与实机验证边界见下节。对应[精简协议](canopusconnect_proto.md)和[消融实验](experiments/canopusconnect-ablation.md)。

## 0. 设备端实现位置

- `src/corona_connect.c`：12 字节帧解析、单个 4 KiB 接收缓冲、累计 ACK 与事件重试、控制屏障、租约和接收记录恢复、磁盘 SHA-256 校验。
- `src/corona_archive.c`：不依赖系统 zlib 的分步 Store / Deflate ZIP 读取器，检查目录与本地头、CRC、路径和输出预算。
- `src/manager_data.c`：manifest 与引用校验、不可变安装代次、哈希查询、安装恢复记录和 revision 分页 LIST；原生 UI 与接收器共用同一 catalog。
- `src/corona_runtime.c`：串联接收、解压和登记，通过模块 owner 上的 10 ms 定时器推进；TIME v2 可用时启用后台定时器。页面关闭不撤销任务，安装成功不自动应用。
- `src/module.c`：首次激活时启动接收器，初始激活失败时撤回 listener、timer 和临时工作区。

验证入口：

```sh
sh scripts/test-corona-connect.sh
sh scripts/test-corona-archive.sh
sh scripts/test-manager-data.sh
python3 scripts/test-corona-runtime.py
```

这些测试运行实际生产 C 源码和 SDK 适配器，不再只运行 `tests/protocol/canopusconnect_bench.c`。原型与离散事件模型继续保留作实验参考。

当前清理使用 FS v2 的 `rmdir`：在文件回收、目录句柄关闭后按后序删除空目录，先检查 `canopus_services_has(ctx, CANOPUS_SVC_FS, 2)` 再访问新增成员。staging 每次定时步骤最多删除一个目录，安装恢复记录和当前安装目录仍受保护；删除主题时回收该主题全部安装代次。FS v1 或缺少接口时兼容保留空目录，实际 `rmdir` 失败不会被当成成功。ZIP 路径单个组件受 SDK 目录枚举限制，最多 63 个 UTF-8 字节，超过返回 LIMIT；展开后的完整路径仍最多 255 字节。安装目录遍历深度最多 16 层，manifest 最多 64 KiB、256 条映射。manifest / schema 校验在一个有界步骤内完成，资源树遍历每步最多读一个目录项或同步一个文件，登记记录、注册表发布和排序刷新分别推进。

后台定时器不保证 CPU 在息屏后一直唤醒。原生文件调用和 manifest / 登记事务的单次耗时仍须实机测量。尚未执行蓝牙实传、断电恢复、flash 延迟和设备堆峰值验收。ZIP 目录重复与重叠校验用固定 RAM、按条目回读实现，总体为 O(n²)，文件很多时安装总耗时需要测量。

安装发布点是 v2 `interconnect-themes.json` 的原子替换，包含 revision、themeId、安装代次和原始 ZIP SHA；兼容旧数组索引。`registry-install.pending` 仅在完整校验与同步后写入，恢复后才能清理它引用的 staging；CANCEL 在发布前撤销该记录，发布后保持 INSTALLED。发布后的排序或接收记录保存错误不撤销安装结果。旧安装代次在覆盖更新后暂保留，卸载主题时一并回收；活动引用仍受原生管理器的活动配置与刷新事务保护。

下文的队列容量和吞吐数字是原实验基线：相邻 Canopus 当前已采用 16 槽 RX（分配失败回退 4 槽）、4 槽响应队列及每个 owner tick 最多处理 4 请求。新增 L2 修复在同一会话发送失败时保留明文队头，发送时使用独立副本，避免原厂发送接口原地加密后重试造成二次加密；会话失效才丢弃。该修复的 BT 栈帧增加 552 字节，设备栈余量尚未实测。不得将原 4 槽模型直接套用到新固件或据此增加手机窗口。

已验证：

- 接收器的实际生产代码在 ASan/UBSan 下通过正常上传、短写、检查点失败、双槽损坏、旧 token、nonce 重试、ACK BUSY / 未监听、HASH、ACTIVE 和登记提交后的终态恢复测试。
- ZIP 读取器通过 223 个 ASan/UBSan 用例，覆盖 Store / Deflate、descriptor、恶意路径、重复与范围重叠、CRC、组件长度以及 I/O 故障。
- 原生登记三目标 host 测试验证逐步 I/O 预算、提交前后恢复、取消撤销、提交后排序失败和损坏资源重装。
- 完整 runtime 集成测试用真实 CRPack 验证接收到登记、UI apply 与安装交错、失败后重启恢复、CANCEL 与 journal 交错。当前小包场景模块动态分配峰值约 54.4 KiB，不包括固件队列、文件系统内部内存与调用栈，也不是设备堆上限保证。

## 1. 发送窗口的依据

用户补充：AstroBox 的 `send` 最初采用了保守串行实现，可以考虑滑动窗口。重新核对代码后确认：**CPC2 已支持多请求在途，协议不必固定窗口为 1**。

当前具体限制如下：

| 项目 | 已有实现 | 影响 |
| --- | --- | --- |
| 手机 CPC2 pending | 按 request_id 存在 HashMap 中，响应按 ID 分发 | 多个请求可以各自等待响应，不需要修改线上 CPC2 格式。 |
| 手机 connect::send | 持有 connect_lock 等待整次 RPC | 这是当前 DATA 串行的直接原因。 |
| 手机发送入队 | request 短暂锁定 SAR 入队后释放，再等待 waiter | 应保留有序入队，允许多个响应 Future 同时在途。 |
| 设备 L2 RX | 4 槽，满时丢弃请求 | 必须限制连接级在途量，不能直接去锁后无限并发。 |
| 设备 RPC TX | 4 槽 | 回调已经执行但响应入队失败时，手机仍可能超时。 |
| 设备事件 TX | 2 槽，优先级低于 RPC 响应 | 业务 ACK 要累计、合并和保留重试。 |
| owner | 每 10 ms 最多处理一个 RX | 当前理想处理上限约为 100 DATA/s，即约 45.7 KiB/s，实际还要扣除其他请求和 I/O。 |
| BT drain | 有工作 2 ms，空闲 10 ms，每 tick 一帧 | 事件延迟、RPC 响应优先级要进性能模型。 |

设备 Connect 请求在 Manager 管理 RPC 之前分派，不使用 Manager 的单个 pending 槽。尽管如此，它仍共享 RX、RPC TX 和实际蓝牙带宽。

## 2. 两个独立窗口

### 2.1 CPC2 RPC 窗口

它限制“已经提交，但还没收到 CPC2 响应”的 DATA 请求数量。

- 默认 2，可在完成设备验证后增加到 3。
- 当前队列大小下最多给 Connect DATA 3 个总许可，另留一个控制 / 管理请求位置；LIST、QUERY、FINISH 不能在 DATA 外再无限叠加。
- 许可按设备全局管理，不能每个插件、每个模块各自拿到独立 3 槽配额。
- 旧 Supervisor 或未完成窗口改造的 AstroBox 仍可只发一个在途请求，使用相同精简线上格式。
- 4 槽只是当前静态容量，不是“窗口 4 永远安全”的证据；没有给其他任务留余量。8 在途在当前 RX 容量下已经违反实验模型中的队列限制。

### 2.2 业务字节窗口

它限制“已经发送，但尚未获知完整写入结果”的字节数量。

- 默认 8 帧、每帧数据最多 468 字节，即 3,744 字节。
- 每写完 4 帧或末片发送累计 ACK，窗口沿连续写入偏移移动。
- 一个 4 KiB 缓冲可以容纳当前窗口的数据；没有乱序表、每片 bitmap 或每个 Future 对应的设备堆对象。
- RPC 成功只释放 RPC 许可。ACK 更新绝对连续前缀，才允许后续字节越过原有业务窗口。
- 两个窗口都满足才能发下一片，不能将 CPC2 成功当作磁盘确认。

## 3. AstroBox 改动

### 3.1 Core Connect 层

当前相关位置：

- `src-tauri/modules/core/src/device/canopus/connect.rs`：现有 connect_lock、send、listen。
- `src-tauri/modules/core/src/device/canopus.rs`：pending waiter、request_id、请求入队与响应分发。
- `src-tauri/modules/core/src/device/res_pack.rs`：目前的 CRPack / Interconnect 安装与查询入口。

建议先增加供 core 资源包安装器使用的有界发送会话，不直接改变所有插件的调用并发保证：

1. 在 device Canopus 状态中保存可用许可和有序发送队列；模块 sender 只能领取设备级预算内的许可。
2. 将“领取许可 → 按既定 offset 注册 waiter 并入队”与“异步等待响应”拆开。顺序由一个 writer 决定，不能通过多个独立任务竞争 writer 锁来碰运气排序。
3. 保留现有 request_id 关联，不另加 DATA 请求 ID。每个 waiter 完成、取消或超时时都释放许可并移除 pending，不泄漏槽位。
4. 超时不等于设备未执行。一次 DATA 超时立即暂停补窗，已有任务受控收尾；若真实链路上仍可能存在迟到请求，通过 OPEN 切换租约后恢复。
5. 断线使会话停止发新请求，清理 waiter；重连先重新监听、HELLO、OPEN，不沿用旧 ACK 的业务额度。
6. 若公开插件 API 也要支持并发，再明确写入顺序和限流契约；先完成内置安装路径，可以减少对已有插件行为的影响。

### 3.2 安装器状态

手机只保留一份上传任务：ZIP 文件句柄、大小、SHA-256、token、ackedOffset、sendOffset、最多 3 个在途描述，以及一个控制请求。数据从原文件按 offset 读取，不把解压文件树或整包复制到每个 Future。

```text
listen → HELLO → OPEN
                   │
              ordered writer
                   │
       RPC 许可与字节额度均足够？
                   │ 是
              提交下一个 DATA ───┐
                   ▲             │ CPC2 响应仅归还许可
                   └─────────────┘
          累计 ACK 移动业务窗口
                   │
      所有数据已写入且 RPC 已收尾
                   ▼
           FINISH → QUERY → INSTALLED
```

原生界面负责后续应用，安装队列仅报告“已安装”。旧的快应用列表检查、launch URI、Base91、逐文件清单和单文件完成流程全部从新路径移除。

## 4. Canopus 设备端改动

### 4.1 L2 层的最小修正

窗口开启前，必须处理当前“执行了回调但没有响应”的队列边界：

- owner 处理 RX 前确认有对应 RPC TX 位置，或先预留响应槽；满时延后取 RX，不能先调用模块再无声丢回复。
- `l2_send` 返回失败时不能无条件清槽；明确哪些失败可重试、哪些意味着会话结束，按队列寿命处理。
- 保持 Manager RPC 响应优先，不能靠大量主动 ACK 抢占管理器。
- 总在途预算要与实际 RX / TX 容量对应，并覆盖来自其他模块、控制命令的请求。
- 不为提高窗口直接扩大全部固件队列；优先用已有 4 槽配合许可和背压。队列扩大属于另一个需要测内存的变量。

L2 请求槽和事件槽属于 Supervisor 的已有常驻成本，不能算成协议“零内存”。host 原型的 4,764 字节也不包括它们。

### 4.2 Corona 接收器

建议在本仓增加独立、可做 host 测试的接收状态机；module.c 只连接生命周期、定时步骤和原生界面。

数据工作区按阶段复用：

```text
接收：4 KiB 写缓冲 + 单帧解析 + 标量状态
  ↓ 缓冲清空、任务受理
校验：复用该 4 KiB 区读取磁盘 + SHA 上下文
  ↓ 哈希通过、释放接收专用临时状态
解压：Deflate 历史窗口 + 小 I/O 缓冲 + 当前条目
```

实现约束：

- callback 内立即验证 token、offset、长度、容量，然后复制；返回后不能保留借用的消息指针。
- 不为多个 RPC 建立多份 480 字节应用队列。buffer 忙或错误时锁存状态，后续帧不得覆盖；不能因底层仍回 CPC2 OK 就忽略业务失败。
- 文件短写必须补齐；失败时连续确认位置不推进。保留这一批数据直到重试 / 租约切换，切换后从已写入前缀覆盖未提交尾部。
- flush 与 callback 在同一 module/UI owner 协调；不得在一次长循环中排完整个 ZIP。单次原生文件调用无法靠协议强行抢占，实际 flash 延迟需要真机测量。
- 累计 ACK 只保存最新待发值；事件队列 BUSY / 未监听时保留，收到 QUERY 时返回同一状态。
- 校验、解压和配置应用均是有界步骤。息屏是否继续取决于 target 后台定时器，不把 ui.post 当后台线程。
- 终态和错误有固定大小表示，详细诊断按需读，不将长路径、JSON 状态树或页面副本常驻。

### 4.3 控制状态和持久化

12 字节帧头缩减了线上开销，但完整控制实现仍需 token 高水位、最近 OPEN nonce / 结果、包哈希、文件大小、检查点及安装代次。host 热路径原型未实现所有这些字段，应在真实模块中再测布局和堆峰值。

接收检查点每 64 KiB 和关闭 / 提交时保存；使用带校验、代次的双槽记录。安装记录和活动配置的恢复仍保留，不能为了缩短文档去掉正在使用文件的引用保护。

当前 SDK 已通过 FS v2 提供真实的空目录删除接口；Corona 在逐文件清理后调用 `rmdir`，取消、卸载和已确认刷新后的过期目录可以回收。FS v1 兼容保留空目录；升级后旧安装代次暂保留，卸载时一并清理。

## 5. 推荐参数与实验结论

首版选择：

| 参数 | 值 | 原因 |
| --- | --- | --- |
| 帧头 / 数据 | 12 / 468 字节 | 删除消息边界重复字段，保留租约和偏移。 |
| RPC 在途 | 默认 2，验证后最多 3 | 获得重叠等待收益，给当前 4 槽队列留余量。 |
| 字节窗口 | 8 帧 | 最多 3,744 字节，可放入一个 4 KiB 数据区。 |
| 累计 ACK | 每 4 帧、末片或 QUERY | 在窗口中段归还额度，比整窗结束才 ACK 更容易维持发送。 |
| 检查点 | 64 KiB，另在暂停 / 完成时同步 | 256 KiB 的建模收益很小，重启重发代价约 4 倍。 |
| 内容校验 | 完整 ZIP 的磁盘 SHA-256 + 解压 CRC | 保留内容身份和文件级检查，不逐片回读比较。 |
| LIST | 固定一条记录 + revision | 最坏 422 字节，无大快照文件及对象。 |

窗口模型中，owner 10 ms 且每 tick 单请求的原始基线下，同一精简方案 `RPC=1/2/3`、ACK 每 4 帧分别约为 `11.42 / 22.83 / 34.23 KiB/s`。这些只是明确假设下的离散事件模型，不能作为预计真机速度向用户承诺。

将 owner 改为 5 / 2 ms 的单独实验有额外收益，但那是 Supervisor 调度变化，不是滑动窗口本身的收益。本轮只记录，不顺带修改固件调度。

## 6. 分阶段验证

1. 先落实生产接收状态机，串行 RPC 下验证精简帧格式、累计 ACK、续传、磁盘 SHA 和安装恢复。
2. 修复设备 RX / TX 背压，再在 AstroBox 开启 2 个 RPC 在途。用同一组真实资源包对比窗口 1 / 2，记录吞吐、CPU、堆峰值、UI 最长阻塞和事件重试。
3. 在有管理请求、其他模块事件、息屏和闪存写入压力下测 3 在途。记录队列峰值，若控制命令响应变差则回退 2，不只比较平均传输速度。
4. 注入 DATA RPC 响应丢失、业务 ACK 丢失、旧 token 的迟到片、重连、短写、检查点失败与重启。特别验证“请求已执行、响应没送达”的幂等恢复。
5. 完成原生目录、解压和安全登记后，再验证完整安装；串行模型和 host inflate 不能替代真实设备的电源故障、持续调度及内存证据。

## 7. 代码依据

以下链接用于当前本地多仓协作：

- [AstroBox pending、request 与响应分发](../../../AstroBox-NG/src-tauri/modules/core/src/device/canopus.rs)：pending 约第 329 行，request 约第 377 行，响应分发约第 839 行。
- [AstroBox Connect send](../../../AstroBox-NG/src-tauri/modules/core/src/device/canopus/connect.rs)：串行锁约第 149 行。
- [Canopus L2](../../Canopus-Private/manager/service/canopus_supervisor_l2.c)：队列第 10 行附近，BT drain 第 143–196 行，owner 第 247–281 行。
- [Supervisor Connect 分派](../../Canopus-Private/manager/service/canopus_supervisor.c)：约第 1516 行。
- [SDK 文件、Connect 与定时器接口](../../Canopus-Private/sdk/c/canopus_services.h)。

行号用于定位本次调查，后续实现会变化；发布时应换成固定提交链接。
