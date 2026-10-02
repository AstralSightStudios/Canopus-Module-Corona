# Interconnect 主题包文件传输协议（草案）

interconnect 的应用层载荷使用文本。当前目标设备上实测单条文本长度约为 20,000 个字符；这是实测值，不是平台保证的固定上限。初始按完整消息不超过 18,000 字符设计，并根据实机重新校准。

## 设计原则

- 传输协议不传 ZIP：分享用 `.crpack` 解包后，链路上是一棵 `corona.json` 与资源文件组成的目录树，按本协议逐文件传输。
- Interconnect 传输层只枚举和传输文件，不解释或改写 `corona.json`，也不重排或改名任何资源相对路径。分享包校验属于发送端的导入层。
- 不设 `packageId` 或传输版本代次。资源的官方 ID 始终等于 `themeId`，它也是短的包目录名；可选 `versionCode` 仅为包元数据，不改变传输/续传身份；更新时先删除旧包再完整重传。
- 一次只允许一个主题上传、一个文件传输；文件内部使用窗口分片。数据包头只带文件序号和分片序号。
- 手环不回读、不计算整文件哈希；只逐片解码和写入。完成表示所有声明分片均写入成功，不是端到端内容校验。
- 更新当前正在使用的主题前，必须先切换到其他主题或默认资源；活动主题目录不可直接删除。中断后可按同一份文件清单续传；重新更新则显式删除并重传。

## 可分享资源包格式（CRPack v1）

`.crpack` 是给用户保存和分享的 ZIP 容器，不是链路载荷。发送端识别容器、展示元数据并安全解包后，将根目录下的 `corona.json` 和资源文件组成目录树交给传输协议；`corona.json` 原样传输并保存到设备主题目录，供 Manager 展示包信息及规则。包内资源相对路径保持不变；协议中的 `T` 文件清单仍由发送端按实际待传输文件生成，和这里的 JSON manifest 不是同一份清单。分享包不包含 `mappings.tsv`：接收完成后，设备 Manager 从 `corona.json` 生成主题目录内派生 `mappings.tsv`，另保存资源文件清单以便目录映射做静态分层合并。资源管理页保存资源包顺序和“系统样式”分界；首页重载时才从分界上方的资源包生成模块读取的活动 `internal://files/mappings.tsv`。

### 配置文件名与旧包兼容

- 新生成、重新导出以及导入后提供的文件树统一使用根目录 `corona.json`；JSON marker 和 `formatVersion: 1` 不变。
- 导入与 Manager 接收兼容旧的根目录 `canora.json`。两种文件名合计必须恰有一个普通文件；同时出现时拒绝，即使内容一致。两者使用相同的格式、64 KiB、CRC、总大小和路径安全校验；均不能作为资源目标、目录或祖先目录。
- 编辑器规范化旧 manifest 的文件名时保留原始 JSON 字节；原包备份保持原样。规范化必须在生成传输清单之前完成。Interconnect 开始传输后不得改名，旧发送端仍可按原名传输 `canora.json`，Manager 按实际清单读取并保存。
- 已安装旧主题仍可读取 `canora.json`；新旧配置同时存在或配置内容损坏时不得静默回退到另一份。下文示例统一使用新文件名，兼容旧输入的规则适用于所有导入、接收和已安装主题读取。
- 若续传清单中的配置文件名发生变化，必须用 `replace` 重新传输，不能沿用旧分片。

### 识别与布局

推荐扩展名为 `.crpack`，但扩展名只是提示，不能单独作为识别依据。发送端必须同时确认文件是可读取的 ZIP，ZIP 根目录中恰有一个大小受限的 `corona.json`（或旧输入 `canora.json`），且其中 `format` 精确为 `canopus-resource-pack`、`formatVersion` 为整数 `1`；普通 ZIP、错误 marker 或不支持的格式版本都不能作为资源包发送。ZIP 中不应再包一层同名目录，字段名和 ZIP 条目路径均区分大小写。

```text
dark.crpack  (ZIP)
├── corona.json               # 元数据与映射规则；也会传输到设备主题目录
├── app/settings/launcher.bin
└── icons/confirm.bin
```

`corona.json` 使用 UTF-8 JSON（无 BOM），文件大小不超过 64 KiB，根对象至少包含：

