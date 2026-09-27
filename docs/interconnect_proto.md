# Interconnect 主题包文件传输协议（草案）

interconnect 的应用层载荷使用文本。当前目标设备上实测单条文本长度约为 20,000 个字符；这是实测值，不是平台保证的固定上限。初始按完整消息不超过 18,000 字符设计，并根据实机重新校准。

## 设计原则

- 不用 ZIP。每个主题包本身是一棵目录树，包含一份 `mappings.tsv` 和多个资源文件。
- Interconnect 只枚举和传输文件，不解析、不改写包内的 `mappings.tsv`，也不重排或改名任何包内相对路径。
- 不设 `packageId` 或版本代次。`themeId` 是短的包目录名；更新时先删除旧包再完整重传。
- 一次只允许一个主题上传、一个文件传输；文件内部使用窗口分片。数据包头只带文件序号和分片序号。
- 手环不回读、不计算整文件哈希；只逐片解码和写入。完成表示所有声明分片均写入成功，不是端到端内容校验。
- 更新当前正在使用的主题前，必须先切换到其他主题或默认资源；活动主题目录不可直接删除。中断后可按同一份文件清单续传；重新更新则显式删除并重传。

## 路径约定

传输源是一个主题包目录，例如：

```text
mappings.tsv
app/settings/launcher.bin
icons/confirm.bin
```

接收端将文件保存到 `themes/<themeId>/` 下，并原样保留包根目录之后的相对路径：

```text
themes/dark/mappings.tsv
themes/dark/app/settings/launcher.bin
themes/dark/icons/confirm.bin
```

`themeId` 取自主题包目录名，限定为 1–12 个小写 ASCII 字母、数字、`_` 或 `-`，例如 `dark`；接收端不另行重命名。传输清单中的 `relativePath` 必须原样用于写入；不得规范化、重排或改名。只拒绝绝对路径、空段、`.`、`..`、反斜杠和越界路径。包内 `mappings.tsv` 是普通文件，传输层不读取或修改它；其中的目标路径必须由主题包准备者预先设置为最终安装路径，并与包目录名相符。

主题管理器激活包时再读取该包自己的 `mappings.tsv`，并按原内容写入活动配置后应用；不改写映射路径，这是传输之外的操作。当前模块仍要求有效映射最多 64 条、配置最多 32 KiB、最终绝对路径少于 256 UTF-8 字节。主题文件数不等于映射规则数。包导入检查文件路径安全、文件数、总大小和剩余空间，但不替包解释映射规则；激活前由主题管理器校验 `mappings.tsv` 是否符合模块限制及其目标文件是否存在。

## 消息格式

消息首字符是包类型。控制包使用完整字段名的 JSON；大块文件数据使用紧凑文本。一次 `interconnect.send` 是一条独立消息；平台发送成功回调不代表对端已写盘，必须等待应用层确认。

### `H` — 握手

```text
H{"version":1,"maxTextChars":18000}
H{"version":1,"maxTextChars":18000,"freeBytes":12345678,"maxWindow":4}
```

`maxTextChars` 为本端最大完整消息字符数；双方取较小值。`freeBytes` 为可用字节数（若可获取），`maxWindow` 为最大分片窗口（初始建议 4）。接收端按本地配置限制文件数与主题总大小；不公开总空间与已用空间。当前 Manager 不提供 `freeBytes`。

### `T` — 文件清单与传输状态

`begin` 声明主题 ID、模式、文件数和总字节数；每个 `file` 条目声明一个包内原始相对路径与文件大小；`end` 结束清单。清单不含映射规则，因为包内的 `mappings.tsv` 会作为普通文件传输。

```text
T{"operation":"begin","themeId":"dark","mode":"replace","fileCount":3,"totalBytes":18432}
T{"operation":"file","themeId":"dark","fileIndex":0,"relativePath":"mappings.tsv","sizeBytes":112}
T{"operation":"file","themeId":"dark","fileIndex":1,"relativePath":"app/settings/launcher.bin","sizeBytes":13580}
T{"operation":"file","themeId":"dark","fileIndex":2,"relativePath":"icons/confirm.bin","sizeBytes":4740}
T{"operation":"end","themeId":"dark"}
```

`mode` 为 `replace` 或 `resume`。`replace` 仅当该主题不是 active 时删除旧包和旧进度，然后开始新传输；`resume` 重发完全相同的文件清单并保留已接收分片。源文件或相对路径发生任何变化都必须用 `replace`，不做哈希意味着不能把新旧内容混合续传。

