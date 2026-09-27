# Interconnect 主题包文件传输协议（草案）

interconnect 的应用层载荷使用文本。当前目标设备上实测单条文本长度约为 20,000 个字符；这是实测值，不是平台保证的固定上限。初始按完整消息不超过 18,000 字符设计，并根据实机重新校准。

## 设计原则

- 传输协议不传 ZIP：分享用 `.crpack` 解包后，链路上是一棵 `canora.json` 与资源文件组成的目录树，按本协议逐文件传输。
- Interconnect 传输层只枚举和传输文件，不解释或改写 `canora.json`，也不重排或改名任何资源相对路径。分享包校验属于发送端的导入层。
- 不设 `packageId` 或版本代次。`themeId` 是短的包目录名；更新时先删除旧包再完整重传。
- 一次只允许一个主题上传、一个文件传输；文件内部使用窗口分片。数据包头只带文件序号和分片序号。
- 手环不回读、不计算整文件哈希；只逐片解码和写入。完成表示所有声明分片均写入成功，不是端到端内容校验。
- 更新当前正在使用的主题前，必须先切换到其他主题或默认资源；活动主题目录不可直接删除。中断后可按同一份文件清单续传；重新更新则显式删除并重传。

## 可分享资源包格式（CRPack v1）

`.crpack` 是给用户保存和分享的 ZIP 容器，不是链路载荷。发送端识别容器、展示元数据并安全解包后，将根目录下的 `canora.json` 和资源文件组成目录树交给传输协议；`canora.json` 原样传输并保存到设备主题目录，供 Manager 展示包信息及规则。包内资源相对路径保持不变；协议中的 `T` 文件清单仍由发送端按实际待传输文件生成，和这里的 JSON manifest 不是同一份清单。分享包不包含 `mappings.tsv`：接收完成后，设备 Manager 从 `canora.json` 生成并保存主题目录内的派生 `mappings.tsv`；用户激活主题时，再根据同一份 manifest 生成模块读取的活动配置 `internal://files/mappings.tsv`。

### 识别与布局

推荐扩展名为 `.crpack`，但扩展名只是提示，不能单独作为识别依据。发送端必须同时确认文件是可读取的 ZIP，ZIP 根目录中有大小受限的 `canora.json`，且其中 `format` 精确为 `canopus-resource-pack`、`formatVersion` 为整数 `1`；普通 ZIP、错误 marker 或不支持的格式版本都不能作为资源包发送。ZIP 中不应再包一层同名目录，字段名和 ZIP 条目路径均区分大小写。

```text
dark.crpack  (ZIP)
├── canora.json               # 元数据与映射规则；也会传输到设备主题目录
├── app/settings/launcher.bin
└── icons/confirm.bin
```

`canora.json` 使用 UTF-8 JSON（无 BOM），文件大小不超过 64 KiB，根对象至少包含：

```json
{
  "format": "canopus-resource-pack",
  "formatVersion": 1,
  "themeId": "dark",
  "name": "Dark",
  "version": "1.0.0",
  "author": "Example author",
  "description": "Example resource pack",
  "targets": ["xiaomi-band-11-4.100.139"],
  "mappings": [
    {
      "source": "/resource/",
      "destination": "app/settings/"
    }
  ]
}
```

`format`、`formatVersion`、`themeId`、`name`、`mappings` 必填；`version`、`author`、`description`、`targets` 可选，供发送端和设备 Manager 展示，不参与传输续传或设备兼容性判定。`themeId` 必须符合下方路径约束；`name` 为非空字符串，最多 128 UTF-8 字节；`version` 最多 64 字节，`author` 最多 128 字节，`description` 最多 1 KiB；`targets` 最多 16 项，每项最多 128 字节。v1 不另设 `packageId`：`themeId` 直接作为协议 `themeId` 和设备主题目录名。`mappings` 是规范的唯一映射来源，最多 64 项，顺序即序列化顺序；`source` 是符合模块限制的绝对固件资源路径，`destination` 是 ZIP 根目录下的安全相对路径，且必须与包内资源路径相对应。两者目录/文件结尾斜杠类型须相同。Manager 拼接 `/data/quickapp/files/ng.lst.corona/themes/<themeId>/` 与 `destination`，生成设备绝对目标路径；拼接后的源、目标路径仍须符合模块的路径及长度限制。映射字段不得包含 TAB、CR、LF 或控制字符。允许 `mappings` 为空，但 Manager 应提示该主题不会产生重定向。Manager 按顺序将每项序列化为 `source<TAB>绝对目标路径<LF>`，校验生成结果不超过 32 KiB。接收完成时生成主题目录内的派生 `mappings.tsv`；激活时从 manifest 重新生成活动配置，避免把派生文件当作权威来源。`.crpack` 中不携带 `mappings.tsv`。`targets` 只是作者声明，不能替代固件或资源格式校验。v1 未定义的额外 JSON 字段可忽略；需要改变必填语义时必须提升 `formatVersion`。