```json
{
  "format": "canopus-resource-pack",
  "formatVersion": 1,
  "themeId": "dark",
  "name": "Dark",
  "version": "1.0.0",
  "versionCode": 1,
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

`format`、`formatVersion`、`themeId`、`name`、`mappings` 必填；`version`、`versionCode`、`author`、`description`、`targets` 可选，供发送端和设备 Manager 读取元数据，不参与传输续传或设备兼容性判定。`versionCode` 若存在，必须为 JSON number 类型的非负安全整数（0–9007199254740991）；不接受字符串、null、负数、小数或超出安全整数范围的值。省略时不默认补 0，旧包继续有效。`themeId` 必须符合下方路径约束；`name` 为非空字符串，最多 128 UTF-8 字节；`version` 最多 64 字节，`author` 最多 128 字节，`description` 最多 1 KiB；`targets` 最多 16 项，每项最多 128 字节。v1 不另设 `packageId`：资源官方 ID 始终等于 `themeId`，直接作为协议 `themeId` 和设备主题目录名，不另设 ID 字段。`mappings` 与下述可选 `quickappIcons` 是规范的映射声明来源，合计最多 256 项，顺序即序列化顺序；`source` 是符合模块限制的绝对固件资源路径或有效的 `@quickapp-icon/<package>` 精确图标 key，`destination` 是 ZIP 根目录下的安全相对路径，且必须与包内资源路径相对应。普通路径的目录/文件结尾斜杠类型须相同；快应用语义 key 始终是精确文件规则，包名末尾斜杠不表示目录。Manager 拼接 `themes/<themeId>/` 与 `destination`，生成相对于快应用 `internal://files/` 的目标路径；模块按固件目标补全原生根目录。TSV 不接受绝对目标路径。源路径与补全后的目标路径仍须符合模块的路径及长度限制；Manager 按受支持设备最长原生根目录预检长度。映射字段不得包含 ASCII 控制字节（含 NUL、TAB、CR、LF）或 DEL。允许 `mappings` 为空；只有它与 `quickappIcons` 均为空时，该主题才不产生重定向。Manager 按顺序将每项序列化为 `source<TAB>快应用文件区相对目标路径<LF>`，校验生成结果不超过 32 KiB。接收完成时生成主题目录内的派生 `mappings.tsv`；首页重载时从 manifest 和保存的顺序重新生成活动配置，避免把派生文件当作权威来源。跨包重叠源路径会按优先级静态合并文件；活动规则数仍不超过 256、活动配置不超过 32 KiB。`.crpack` 中不携带 `mappings.tsv`。`targets` 只是作者声明，不能替代固件或资源格式校验。v1 未定义的额外 JSON 字段可忽略；需要改变必填语义时必须提升 `formatVersion`。

### 可选快应用图标声明

```json
{
  "format": "canopus-resource-pack",
  "formatVersion": 1,
  "themeId": "demo",
  "name": "QuickApp icons",
  "mappings": [],
  "quickappIcons": [
    { "package": "ng.lst.corona", "destination": "icons/corona.bin" }
  ]
}
```

`quickappIcons` 可省略；接收端归一化为 source `@quickapp-icon/ng.lst.corona` 的
精确映射。归一化 manifest 只保存合并后的 `mappings`，避免重复序列化声明；原始
传输文件仍原样保存。`package` 是不透明字符串，按原始值精确匹配，不限定 ASCII、
点分段数或包名语法，不 trim，也不按文件路径解释。尾部 `/` 仍属于包名，不形成目录规则。
现有 C 字符串和 TSV 传输不接受 NUL、TAB、CR、LF、其他 ASCII 控制字节或 DEL；
`@quickapp-icon/` 加原包名合计最多 255 UTF-8 字节。这是传输容量约束，不是小米包名规范。
重复包名 key（包括和 `mappings` 重复）拒绝。
`destination` 必须对应包内真实的、小写 `.bin` 后缀文件，不允许目录或 PNG。
目标长度、合计规则数与生成 TSV 大小仍按现有预算校验。

活动配置中该 key 可通过混搭生成 `@system` 目标，资源包内 destination 仍必须对应
真实文件。包名 key 不参与目录展开；不同包优先级按同一包名 key 决定，指定资源包与
系统透传选择沿用混搭语义。模块在 UI owner 查询当前注册应用的实际 BIN 图标路径，
以精确匹配的注册记录为权威来源，包名不参与目录拼接；真实记录若共用图标路径，
不能保证独占效果，展开后的同源规则冲突仍拒绝，保留 last-known-good。
不在 Manager 推测 manifest 文件名，也不写回原生展开结果。当前支持 Band 11
`.139/.155` 和 10 Pro `.043`；PNG/内存 source 仍拒绝。需要新版模块与已登记的精确目标
入口；10 Pro 复用现有 active-name 查询入口。`.043` 尚待实机验证，不因资源包声明而获批。