`fileIndex` 从 0 连续编号。每条 `begin` / `file` 都要等接收端确认后再发下一条；丢失时可重发，相同序号及内容幂等确认。接收端可用 `T{"operation":"ack","themeId":"dark","itemType":"file","fileIndex":1}` 确认条目；`end` 校验文件数、总大小、相对路径重复项、路径安全、空间和本地上限，再回 `T{"operation":"status","themeId":"dark","status":"ready"}`。无效或超限时回 `status:"reject"` 和 `errorCode`。续传时清单必须与已有状态一致，否则拒绝并要求 `replace`。

### `P` — 单文件准备与续传

每次只准备一个文件。`fileIndex` 引用 `T` 清单条目；`sizeBytes` 必须与清单一致；`chunkSizeBytes` 是单片解码后的字节数；`chunkCount` 必须等于 `ceil(sizeBytes / chunkSizeBytes)`。

```text
P{"themeId":"dark","fileIndex":1,"sizeBytes":13580,"chunkSizeBytes":12000,"chunkCount":2}
P{"themeId":"dark","fileIndex":1,"status":"resume","window":4,"receivedRanges":[[0,0]]}
```

响应 `status` 为 `ready`、`resume`、`complete` 或 `reject`；`receivedRanges` 是已写入的闭区间分片序号。文件序号和分片序号均用 4 位十六进制，单文件最多 65,536 片；实际文件数受 Manager 本地配置限制。接收端根据当前清单的 `themeId` 与 `fileIndex` 决定写入路径，不接受传输端指定绝对路径。

### `F` — 文件数据

```text
F<fileIndex:4位十六进制><chunkIndex:4位十六进制><data:Base91>
```

固定头共 8 个字符，余下部分是 Base91 文件数据。按 `chunkIndex * chunkSizeBytes` 写入当前文件偏移；解码后长度须符合该片预期长度。发送端限制完整 `F` 消息不超过协商的 `maxTextChars`。Manager 接收端固定使用 basE91（Bas Wijnen 变体）字符表，发送端必须使用完全相同的字符表：

```text
ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~"
```

测试向量：ASCII `test` 编码为 `fPNKd`。Manager 目前本地限制为最多 128 个文件、主题总大小 64 MiB、单文件最多 2,048 片；没有可用的文件系统剩余空间 API，因此通过本地上限预检，实际写入失败时回 `write-failed`。

### `A` — 分片确认

```text
A<fileIndex:4位十六进制><chunkIndex:4位十六进制>
```

仅在该片写入成功并记录接收状态后确认。重复片幂等写入并重复 ACK。最多有 `window` 片未确认；超时只重传未确认片。收到当前文件全部 ACK 后才开始下一个文件。断线重连后重发 `P`，按 `receivedRanges` 补传缺片。

### `C` — 单文件完成

```text
C<fileIndex:4位十六进制>
```

所有分片写入成功后原样回复 `C`；若有缺片则回复当前 `P` 状态。接收端不回读文件、不计算 SHA-256。

### `E` — 错误

```text
E{"themeId":"dark","fileIndex":1,"errorCode":"write-failed"}
```

`errorCode` 可为 `active-theme`、`invalid-manifest`、`invalid-path`、`no-space`、`decode-failed`、`write-failed` 等。错误片不得 ACK；可恢复的中断保留状态供续传。

## 完成与切换

1. 所有清单文件均收到 `C` 后，发送 `T{"operation":"finish","themeId":"dark"}`；接收端确认全包文件都已完成后登记为可用主题，并回 `T{"operation":"status","themeId":"dark","status":"ready"}`。`mappings.tsv` 仍只是被传输的原文件。
2. 用户在 Manager 选择主题后，主题管理器读取该包自己的 `mappings.tsv` 并应用；传输层不拼接规则、不修改映射路径。写入活动配置、发送重载信号并收到模块回执后，才标记为 active。
3. 更新 active 主题时先切换到其他主题或默认资源，再删除 `themes/<themeId>/` 并重传。传输期间旧主题包不可用，但当前活动主题不受影响。

因此没有 ZIP、没有 `packageId`，也不会重写原包内路径：主题 ID 只用于选择短安装目录，`mappings.tsv` 与资源文件都按原相对位置逐个传输。
