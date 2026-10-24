# Corona 原生管理器 CanopusConnect 协议（精简草案）

状态：设备端接收、磁盘校验、分步解压及原生登记链路已接入生产构建，host 验证见[实现方案](canopusconnect_implementation.md)。手机发送端和真机验收仍待完成；FS v2 目录删除已接入，并保留 FS v1 兼容。先前 host 接收原型、故障注入和参数化传输实验见[消融报告](experiments/canopusconnect-ablation.md)；host / 模型结果不是手环实测速度或内存保证。

本版保留完整 CRPack 上传、断点续传、内容校验和安全安装，采用 **12 字节帧头、二进制控制消息、累计确认、滑动窗口、单个 4 KiB 缓冲**。相较初稿，删除每片 JSON 回执、通用大对象传输及列表快照文件。

## 1. 职责与存储

- AstroBox 负责识别并上传原始 `.crpack`、续传和查询结果。
- 原生 Corona 管理器负责解压、登记、排序、混搭、删除和应用；离线可管理主题。
- Corona Hook 负责不可变配置快照与资源刷新。上传成功、安装成功、应用成功分别展示。
- 接收模块 ID 为 `corona`。所有消息使用 CanopusConnect `binary`，单帧最多 480 字节。

数据根 `R` 通过 `get_data_path` 获取，当前为 `/data/canopus/m.corona`，不使用快应用目录。

```text
R/
├── incoming/                 # 单个 ZIP 和固定长度接收记录
├── staging/                  # 未登记的解压结果
├── themes/<themeId>/<gen>/   # 不可变安装代次，包内路径原样保留
├── registry.*               # 已安装索引及恢复记录
├── resource-order.*         # 排序与系统样式分界
├── resource-overrides.*     # 混搭选择
└── active/<gen>/            # 活动配置及必要的合成资源
```

`gen` 只用于内部提交和文件引用，不改变资源官方 ID `themeId`。所有配置、Hook、字体、背景和日历路径须一起迁根。

分享格式继续使用 CRPack v1，`corona.json` 原样保存，兼容唯一根 `canora.json`，不得两者共存。映射、快应用图标、背景和日历字段语义沿用[原 CRPack 说明](interconnect_proto.md)。旧 Interconnect 的消息、目录、逐文件传输和生命周期规则不再适用。

## 2. 帧格式

本协议版本在 `HELLO` 中协商，当前为 `1`。多字节整数均为小端序；不直接将未对齐字节强转为 C struct。消息边界与载荷长度由 Connect 提供，不再重复携带 magic、长度和保留区。

| 偏移 | 字段 | 类型 | 说明 |
| --- | --- | --- | --- |
| 0 | op | u8 | 操作码；响应设置最高位 `0x80`。 |
| 1 | code | u8 | 请求为 0，响应为结果码。 |
| 2 | seq | u16 | 控制请求序号；响应回显；DATA 和主动累计 ACK 为 0。 |
| 4 | token | u32 | 当前传输租约；HELLO、OPEN 请求和 LIST 为 0。 |
| 8 | arg | u32 | DATA 偏移、ACK 连续写入偏移、OPEN 总大小或 LIST 索引；其余为 0。 |
| 12 | payload | bytes | 最多 468 字节，由 op 确定格式。 |

控制请求使用非零 `seq`，同时最多一个控制请求在途。序号不在同一租约中复用；耗尽时通过 OPEN 换租约。错误响应回显请求的 op、seq、token；不携带进度结构时回显 arg，携带时 arg 始终为 writtenOffset，不含额外字符串。可关联到任务的 QUERY 错误仍返回进度结构；未知 token 只返回 STALE 错误帧。无法关联的坏帧丢弃。

`token` 不是文件 ID 或认证凭据。每次新的 OPEN 控制请求成功，都分配新 token、废弃旧 token；相同 openNonce 和完全相同内容的 OPEN 重试返回同一结果。设备在发放前持久化单调递增的 token 高水位，跨重启不复用，溢出时拒绝发放。旧 token 的迟到 DATA / ACK 均拒绝或忽略。u32 可由手机准确表示，无需 u64 请求 ID。

| op | 名称 | 请求载荷 | 成功响应 |
| --- | --- | --- | --- |
| 1 | HELLO | `version:u16` | 能力结构，见下文。 |
| 2 | OPEN | `sha256:32 bytes + openNonce:8 bytes`，arg 为 ZIP 总大小 | 新 token，进度结构。 |
| 3 | DATA | ZIP 原始字节，arg 为偏移 | 不逐片回应；设备合并发送 `0x83` ACK。 |
| 4 | QUERY | 空 | 进度结构，兼作 ACK 丢失后的恢复查询。 |
| 5 | FINISH | 空 | 进度结构；只受理校验安装，不自动应用。 |
| 6 | CANCEL | 空 | 进度结构，取消未登记任务。 |
| 7 | LIST | `revision:u32`，arg 为条目索引 | 一条资源包记录，见 §7。 |