### 解包与发送校验

- 普通文件条目必须位于 ZIP 根下，不能多套一层 wrapper 目录；安全的目录条目可忽略，拒绝加密条目、符号链接及其他特殊文件、重复路径、绝对路径、反斜杠、空段、`.` / `..`、越界路径、超限 manifest 及解压后超限内容。不得通过路径规范化来“修复”不安全条目。仅支持 ZIP Store/Deflate；校验 CRC，并按实际解压字节数执行限额，不能只信任 ZIP 目录中声明的大小。
- ZIP 内所有普通文件都是待传输文件，包括 `corona.json`；空目录不传输。`fileCount` 和 `totalBytes` 均包含该文件，接收后路径为 `themes/<themeId>/corona.json`。CRPack v1 中不允许携带包内 `mappings.tsv`，避免两份映射来源不一致。
- CRPack 容器不设文件数量上限；Interconnect 单次传输最多 65,536 个文件（受 4 位十六进制文件序号限制），解压总量最多 64 MiB；`corona.json` 最多 64 KiB，并且在 `.crpack` 中必须恰有一份。Manager 生成的主题派生及活动 `mappings.tsv` 最多 32 KiB；派生文件不计入传输 `fileCount` / `totalBytes`。单文件还必须能在协商的 `chunkSizeBytes` 和本地 2,048 片上限内传完。路径安全规则与下方协议接收端校验一致。
- 导入层负责解析并校验 `corona.json` 的映射数组及资源包约束；之后的 Interconnect 传输层仍只传路径、大小和文件数据，不传 ZIP、不重写映射。接收端在登记前验证 `corona.json` 与传输 `themeId` 一致，并从其规则生成主题目录内的派生 `mappings.tsv`；设备端 Manager 展示名称、作者、版本、描述和目标设备，不展示映射规则；元数据文件在登记后损坏时退回显示 `themeId`。设备端 Manager 展示名称、作者、版本、描述和目标设备，不展示映射规则；新包进入排序列表顶部。资源管理页保存包括“系统样式”分界项在内的顺序；分界上方参与活动映射生成，下方不生效。
- marker 用于识别格式，不代表发布者可信或文件安全；v1 没有签名。ZIP CRC 只能发现部分传输损坏，不能证明来源或内容真实性。

## 路径约定

CRPack v1 解包后的传输源是一个主题目录，例如：

```text
corona.json
app/settings/launcher.bin
icons/confirm.bin
```

新资源包必须在根目录恰好包含一个 `corona.json`；旧输入可改为唯一的 `canora.json`，不得两者共存。资源文件直接放在包根目录下的相对路径中。

接收端将文件保存到 `themes/<themeId>/` 下，并原样保留包根目录之后的相对路径：

```text
themes/dark/corona.json
themes/dark/app/settings/launcher.bin
themes/dark/icons/confirm.bin
```

CRPack v1 的 `themeId` 取自 `corona.json`，限定为 1–64 个小写 ASCII 字母、数字、`_` 或 `-`，例如 `dark`；接收端不另行重命名。传输清单中的 `relativePath` 必须原样用于写入；不得规范化、重排或改名。只拒绝绝对路径、空段、`.`、`..`、反斜杠和越界路径。`corona.json` 中的 `destination` 相对于 ZIP 根目录并对应包内资源路径；Manager 根据 `themeId` 加上主题根目录后生成最终安装路径。