### 解包与发送校验

- 普通文件条目必须位于 ZIP 根下，不能多套一层 wrapper 目录；安全的目录条目可忽略，拒绝加密条目、符号链接及其他特殊文件、重复路径、绝对路径、反斜杠、空段、`.` / `..`、越界路径、超限 manifest 及解压后超限内容。不得通过路径规范化来“修复”不安全条目。仅支持 ZIP Store/Deflate；校验 CRC，并按实际解压字节数执行限额，不能只信任 ZIP 目录中声明的大小。
- ZIP 内所有普通文件都是待传输文件，包括 `canora.json`；空目录不传输。`fileCount` 和 `totalBytes` 均包含该文件，接收后路径为 `themes/<themeId>/canora.json`。CRPack v1 中不允许携带包内 `mappings.tsv`，避免两份映射来源不一致。
- 待传输文件数最多 128、解压总量最多 64 MiB；`canora.json` 最多 64 KiB，并且在 `.crpack` 中必须恰有一份。Manager 生成的主题派生及活动 `mappings.tsv` 最多 32 KiB；派生文件不计入传输 `fileCount` / `totalBytes`。单文件还必须能在协商的 `chunkSizeBytes` 和本地 2,048 片上限内传完。路径安全规则与下方协议接收端校验一致。
- 导入层负责解析并校验 `canora.json` 的映射数组及资源包约束；之后的 Interconnect 传输层仍只传路径、大小和文件数据，不传 ZIP、不重写映射。接收端在登记前验证 `canora.json` 与传输 `themeId` 一致，并从其规则生成主题目录内的派生 `mappings.tsv`；设备端 Manager 展示名称、作者、版本、描述和目标设备，不展示映射规则；元数据文件在登记后损坏时退回显示 `themeId`。当前仓库没有分享包发送端或主题激活 UI 实现。
- marker 用于识别格式，不代表发布者可信或文件安全；v1 没有签名。ZIP CRC 只能发现部分传输损坏，不能证明来源或内容真实性。

## 路径约定

CRPack v1 解包后的传输源是一个主题目录，例如：

```text
canora.json
app/settings/launcher.bin
icons/confirm.bin
```

资源包必须在根目录恰好包含一个 `canora.json`；资源文件直接放在包根目录下的相对路径中。

接收端将文件保存到 `themes/<themeId>/` 下，并原样保留包根目录之后的相对路径：

```text
themes/dark/canora.json
themes/dark/app/settings/launcher.bin
themes/dark/icons/confirm.bin
```

CRPack v1 的 `themeId` 取自 `canora.json`，限定为 1–12 个小写 ASCII 字母、数字、`_` 或 `-`，例如 `dark`；接收端不另行重命名。传输清单中的 `relativePath` 必须原样用于写入；不得规范化、重排或改名。只拒绝绝对路径、空段、`.`、`..`、反斜杠和越界路径。`canora.json` 中的 `destination` 相对于 ZIP 根目录并对应包内资源路径；Manager 根据 `themeId` 加上主题根目录后生成最终安装路径。

Manager 接收 CRPack v1 时解析 `canora.json`、校验规则及模块限制，并将每条相对 `destination` 展开到 `themes/<themeId>/` 后逐行拼接到主题目录的派生 `mappings.tsv`；未来激活时仍从 manifest 重新生成活动 `internal://files/mappings.tsv` 并应用，包内不存第二份 TSV。当前模块仍要求最多 64 条有效映射、生成配置最多 32 KiB、最终绝对路径少于 256 UTF-8 字节。主题文件数不等于映射规则数。发送端导入检查文件路径安全、文件数和总大小；接收端按本地上限预检，但当前 Manager 没有可用空间查询接口，不能保证剩余空间，实际写入失败时回 `write-failed`。协议接收层不解释映射规则；Manager 在激活前完成上述校验。