## 3. 握手与 OPEN

先监听模块事件，再 HELLO。HELLO 响应 token、arg 为 0，载荷固定 20 字节：

```text
version:u16 | chunkBytes:u16 | maxRpc:u8 | windowFrames:u8 | ackEvery:u8 | flags:u8
maxArchiveBytes:u32 | maxUnpackedBytes:u32 | checkpointBytes:u32
```

首版目标值：`chunkBytes=468`、`maxRpc=2`、`windowFrames=8`、`ackEvery=4`、`checkpointBytes=65536`；ZIP 及实际解压总量最多各 64 MiB。flags bit 0 表示跨重启续传能力，其余为 0。首版链路固定 480 字节帧，不协商任意小帧。

`maxRpc` 是设备允许的 Connect DATA 在途请求上限，默认 2；验证有余量后允许 3。手机实际值还须取自身实现上限与设备剩余全局许可的较小值。它与 `windowFrames` 是两个不同限制，不能将“允许 8 帧未业务确认”理解为“同时灌入 8 个 RPC”。

OPEN 通过 `总大小 + 完整 ZIP SHA-256` 唯一识别内容，不重复传主题 ID、版本号或文件清单。openNonce 为手机给本次租约切换生成的随机 8 字节值，重发保留，新的切换更换；它只按字节比较，不需要转成数值。设备只缓存最近一次 OPEN 的 nonce / 内容 / 响应用于去重。主题 ID 在设备解压后从 manifest 读取。无任务时创建；相同内容返回当前阶段和恢复偏移；不同内容已有未结束任务时返回 BUSY，不能自动覆盖。

OPEN 是租约切换屏障：先等待正在执行的有界文件步骤结束，撤销旧租约，再返回新 token。旧租约尚未写入的缓存可丢弃，响应的偏移只包含已完整写入的前缀；不能从未写盘的 RAM 进度继续。手机不得再发送旧租约的数据。

成功 OPEN 续传时清除暂态 OFFSET / 缓冲背压 BUSY 锁存，进入 RECEIVING；IO 错误只有在存储恢复且已写前缀可确认后才允许清除，否则继续报告 IO。HASH / PACK 不通过换租约清除，须先 CANCEL 后重传。正在 VERIFYING / INSTALLING 或已 INSTALLED 的相同包返回该阶段，不重新转为 RECEIVING。

设备只保存一个接收任务及一个最近终态。已安装注册表保存原始 ZIP 哈希，重复安装同一内容可返回 INSTALLED，不重复解压或插入排序。

## 4. 累计确认与滑动窗口

进度结构固定 8 字节，位于 ACK / OPEN / QUERY / FINISH / CANCEL 响应帧头之后：

```text
durableOffset:u32 | windowFrames:u8 | ackEvery:u8 | phase:u8 | detail:u8
```

响应帧头 arg 为 `writtenOffset`，表示已完整写入文件的连续前缀；`durableOffset` 只表示数据同步且检查点保存成功的前缀。保持 `durableOffset <= writtenOffset <= archiveBytes`。detail 为最后一个任务错误码，无错误为 0。

手机维护 `ackedOffset`、`sendOffset` 和在途 RPC 数量：

```text
sendOffset <= min(archiveBytes, ackedOffset + windowFrames * chunkBytes)
inflightRpc <= negotiatedMaxRpc
```

- 新 DATA 通常使用满片 468 字节，最后一片可短。设备内部只接收下一连续偏移；不保存乱序片表或位图。
- 手机由一个有序写入队列按 offset 入队 DATA，同时等待多个 CPC2 响应；多个 RPC 在途不应变成多个线程乱序提交数据。
- CPC2 成功只释放一个 RPC 许可，不释放业务字节额度。只有同 token 的累计 ACK 才移动 `ackedOffset`；ACK 偏移必须不大于该租约已发送偏移和包总大小，旧的较小偏移只忽略，重复 ACK 不得累计增加额度。
- 每连续接收 4 帧、末片到达或 QUERY 时，设备排空这批数据，完整写入后发送累计 ACK。写入失败不得越过失败范围确认。上传回调只复制到单个 4 KiB 缓冲，不申请一份每帧对象或无界队列。
- 写缓冲仍在处理时不得被覆盖。空间不足、I/O 失败或偏移出现缺口，设备锁存错误并停止接收新数据；QUERY 返回实际连续写入偏移。错误期间不继续 ACK 更大的偏移。
- ACK 使用一个“最新待发状态”，队列满时保留并重试，不缓存每帧历史 ACK。连续流的业务额度有限，因此手机停止补窗后，RPC 响应优先的事件发送器仍能排出 ACK。发出成功 ACK 后若只是检查点失败，可冻结任务并将 durableOffset 保持在旧位置，不能回退同租约已经确认的 writtenOffset。
- 已接收前缀内的完整重复片直接忽略，不回读逐片比较；跨越当前前缀或超出总大小则拒绝。首次收到的错误内容最终由磁盘 SHA-256 检出。