Manager 接收 CRPack v1 时解析 `corona.json`、校验规则及映射目标对应的包内文件，并将每条相对 `destination` 展开到 `themes/<themeId>/` 后逐行拼接到主题目录的派生 `mappings.tsv`；同时保存包内文件相对路径和大小。主页重载时只从系统样式分界上方的资源包生成活动 `internal://files/mappings.tsv` 并应用，包内不存第二份 TSV。跨包重叠的源路径按排序优先级解析，配置预算内直接映射到包文件，超预算才合并到不可变活动代次；低层文件填补高层没有的相对路径；同一包内重叠映射仍按模块最长前缀语义解析。当前模块要求最多 256 条有效映射、生成配置最多 32 KiB、最终绝对路径少于 256 UTF-8 字节。主题文件数不等于映射规则数；CRPack 容器及已安装主题文件清单不设文件数量上限，文件清单仍受既有索引存储预算约束；Interconnect 单次传输受文件序号编码限制，最多 65,536 个文件（含 `corona.json`）；manifest 仍最多 64 KiB。旧版 Manager 的 128 文件限制已移除，超过 128 个文件的包需更新接收端 Manager。发送端导入检查文件路径安全和总大小，并在传输前检查文件数是否可由协议编码；接收端按本地上限预检，但当前 Manager 没有可用空间查询接口，不能保证剩余空间，实际写入失败时回 `write-failed`。协议接收层不解释映射规则；Manager 在激活前完成上述校验。

模块发布配置时使用紧凑索引快照，按实际 TSV 字节数及有效规则数分配；定向刷新按需保留旧快照引用，引用释放后回收，不为第二份最大容量规则表永久预留额外 32 KiB Umem。读取及解析仍需临时缓冲，32 KiB 是配置字节预算，不是每份快照的固定常驻开销；分配失败保留 last-known-good 并在后续轮询重试。

## 消息格式

消息首字符是包类型。控制包使用完整字段名的 JSON；大块文件数据使用紧凑文本。AstroBox 发往 Manager 的包继续使用原始协议文本；Manager 发往 AstroBox 时，Vela `connect.send` 的 `data` 必须是对象；现有文本包发送 `{msg: packet}`，新增列表响应直接发送 `{msg: "L", ...}`（见下方 `L`）。固件会将该对象序列化后传回 AstroBox，接收端先解析 JSON：若 `msg === "L"` 则直接读取列表字段，否则取出 `msg` 交给相同的内层包解析器；`tag` 不需要。文本包的 `maxTextChars` 限制内层协议包，Manager 还须确保序列化后的 `{msg: packet}` 不超过本地 Vela 消息上限；列表响应按完整对象的序列化长度限制。一次 `interconnect.send` 是一条独立消息；平台发送成功回调不代表对端已写盘，必须等待应用层确认。

```text
AstroBox -> Manager: H{"version":2,"type":"request",...}
Manager -> Vela API: { data: { msg: 'H{"version":2,"type":"response",...}' } }
Firmware -> AstroBox: {"msg":"H{\"version\":2,\"type\":\"response\",...}"}
```

### `H` — 握手

```text
H{"version":2,"type":"announce","maxTextChars":18000,"maxWindow":4}
H{"version":2,"type":"request","requestId":"peer_42","maxTextChars":18000}
H{"version":2,"type":"response","replyTo":"peer_42","maxTextChars":18000,"maxWindow":4}
```

只支持协议版本 2，不进行 v1 fallback。`maxTextChars` 为本端最大完整消息字符数；双方取较小值。`maxWindow` 为最大分片窗口（当前为 4）。Manager 不提供 `freeBytes`。

`response` 可带可选 `launchToken`，只用于下述自动退出所有权，不改变握手是否有效。普通启动、普通资源安装（仍使用空 launch URI）、旧 Manager 或没有所有权标记的响应都继续是合法握手。Manager **绝不从 H 请求复制或绑定 token**；`announce` 不授予所有权。只有本应用冷启动的首个页面恰为 index、通过公开属性收到合法查询参数且退出权限尚未撤销时，响应才带该冷启动上下文的 token：

```text
H{"version":2,"type":"response","replyTo":"peer_42","maxTextChars":18000,"maxWindow":4,"launchToken":"check_42"}
```

AstroBox 查询已安装列表时，先订阅互联并进行最长 1,500 ms 的 H 探测，**不 launch**。探测得到正常关联响应后直接查询 L，无论响应有无 token，都不 launch、不发 Q，保留已打开的 Manager。只有探测超时才尝试 launch；协议错误、断线等非超时失败不据此重启应用。超时后的查询 launch 使用官方 HAP 页面 URI：

```text
hap://app/ng.lst.corona/pages/index?astroboxCheckToken=<nonce>
```