## 消息格式

消息首字符是包类型。控制包使用完整字段名的 JSON；大块文件数据使用紧凑文本。一次 `interconnect.send` 是一条独立消息；平台发送成功回调不代表对端已写盘，必须等待应用层确认。

### `H` — 握手

```text
H{"version":1,"maxTextChars":18000}
H{"version":1,"maxTextChars":18000,"freeBytes":12345678,"maxWindow":4}
```

握手是双向的：发送端与接收端都应在互联链路可用后主动发送 `H`，不必等待先收到对方的 `H`。接收端应用启动时若链路已经连接，应立即发送；若尚未连接，则等链路打开事件后发送。重连后双方重新握手。收到对方 `H` 时，若本端本轮尚未发送握手，也应回发自己的 `H`；若已发送则不必重复回复，避免握手包来回循环。

`maxTextChars` 为本端最大完整消息字符数；双方取较小值。`freeBytes` 为可用字节数（若可获取），`maxWindow` 为最大分片窗口（初始建议 4）。接收端按本地配置限制文件数与主题总大小；不公开总空间与已用空间。当前 Manager 不提供 `freeBytes`。

### `T` — 文件清单与传输状态

`begin` 声明主题 ID、模式、文件数和总字节数；每个 `file` 条目声明一个包内原始相对路径与文件大小；`end` 结束清单。清单不含映射规则；规则随 `canora.json` 一起作为普通文件传输。

```text
T{"operation":"begin","themeId":"dark","mode":"replace","fileCount":3,"totalBytes":19440}
T{"operation":"file","themeId":"dark","fileIndex":0,"relativePath":"canora.json","sizeBytes":1120}
T{"operation":"file","themeId":"dark","fileIndex":1,"relativePath":"app/settings/launcher.bin","sizeBytes":13580}
T{"operation":"file","themeId":"dark","fileIndex":2,"relativePath":"icons/confirm.bin","sizeBytes":4740}
T{"operation":"end","themeId":"dark"}
```

`mode` 为 `replace` 或 `resume`。`replace` 仅当该主题不是 active 时删除旧包和旧进度，然后开始新传输；`resume` 重发完全相同的文件清单并保留已接收分片。源文件或相对路径发生任何变化都必须用 `replace`，不做哈希意味着不能把新旧内容混合续传。主题必须有且仅有一个根目录 `canora.json`，不接受 `mappings.tsv`。

`fileIndex` 从 0 连续编号。每条 `begin` / `file` 都要等接收端确认后再发下一条；丢失时可重发，相同序号及内容幂等确认。接收端可用 `T{"operation":"ack","themeId":"dark","itemType":"file","fileIndex":1}` 确认条目；`end` 校验文件数、总大小、相对路径重复项、路径安全、空间和本地上限，并确认文件清单恰含唯一根目录 `canora.json`，再回 `T{"operation":"status","themeId":"dark","status":"ready"}`。无效或超限时回 `status:"reject"` 和 `errorCode`。续传时清单必须与已有状态一致，否则拒绝并要求 `replace`。

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

1. 所有清单文件均收到 `C` 后，发送 `T{"operation":"finish","themeId":"dark"}`；接收端验证 `canora.json`，生成主题目录内的派生 `mappings.tsv`，确认全包文件完成后登记主题，并回 `T{"operation":"status","themeId":"dark","status":"ready"}`。`canora.json` 原样留在主题目录。
2. 用户在 Manager 选择主题后，Manager 读取并校验 `canora.json`，按规则数组顺序重新生成活动 `mappings.tsv` 并应用。Interconnect 传输层不拼接规则、不修改映射路径。写入活动配置、发送重载信号并收到模块回执后，才标记为 active。
3. 更新 active 主题时先切换到其他主题或默认资源，再删除 `themes/<themeId>/` 并重传。传输期间旧主题包不可用，但当前活动主题不受影响。

因此链路上不传 ZIP，也没有 `packageId`；分享用 `.crpack` ZIP 由发送端解包后按树逐文件传输。CRPack v1 仅有 `canora.json` 和资源文件，Manager 激活时才生成模块需要的 TSV 配置。