### 断线、超时与背压

当前 Connect 返回成功不证明写盘，事件入队也不证明送达。ACK 丢失后，手机暂停补窗，等已有 RPC 完成，再 QUERY；不得直接从旧 ACK 重传整窗。建议 ACK 等待 1 秒，QUERY 无回应再按当前 5 秒 RPC 超时处理，不因一次业务超时自动删除任务。

QUERY 是有序写屏障：等已进入模块的前序 DATA 写步骤结束，排空缓冲后报告进度。它不会撤销租约。若有超时 RPC 可能继续迟到，或需要从较小偏移重放，手机必须 HELLO / OPEN 换 token，再从响应偏移继续，不能用清空手机 Future 假装设备请求也已取消。

发生乱序或缓存背压时使用 Go-Back-N 恢复：停止补窗，排空 / 失效旧请求，通过新 OPEN 获取连续偏移，重发缺口之后的数据。设备不为恢复分配乱序缓冲。

重启后从经检查的 `durableOffset` 恢复；检查点之后的尾部允许覆盖，但 ZIP reader 只能读取声明长度。设备不得依赖尚未公开的任意 `ftruncate` 能力。

## 5. 同步、校验与安装

默认只按 **64 KiB 字节阈值**检查点，不再每 1 秒强制同步。暂停、FINISH 和正常关闭时同步；数据先 sync，再提交带版本与校验的双槽接收记录。检查点失败保留旧 durableOffset，返回错误。

选择 64 KiB 的理由：消融实验中改成 256 KiB 对建模吞吐收益很小，却将重启重传上限提高约 4 倍。同步策略可在实机测得 flash 延迟后调整，不改变线上格式。

FINISH 在全部数据写入后受理后台有界安装步骤，手机 QUERY 进度：

```text
RECEIVING → VERIFYING → INSTALLING → INSTALLED
                         ↘ ERROR
未登记任务可转 CANCELED；INSTALLED 的 CANCEL 不等于卸载。
```

- VERIFYING 必须从磁盘回读完整 ZIP 并核对 SHA-256；不能用接收缓冲区哈希或文件 CRC 代替。
- INSTALLING 合并解压、manifest 校验和登记几个内部步骤，线上不再暴露每个子步骤。失败保留错误码，详细诊断只在本地显示。
- 只支持常规单卷 Store / Deflate，拒绝加密、ZIP64、不支持的压缩方法和特殊文件。核对 central directory、local header / descriptor、CRC、实际输出大小与范围。
- 拒绝绝对路径、反斜杠、空段、`.` / `..`、控制字符、重复文件、文件目录冲突及越界路径；不能靠路径规范化接受不安全条目。
- 当前设备实现受公开 SDK 目录枚举能力限制：单个路径组件最多 63 个 UTF-8 字节、安装目录深度最多 16 层；超出返回 LIMIT，不截断名称。manifest 映射最多 256 条。其他实现可在保持完整原生路径上限的前提下另行提高本地预算。
- 根 manifest 唯一，最大 64 KiB；`themeId` 为 1–64 个 `[a-z0-9_-]`。包中不得自带派生 `mappings.tsv`，所有资源引用须真实存在，展开后的原生路径最多 255 UTF-8 字节。
- 不将全部 manifest token、ZIP 目录或文件清单常驻 RAM。按文件 / 字段流式处理，文件清单可落盘；上限由本地存储预算限制，不能悄悄漏登记。解压总量按实际输出累计。
- 解压到新目录，全部验证后才提交注册表。旧包被活动映射、背景、日历或刷新快照引用时返回 ACTIVE，用户在原生管理器切换后可重发 FINISH。
- 注册表、排序和目录引用使用可恢复提交。不能只因目录存在或一次 rename 成功就宣称安装完成。重复 FINISH 返回现有阶段 / 终态。
- CANCEL 在安全步骤边界停止，终态表示不再登记；清理可以继续。安装成功后回收 ZIP；清理失败不得撤销已提交的安装结果。

## 6. 内存预算