`<nonce>` 是本次查询独有的 1–64 个 ASCII `[A-Za-z0-9_-]` 字符。Vela 页面查询参数通过 index 的 `public.astroboxCheckToken` 接收，不能放在 `private`。应用 `onCreate` 建立一次冷启动接收器和一次性上下文 gate；六个可作为首屏的路由均在 `onInit` 开始、任何异步工作之前同步消费 gate：index 提供公开参数，其他路由明确记录无 token。首屏缺参/非法参数也消费 gate；之后任一页面再次调用上下文捕获（`onInit`，包括无参或带同一/另一 token 的 index 重进/重新 launch）都会直接撤销退出权限，不能绑定或替换 token。首页 `onRefresh` 和应用 `onHide` 也撤权，避免用户未触摸 Manager 就手动重新打开/切回时被迟到 Q 关闭。`onShow` 不授予权限，接收器 stop/start 不重开 gate。若 H 早于首屏捕获，不附带标记，不推测所有权。无需未文档化的 `getSource`、`onRequest` 或应用生命周期参数。HAP URI 语法及公开属性注入规则来自平台文档；目标固件的实际 launch/query 注入行为仍需实机验证。未响应的已有应用可能被 timeout fallback 导航，但这不能证明是新启动，token gate 保证不据此取得退出权限。

launch 后 AstroBox 仍须完成正常的关联 H：只有 `response.replyTo` 正确、握手字段有效，且 `response.launchToken` **精确等于本次 nonce** 才确认自动清理所有权。握手失败/超时、旧版或无标记响应、token 不符均不得发 Q，必须保留 Manager；握手本身不因缺少所有权标记而失败。已确认所有权后才可在列表成功或失败时尝试 Q。

握手完整时序如下：Manager 在应用启动时探测到链路已打开，或收到连接打开事件（包括重连）时主动发送 `announce`；若链路未打开，则等 `onopen` 后发送。AstroBox 在订阅互联事件后发送 `request`（普通安装与上述超时 fallback 会先 launch），`requestId` 必须为 1–64 个 ASCII `[A-Za-z0-9_-]` 字符。Manager 收到每个合法 `request`，无论此前是否已发送 `announce`，都必须回复 `response`，并以 `replyTo` 原样匹配请求 ID；重复请求可重复回复同一响应，处理具幂等性。收到 `announce` 或 `response` 不回复，避免握手回环。典型顺序为：

```text
Manager -> AstroBox: announce（链路打开时）
AstroBox -> Manager: request（订阅后；需要 launch 的流程在 launch 后）
Manager -> AstroBox: response（replyTo 匹配 requestId）
```

两端连接事件与订阅/launch 的先后可能不同；若 AstroBox 的 `request` 先于 Manager 的 `announce` 到达，Manager 仍须响应，之后照常按链路生命周期发送 `announce`。AstroBox 收到 `announce` 后会立即重发 `request`，并每 750 ms 重试一次，最多持续 8 秒；重试必须沿用同一个 `requestId`。响应只按 `replyTo` 与待处理请求的 `requestId` 匹配，不因先后顺序或重复请求而改变关联。

### `L` — 已安装资源包列表查询

发送端在 `H` 握手后发送文本请求，不需要额外的 `type` 字段：

```text
L{"requestId":"list_42"}
```

`requestId` 必须为 1–64 个 ASCII `[A-Za-z0-9_-]` 字符。Manager **直接发送结构化对象**，以 `msg: "L"` 标记列表响应；列表 JSON 不再序列化成字符串嵌入 `msg`。这是一种新增响应格式，现有 `H/T/P/F/A/C/E` 及新增 `Q` 响应仍使用 `{msg: packet}`。

```json
{
  "msg": "L",
  "replyTo": "list_42",
  "pageIndex": 0,
  "done": true,
  "total": 1,
  "items": [
    {
      "themeId": "dark",
      "name": "Dark",
      "version": "1.0.0",
      "versionCode": 1,
      "author": "Example",
      "metadataStatus": "ok"
    }
  ]
}
```

Vela API 调用为 `connect.send({data: responseObject, ...})`；固件负责对象的链路序列化。发送端解析外层 JSON 后，先判断 `msg === "L"` 并直接读取同层字段；其他 `msg` 继续交给现有文本包解析器，不能把单独的 `"L"` 再当作内层 JSON 包解析。

- 列表以 `interconnect-themes.json` 的已安装索引为准，按 `themeId` 的 ASCII 顺序返回；不扫描目录、不包含未登记的上传包，也不包含“系统样式”项。
- 每项只包含 `themeId`（也是资源官方 ID）、`name`、可选的 `version` / `versionCode` / `author` 和 `metadataStatus`。元数据正常时状态为 `ok`；`versionCode` 保留 JSON 数值类型，manifest 未声明时省略，不补 0。缺失、损坏、歧义或不可读取时为 `unavailable`，名称回退到 `themeId`，省略版本、版本代码和作者。沿用 `corona.json` / `canora.json` 兼容规则，不返回映射、文件清单或实际生效状态。
- 每个请求在资源操作锁内构建一份只读快照，不写索引、顺序或迁移结果。发送响应页时不持有资源操作锁。查询不打开上传页面、不更改上传进度；查询错误也不把上传接收状态切成错误。
- 对结构化响应，`maxTextChars` 约束完整对象的紧凑 JSON 长度，且不得超过 Manager 本地 18,000 字符上限。Manager 按实际序列化长度拆页，不使用固定条数；页内每项不可拆分。
- `pageIndex` 从 0 连续递增，`total` 为本次快照总条数，最后一页 `done:true`，其余页为 `false`。空列表也返回第 0 页，`total:0`、`items:[]`、`done:true`。单个条目无法装入协商长度时返回 `response-too-large`，不截断元数据。
- 发送端按 `replyTo` 关联并按页序号去重；只有收到全部连续页和最后一页才替换本地列表。缺页、超时或断线后使用新的 `requestId` 整次重试，忽略旧请求的迟到响应；不增加列表 ACK、续传或持久化快照。重复请求重新读取当时的快照，发送端不要混合不同重试结果。

查询错误同样直接发送对象，不使用上传流程的 `E`：

```json
{"msg":"L","replyTo":"list_42","errorCode":"list-failed"}
```

错误码为 `invalid-request`（JSON、请求 ID 或请求长度无效）、`list-failed`（已安装索引损坏或读取失败）、`response-too-large`（响应无法装入协商长度）。无效请求中只有合法的 `requestId` 才会被复制为 `replyTo`，否则省略 `replyTo`。索引缺失表示空列表，索引损坏不能伪装成空列表。错误响应没有 `items` / 分页字段；接收错误后发送端应丢弃本次已收集的页。发送失败时停止该次响应并等待发送端重新查询，不改变上传状态。

### `Q` — 仅请求退出本次查询拥有的 Manager

AstroBox 只在上述正常关联 H 确认本次 nonce 所有权后发送 Q；握手失败或没有/不匹配标记不得用 Q 清理。列表成功或失败不改变已确认的所有权。Manager 接收侧独立验证冷启动 token 与当前权限，不要求先记录一次 H；知道 requestId 或把 token 放进 H 并不能取得权限。退出不卸载资源包、不改变活动映射，也不取消上传：

```text
Q{"requestId":"quit_42","launchToken":"check_42"}
Q{"replyTo":"quit_42","status":"ready"}
Q{"replyTo":"quit_42","status":"reject","errorCode":"not-owner"}
Q{"replyTo":"quit_42","status":"reject","errorCode":"busy"}
```