| 阶段 | 要求 |
| --- | --- |
| 上传 / 校验 | 同一个 4 KiB 数据区；另有最多 480 字节收帧及短回执、标量状态和 SHA 上下文。host 原型显式对象合计 4,764 字节，不含调用栈、固件队列和文件系统内部内存。 |
| 解压 | 释放 / 复用上传工作区后再启动；完整 Deflate 可需要 32 KiB 历史窗口，加解码器及小 I/O 缓冲。host zlib 示例约 44.1 KiB，不代表嵌入式实现最终值。 |
| 管理界面 | 与接收和解压共用任务状态，不复制 ZIP、资源目录或完整列表到每个页面。 |

整包始终落盘，不能按 ZIP 大小分配 RAM。两端窗口增大的是允许在途的数据范围，不等于手环新增同等大小的乱序队列。解压器必须可中断、分步推进；32 KiB 历史窗口是完整 Deflate 的格式约束，不能声称靠精简帧头就消除了它。

## 7. LIST：一页一条，不建立大快照

LIST 请求 token=0，arg 为条目索引，payload 为期望 `revision:u32`；首次传 0。响应回显 seq、arg，token=0，前缀为：

```text
revision:u32 | total:u32 | nextIndex:u32
```

有条目时再跟：

```text
flags:u8 | idLen:u8 | nameLen:u8 | versionLen:u8 | authorLen:u8
metadata:u8 | versionCode:u64 | themeId | name | version | author
```

四个字符串使用各自长度指定的 UTF-8 字节，不含 NUL；最大分别 64 / 128 / 64 / 128 字节。flags bit 0 / 1 / 2 表示 version / versionCode / author 存在；未声明的长度和 versionCode 置 0，不能将缺失版本代码解释为 0。metadata 为 0 正常、1 不可用，后者名称回退 themeId。versionCode 仍受 CRPack 的非负安全整数约束。

整帧最坏 422 字节，可放进单条 Connect 消息。空列表仅返回前缀；最后 `nextIndex=0xffffffff`。按 themeId ASCII 顺序分页，不返回 description、文件清单或混搭细节。

每次登记、删除和影响列表元数据的受控修复都递增注册表 revision。读取时在资源操作队列中检查 revision；不匹配返回 CHANGED，手机清空已收页后重查。注册表损坏返回 IO，不能伪装成空列表。稳定查询期间注册表无变化即可逐页读取，不需要设备为手机复制整份目录、维护快照文件或计算列表 SHA。

## 8. 状态码与应用边界

| code | 名称 | 含义 |
| --- | --- | --- |
| 0 | OK | 操作受理 / 成功，是否安装完成以 phase 为准。 |
| 1 | BAD | 格式、版本、长度或字段无效。 |
| 2 | BUSY | 操作冲突或接收背压。 |
| 3 | STALE | 租约 / 任务无效。 |
| 4 | OFFSET | 数据缺口或非法范围。 |
| 5 | IO | 写入、同步、关闭或状态保存失败。 |
| 6 | HASH | 完整 ZIP 的磁盘 SHA 不匹配。 |
| 7 | PACK | ZIP、CRC、路径或 manifest 校验失败。 |
| 8 | LIMIT | 容量或本地预算不足。 |
| 9 | ACTIVE | 旧安装仍被引用。 |
| 10 | CHANGED | 列表 revision 改变。 |
| 11 | UNSUPPORTED | 所需能力未提供。 |

phase 为 0 空闲、1 RECEIVING、2 VERIFYING、3 INSTALLING、4 INSTALLED、5 ERROR、6 CANCELED。ERROR 后是否可恢复由错误来源决定；ACTIVE 可在释放依赖后 FINISH，HASH / PACK 必须 CANCEL 后重新 OPEN。CANCELED 不承诺目录已立即物理清空。

本协议不包含远程应用命令。用户在原生管理器调整排序和混搭，生成新活动代次，通过内部接口发布快照并等待刷新完成。活动映射仍最多 256 条、TSV 最多 32 KiB；刷新未确认前保留旧、新引用，不能删除正在使用的资源。

旧目录迁移、分步解压、原生界面与可恢复安装登记已接入当前设备端。FS v2 提供 `rmdir`，模块按服务版本检测后，在清空并关闭目录后删除空目录；FS v1 保留空目录兼容。更新后的旧安装代次暂保留，卸载主题时一并回收。当前生产构建目标为 `.043` / `.155` / `.179`；三者均需独立实机传输和断电验收，其他固件需独立适配。

两端改动、队列预算、重试与验证顺序见[实现方案](canopusconnect_implementation.md)。原始测量、模型假设和取消某机制的代价见[消融报告](experiments/canopusconnect-ablation.md)。