- `requestId` 与 `L/H` 相同，必须为 1–64 个 ASCII `[A-Za-z0-9_-]` 字符；`launchToken` **必填**，采用相同字符/长度限制，必须精确匹配本应用冷启动首屏捕获的 nonce。不需要 `type`、版本或强制退出参数。缺失/非法 token 为 `invalid-request`；合法但不匹配或从未绑定 token 为 `not-owner`，不退出。
- 响应使用既有文本信封，例如 Vela `connect.send({data:{msg:'Q{"replyTo":"quit_42","status":"ready"}'},...})`；不是 `L` 的结构化格式。发送端按 `replyTo` 关联。
- 请求在接收队列中排在此前消息之后，并在共享资源操作锁内检查及响应；等待先前文件写入、登记、删除或重载等资源操作完成，不在操作中途终止。匹配冷启动 token 时，`busy` 只取决于本次运行会话的上传活动标记：合法 `T begin`（包括重复 begin、续传清单重放）或用于直接续传的合法 `P` 激活标记；成功 `T finish`、链路 `onclose` / `onerror`、停止/重启接收器清除标记。活动上传中匹配 token 的 Q 回 `busy`，不同 token 仍回 `not-owner`；所有文件 `C` 不等于完成 `T finish`。
- 自动退出权限独立于上传 busy 标记，保存在内存且不可恢复/持久化。任何页面根元素的 `touchstart`（包括滚动/手势）、后续页面上下文捕获（重进/重新初始化 `onInit`）、首页 `onRefresh`、应用 `onHide`、首页既有导航/重载操作、接受并激活合法 `T begin` / `P`、`onclose` / `onerror` / 重连、接收器 stop/restart 都永久撤销本次启动的退出权限，后续 H 不再带 `launchToken`。上传激活即撤权，`T finish` 清除 busy 后匹配旧 token 的 Q 也回 `not-owner`，避免迟到查询关闭上传成功页。首屏 gate 也不因重启接收器而重开。无效的上传请求本身不激活 busy 或夺取权限。
- 每个入队消息捕获当前连接代次；`onclose` / `onerror` / 停止 / 启动使旧代次失效。旧代次尚未执行的消息（包括等待资源锁的消息）跳过，不在重连后激活上传。旧代次已开始的处理仍可按原有流程完成持久化，但异步等待恢复后不得重新激活本会话上传标记；新的 L 仍可读取，Q 不恢复已撤销的权限。
- 只有匹配 token、退出权限仍有效且没有本会话活动上传时才发送 `ready`。磁盘保留未完成或损坏的续传状态不妨碍这样的拥有者退出；Q 不读取、验证、修改或删除持久化上传状态。等 Vela 发送成功回调后，**再次检查连接代次和退出所有权/权限**；期间若用户接管、断线或 stop/restart，保留 Manager，不据旧 ready 终止。确认仍有权限时才停止接收器并调用 `@system.app.terminate()`，此间不释放资源操作锁，ready 后已排队的新请求不会继续执行。发送失败不退出；成功回调不证明对端收到响应，ready 也不证明平台已完成退出。协议不增加对端 ACK、固定延迟或退出完成通知。
- JSON、请求 ID、launchToken 或超过本地 18,000 字符的请求使用 `status:"reject",errorCode:"invalid-request"`；只有合法 ID 才复制为 `replyTo`，否则省略。Q 不使用 `exit-failed` 错误码，不发送上传 `E`，不打开接收页或修改接收进度/状态。接收端 token 校验是防误关的启动所有权关联，不是身份认证协议。
- 无重复请求缓存：退出前的拒绝或发送失败可沿用同一 ID 重试并重新检查，但不能恢复撤销的权限；成功 ready 后不保证重复响应。不支持强制终止或通过 Q 丢弃未完成上传。若平台终止调用异常，已发送 ready 不可撤回，也不追加第二个状态。

### `T` — 文件清单与传输状态

`begin` 声明主题 ID、模式、文件数和总字节数；每个 `file` 条目声明一个包内原始相对路径与文件大小；`end` 结束清单。清单不含映射规则；规则随 `corona.json` 一起作为普通文件传输。

```text
T{"operation":"begin","themeId":"dark","mode":"replace","fileCount":3,"totalBytes":19440}
T{"operation":"file","themeId":"dark","fileIndex":0,"relativePath":"corona.json","sizeBytes":1120}
T{"operation":"file","themeId":"dark","fileIndex":1,"relativePath":"app/settings/launcher.bin","sizeBytes":13580}
T{"operation":"file","themeId":"dark","fileIndex":2,"relativePath":"icons/confirm.bin","sizeBytes":4740}
T{"operation":"end","themeId":"dark"}
```

`mode` 为 `replace` 或 `resume`。`replace` 仅当该主题不是 active 时删除旧包和旧进度，然后开始新传输；`resume` 重发完全相同的文件清单并保留已接收分片。源文件或相对路径发生任何变化都必须用 `replace`，不做哈希意味着不能把新旧内容混合续传。主题必须有且仅有一个根目录 `corona.json`，不接受 `mappings.tsv`。

`fileIndex` 从 0 连续编号。每条 `begin` / `file` 都要等接收端确认后再发下一条；丢失时可重发，相同序号及内容幂等确认。接收端可用 `T{"operation":"ack","themeId":"dark","itemType":"file","fileIndex":1}` 确认条目；`end` 校验文件数、总大小、相对路径重复项、路径安全、空间和本地上限，并确认文件清单恰含唯一根目录 `corona.json`，再回 `T{"operation":"status","themeId":"dark","status":"ready"}`。无效或超限时回 `status:"reject"` 和 `errorCode`。续传时清单必须与已有状态一致，否则拒绝并要求 `replace`。

### `P` — 单文件准备与续传

每次只准备一个文件。`fileIndex` 引用 `T` 清单条目；`sizeBytes` 必须与清单一致；`chunkSizeBytes` 是单片解码后的字节数；`chunkCount` 必须等于 `ceil(sizeBytes / chunkSizeBytes)`。

```text
P{"themeId":"dark","fileIndex":1,"sizeBytes":13580,"chunkSizeBytes":12000,"chunkCount":2}
P{"themeId":"dark","fileIndex":1,"status":"resume","window":4,"receivedRanges":[[0,0]]}
```

响应 `status` 为 `ready`、`resume`、`complete` 或 `reject`；`receivedRanges` 是已写入的闭区间分片序号。文件序号和分片序号均用 4 位十六进制，文件序号范围为 `0x0000`–`0xffff`，因此单次传输最多 65,536 个文件；分片编码可表示单文件最多 65,536 片，实际仍受 Manager 本地 2,048 片限制。接收端根据当前清单的 `themeId` 与 `fileIndex` 决定写入路径，不接受传输端指定绝对路径。

### `F` — 文件数据

```text
F<fileIndex:4位十六进制><chunkIndex:4位十六进制><data:Base91>
```

固定头共 8 个字符，余下部分是 Base91 文件数据。按 `chunkIndex * chunkSizeBytes` 写入当前文件偏移；解码后长度须符合该片预期长度。发送端限制完整 `F` 消息不超过协商的 `maxTextChars`。Manager 接收端固定使用 basE91（Bas Wijnen 变体）字符表，发送端必须使用完全相同的字符表：

```text
ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789!#$%&()*+,./:;<=>?@[]^_`{|}~"
```

测试向量：ASCII `test` 编码为 `fPNKd`。Manager 不再设置人为文件数量上限，仅保留协议文件序号范围（单次传输最多 65,536 个文件）、主题总大小 64 MiB 和单文件最多 2,048 片的限制；没有可用的文件系统剩余空间 API，因此通过本地上限预检，实际写入失败时回 `write-failed`。

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

1. 所有清单文件均收到 `C` 后，发送 `T{"operation":"finish","themeId":"dark"}`；接收端验证 `corona.json`、映射目标与已传文件的对应关系，生成主题目录内派生 `mappings.tsv` 和全包文件清单，确认全包文件完成后登记主题并更新排序；新包排在顶部，然后回 `T{"operation":"status","themeId":"dark","status":"ready"}`。`corona.json` 原样留在主题目录。
2. 资源管理页按顶部优先顺序展示已安装包，并插入可拖动的“系统样式”分界；其上方参与覆盖，其下方暂不生效。每次放手后 Manager 将完整顺序立即写入自己的 `resource-order.json`。新安装包插入顶部；删除包时从顺序及文件清单中移除。
3. 排序和混搭选择都不会立即修改模块活动映射。混搭微调按实际源文件路径列出所有已安装包注册的替换资源，不受系统样式分界影响；每个文件可选择默认、系统原资源或任一注册包，选择保存到 app-scoped `resource-overrides.json`。删除被选资源包时对应选择回退为默认，没有剩余注册包的路径设置会清理。用户点主页重载时，Manager 先按已保存顺序解析默认结果，再应用逐文件选择；显式包选择可覆盖资源管理优先级，系统选择序列化为模块专用的精确文件目标 `@system`，即使位于更宽目录映射内也透传到固件原资源；指定包选择则直接映射到包内文件，无需为逐文件选择复制资源。随后生成活动 `mappings.tsv`。跨包重叠源路径按包优先级解析，预算内用直接文件映射、超预算才静态合并，避免高层缺失文件跳过低层资源；同一包内的重叠源规则仍遵守模块最长前缀规则。生成失败时不发送重载信号；活动配置写入后发送信号并等待匹配版本的模块回执，成功后清理旧活动代次，并释放旧直接映射包的保护；已有活动代次索引先持久化新旧包依赖，重载未确认时删除/替换守卫继续保护可能驻留的包。应用会话共享包快照并复用未变的活动映射，接收/删除包使缓存失效；排序和选择仍在每次重载时读取。缓存命中也发送新的重载版本并等待回执，不检测外部文件修改，也不逐文件检查复制前后的大小/类型。更新同一包时保留其排序槽位。

因此链路上不传 ZIP，也没有 `packageId`；分享用 `.crpack` ZIP 由发送端解包后按树逐文件传输。CRPack v1 仅有 `corona.json` 和资源文件，Manager 重载时才生成模块需要的活动 TSV 配置。
