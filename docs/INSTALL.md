# Resource Hook 安装与验收

适用交付：Canopus-Module-Resource-Hook **0.3.0**，支持 Xiaomi Band 11
**4.100.139 / 4.100.155** 与 Xiaomi Band 10 Pro **3.101.043**。各固件使用独立
ELF 和收据，不能混用。这是 ELF + CMI1 交付，不是 canonical `.canopus` 压缩包。
用户已反馈三个目标的正常字体重载实机验收通过（**USER_REPORTED_PASS**），字体事务
现已默认启用；该反馈不自动覆盖 GPU 故障、框架重启或所有资源 owner，也不代表新生成
的每份交付产物均重新做过实机测试。详见[字体重载说明](FONT_RELOAD.md)。

## .155 历史图标单项实机记录

用户确认：在 **4.100.155** 上，通过本地临时安装器
`~/develop/temp/settings-icon-installer-155`，使用当前签名模块，完成安装和主题路径
配置后，自定义**设置启动器图标实际可见**。该流程包含 execute 恢复路径与 `mkdir`
目录创建；这是历史测试流程记录，不是通用安装器已支持这些操作的承诺。
本仓库不整合该临时安装器；后续资源传输使用其他方式。
本次历史测试把 `/resource/app/settings/launcher.bin` 映射到旧目录
`/data/canopus/themes/current/app/settings/launcher.bin`，配置位于旧路径
`/data/canopus/themes/mappings.tsv`。当前配置和主题目录已迁至
`/data/quickapp/files/ng.lst.corona/`；这条历史测试不代表新目录已在所有固件目标上重新验收。
本次交付 ELF 的 SHA-256 为
`3b43d674390459457d4b302c811d516a4518cd4fae599ae8f85949a05abc27fe`；
该值用于识别测试交付，不代表后续构建自动获得相同实机结论。

此处仅记录**用户报告**，不是本次文档整理重新执行的设备测试，也没有新增独立设备
日志或逐项验收记录。可见图标证明这一条实际安装/映射/显示路径成功，不证明其他
图片 owner、字体、动画、离屏页面、GPU 生命周期或压力/恢复场景均通过，也不证明
`.139` 可用。其余[实机验收门禁](#实机验收门禁)仍需分别验证。

## 安装前提

- AP 固件（OTA 中的 `vela_ap.bin`，不是整个 OTA ZIP）SHA-256 必须匹配所选目标：
  - `.139`：`31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`
  - `.155`：`ea0bdf1920cb30223d616432af00565ca67622e6468328f5eab155f8cdc2fb9f`
  - `10 Pro .043`：`519307675665e4866d722a8119a98589c397b614ac3294cb87bfc86de45756ec`
- 已部署兼容 Canopus ABI **1.2**、CMI1 的 Supervisor；其可信签名公钥
  必须与本 payload 的签名者一致。不要为安装此模块关闭签名或固件校验。
- 操作方必须具备文件传输、Manager 控制及故障后的设备重启/恢复手段。
- 模块激活必须由已串行化的 UI 所有者流程执行。短临界区只保护回调发布，
  不提供 UI teardown、在途调用排空或缓存重建协议。

先用**独立可信来源**的 Supervisor 签名公钥验证：

```sh
python3 verify-payload.py . --target xiaomi-band-11-4.100.155 \
  --public-key /path/to/trusted-supervisor-public.pem
shasum -a 256 -c SHA256SUMS
```

`.139` 包将命令中的目标改为 `xiaomi-band-11-4.100.139`。校验器默认 `.139`，
不会从不可信收据自动猜测目标；`.155` 和 `10 Pro .043` 必须显式指定。固件升级前先禁用旧模块并完整
重启，再按新固件重新部署匹配的 Supervisor 和模块，不要只给旧 ELF 换收据。

`signer-public.pem` 是供比对的公钥，不是信任根。单独使用包内公钥只能检查
自洽性，不能确认来源。SHA256SUMS 检查传输损坏；ELF 的真实性由 CMI1
签名验证。说明文件和配置样例没有独立签名。私钥不在交付包中。

## 配置和启用

需要提取或编辑图片时，先阅读[资源图片工具说明](../tools/RESOURCE_IMAGE.md)：
工具仅支持受限 LVGL v9 I8 格式，先做 PNG 预览，再传输 BIN；它不负责模块安装或激活。

1. 备份快应用文件区中的 `mappings.tsv` 和原主题文件。Manager URI 根为
   `internal://files/`，模块原生文件根在 11 上为 `/data/quickapp/files/ng.lst.corona/`，10 Pro 上为 `/data/files/ng.lst.corona/`。以下原生传输示例按 11 展示，10 Pro 请使用其对应根目录。
2. 在 `/data/quickapp/files/ng.lst.corona/themes/current/` 下放置兼容固件的资源文件。
   例如 `/resource/icons/a.bin` 对应
   `/data/quickapp/files/ng.lst.corona/themes/current/icons/a.bin`。
3. 检查 `mappings.tsv.example`，按实际资源源路径修改，然后保存为
   `/data/quickapp/files/ng.lst.corona/mappings.tsv`。样例里的 `/resource/` 只是示例规则，
   不意味着固件的所有字体和图片都经过这个目录。
4. 使用兼容 Manager 的已验签安装流程导入 ELF 和 receipt；对应的 inbox
   文件名为 `corona.ko` 和 `corona.cmi`，目录是
   `/data/canopus/inbox/`。**仅复制文件不等于完成安装**，还必须执行 Manager
   的 install 操作。不要直接编辑 registry.bin，也不要直接调用 ELF。
5. 在 Manager 的模块列表打开 `corona` 详情，点击“启用”并确认。
   “启用”只保存下次启动的启用意图，**不**立即安装 Hook；“已启用”
   不能作为已经生效的证明。
6. 想本次运行就生效，在同一详情页点击**“立即激活”**并确认：它会当场加载并
   激活模块，安装重定向、执行目标支持的资源适配器并请求一次整屏刷新。安全模式、未签名
   模块和仅重启类模块不提供该入口；已常驻的模块也不再显示（不能重复激活）。
   若不使用“立即激活”，则按第 7 步重启。
7. 若走重启路径：按框架的已验证启动流程完整重启设备，重新载入 Canopus
   Supervisor，然后进入 Manager，让 UI 上下文执行启用模块的恢复。不能只重启
   设备就假定框架会自动加载；当前模块不提供系统级开机引导。
8. 查看详情是否为“启动时常驻”，且没有激活错误。只有之后发生的、经过
   LVGL POSIX driver 的新 open 才会被映射。

## 资源重载：定向退休与 owner 适配器

路径映射与资源采用是两件事。首次激活后规则保持不可变；open hook、缓存过滤和
对象刷新共享同一份规则。**不再自动拆除/重建栈顶页，也不停止所有动画。**

### .155 文件图片和图片样式

1. 检查两个缓存的精确类、链表布局和有界可达性，再只退休受影响的**文件** key：
   source 必须是 `/` 开头的有效绝对路径，并能按当前规则成功拼接映射结果。
   未命中、非法/过长路径、其他驱动、符号和内存描述符不会退休。
2. 图片头与解码缓存使用各自的 source/type 偏移；逐键调用原生 `lv_cache_drop`
   （0x0c8b8c9e），并检查摘链进度。没有 `lv_image_cache_drop(NULL)`，也不逐项
   清掉所有无关资源。持有引用的项只被失效/摘链，最后一次原生 release 才回收载荷。
3. 读取 LVGL 已注册的各显示器屏幕树，包括仍挂在屏幕树中的离屏缓存页面。
   快照最多 1024 个对象，父链深度最多 32；分配/容量失败不执行部分元数据刷新，
   请求保持待处理。快照不跨 UI tick 存活，每次原生操作后重新查验对象 membership。
4. 精确 image class（不是任意派生动画/canvas class）的文件 source 命中规则时，
   先用原生 get_info 取新头；**头缓存必须开启且成功查询**，才传回相同 source 指针
   调用 `lv_image_set_src`（0x0c3b2c28）。该固件先查询头再比较 source 指针，因此
   即使指针不变也会更新宽高、尺寸及失效，不需要释放/复制控件 source 字符串。
   查询失败、缓存关闭或查询期间 owner 改了 source，则跳过 setter、保留现有属性。
5. 对非 image 对象也查询主 part、当前状态的图片属性 40。命中规则时调用明确的
   style refresh（0x0c38525c），而不是重赋相同样式指针。图片按钮使用的
   0/32/128 状态由原生样式 getter 选择，模块不改变状态/selector。
   非当前状态会在其正常切换/绘制时采用已退休的文件 key；其他 part/属性不在本适配器内。
6. 最后请求整屏失效（0x0c382428），由固件正常刷新定时器绘制，不强制同步刷新。
   只有固件保留了覆盖整屏的脏区，才增加 `redraws`。

### 安全与覆盖限制

- 必须在串行化的 UI owner 上运行，不能并发改变对象树、缓存或映射。事务期间不要
  修改主题文件。与正常原生 setter 一样，其事件回调不得删除 setter 的接收对象或
  在调用期间使 source/头缓存失效；模块不是通用的恶意回调隔离或资源替换回滚机制。
- `rendering_in_progress` 为假**不是 GPU 空闲证明**。已追踪的延迟 VG_LITE 分支
  会把 decoder descriptor 存入 pending 队列，直到原生 flush/finish 清理；模块
  不手动关闭这些 decoder，也不释放其 payload。引用退休保证不替代硬件验收。
- busy/无活动屏/禁止失效/缓存不匹配/快照失败时，临时 timer 保留请求并重试；
  一轮已完成的退休和元数据阶段不会因最后脏区被拒绝而重复。
  持续超限或布局不匹配会持续待处理，而不是虚报整屏刷新成功。
- 动画派生类、callback 帧、私有 buffer、canvas/snapshot、字体 live/idle 缓存、
  打包资源和未注册到屏幕树的对象不保证采用新资源。没有全局动画 stop，
  没有 watchface loader Hook，没有强制页面重建兜底。
- `images_dropped` 只统计定向退休**轮次**（即使没有匹配项）；`redraws` 只统计
  脏区接受，不证明新尺寸/内容实际显示或所有 owner 已刷新。v5 未提供逐对象结果。

### .139 的目标专属适配

`.139` 的旧直接-key/全局 cache 路径仍停用；已按其精确 AP 单独恢复有界定向适配，
逻辑与 `.155` 一致，但使用 `.139` 自己的 API 地址和 class/layout guards：image
object class `0x2ca14cb8`、decoded/header cache classes `0x2ca168c4/0x2ca16944`，
逐键 drop `0x0c8b8cae`。只处理命中当前映射的绝对文件 key；精确 image class 的尺寸
更新、当前 main-part 图片 property 40 样式刷新及异步整屏失效均按上文流程进行。
所有地址与类均由 `.139` AP 独立确认，没有将 `.155` 的 class/layout 常量平移使用。

上述指令证据本身不是实机验收。正常字体重载的最新用户反馈另见下节；全 UI 重启、
动画/canvas/private buffer owners 等仍不支持。

### 字体热重载（默认启用）

字体走原生 access()/FreeType，不经过图片路径的 LVGL POSIX Hook。三个目标默认
在串行 UI 定时器中执行受检查的字体事务：保存原厂路径基线，验证绘制边界及引用，
准备全部替代字体，再一次性切换注册路径、活跃 backing 和已有 wrapper；保留 fallback、
用户数据及链表身份，退休受影响的 idle 字体，并刷新受支持的普通文字/矢量文字 owner。
准备失败不发布新字体；busy 保留请求重试；提交后的刷新失败会锁定字体适配器，不能
虚报回滚或重载成功。`fonts_retargeted` 统计已提交的字体 family 数量。

用户反馈 `.139`、`.155`、`10 Pro .043` 正常字体重载均已通过实机验收。文件必须使用
新的不可变字体代次，不能原位覆盖或再次复用已离开的非原厂路径；移除映射可恢复
原厂基线。自有字体副本、canvas、未知 backend、GPU 故障恢复和框架重启不在支持范围。
完整边界、错误码与历史设备记录见[字体重载说明](FONT_RELOAD.md)。

当前证据与测试说明见两个目标的 `ui-reload-audit.md`，以及
`targets/xiaomi-band-11-4.100.139/evidence/EVID-RESOURCE-4139-007.json`。
`.139` 探针必须提供与目标 fingerprint 匹配的 AP 文件，例如：

```sh
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.139 \
RESOURCE_HOOK_FIRMWARE=build/firmware-analysis/vela_ap_4.100.139.bin \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py
```

旧 page/restart 固件测试保留为生命周期反例，不代表激活路径仍会重建页面或重启
miwear。

当前 Manager 提供按 [Interconnect 协议](interconnect_proto.md)接收 CRPack v1 资源包的页面；`corona.json` 描述元数据和映射（旧输入与已安装主题兼容 `canora.json`，两者共存时拒绝）；新导出统一使用 `corona.json`，接收时派生每包 `mappings.tsv` 并记录资源文件清单。资源管理页按顶部优先排序，包含可拖动的“系统样式”分界；分界上方参与覆盖，下方不生效。每次放下后，完整顺序保存到 `resource-order.json`；新接收包插入顶部。混搭微调页按实际源文件路径展示所有已安装包的注册项，可逐文件选择默认、系统原资源或指定包，保存在 `resource-overrides.json`；删除所选包时该项回退默认。排序和选择本身不修改模块活动配置，首页重载时先按资源顺序生成常规结果，再应用逐文件选择，最后生成 `mappings.tsv` 并通过 `control.request` 发送 reload v2 请求。跨包重叠源路径按包顺序静态合并，低优先级资源填补高层缺项；同一包内重叠源规则仍由模块最长前缀决定。生成失败不发送信号。Manager 首页状态卡片显示模块实际生效规则数及配置状态，不展示开发者 RHQ1 计数器；设置图标与实验字体测试 UI 已移除。固件侧字体替换/重载已有[用户报告的实机通过](../targets/xiaomi-band-11-4.100.155/font-reload-device-report.md)，但未覆盖 GPU 故障恢复或框架重启。
**兼容性：Manager 与支持 reload v2 的模块需配套更新。** 旧 `reload.request` / `reload.result` 文件不再读取。Manager 在同一应用会话首次进入首页时发送一次状态查询，返回首页复用结果；重载从 RHRS2 回执更新状态和生效规则数，不增加周期或重载后状态查询。传输和解析失败不等于配置错误；“未检测到模块”仅指没有收到有效的对应响应，不证明模块已卸载。协议见 [MODULE_CONTROL.md](MODULE_CONTROL.md)。

下文的 query 是开发者描述符接口，不是现有 UI 能直接看到的统计页面。
不能按“读取计数器”当作普通用户的操作步骤。

配置文件或父目录不存在（ENOENT），以及配置为空/仅含注释时，激活成功并安装零规则 pass-through hook 与轮询器：资源仍使用原始 driver，不重定向、不刷新 UI；之后写入有效映射并更新 reload 信号即可加载主题。超长、非法内容、权限或 I/O 错误、配置内存分配失败同样降级为零规则启动，真实配置错误记录在启动日志和内存状态中，不改写配置，不因配置问题阻断 Canopus 启动其他模块。可由兼容控制客户端修复配置并重载；driver/slot 安全校验与定时器创建失败仍会返回激活错误。运行后热更新也允许 0 条规则以清除映射。替代文件不存在/无法打开时退回原资源。
成功打开但格式错误的资源**不会**自动退回；打开成功不等于解码成功。

## 先在电脑上检查配置

在模块源码目录执行（只检查本地文件，不连接设备）：

```sh
sh scripts/build.sh xiaomi-band-11-4.100.155
./build/check-config examples/mappings.tsv /resource/icons/a.bin
```

预期输出包含：

```text
Valid configuration: 1 rule(s)
Mapped path: /data/quickapp/files/ng.lst.corona/themes/current/icons/a.bin
```

工具直接使用设备模块的 C 解析器和映射函数，不是另一份近似校验逻辑。
它不检查设备上文件是否存在，也不检查图片或字体格式。

例如要映射整个 `/resource/icons/` 目录，在电脑上生成真实 TAB 的配置：

```sh
printf '/resource/icons/\tthemes/my-theme/icons/\n' > mappings.tsv
./build/check-config mappings.tsv /resource/icons/a.bin
```

然后用已有、已授权的设备文件传输工具创建父目录并分别传输：

```text
本地 mappings.tsv       -> /data/quickapp/files/ng.lst.corona/mappings.tsv
本地兼容格式的 a.bin    -> /data/quickapp/files/ng.lst.corona/themes/my-theme/icons/a.bin
```

这里的 `a.bin` 是路径示例，不是包内附带的图片。原始资源路径和格式必须先
确认；没有现成资源的情况下，单独安装 Hook 不会产生任何主题效果。

## 配置规则

- 每行 `源路径<TAB>目标路径`，必须使用真实制表符；没有 `\\t` 转义语法。
- LF 或 CRLF；允许末行无换行；空行以及首字节 `#` 的注释行被忽略。
- 最多 256 条；文件最多 32768 字节（32 KiB）；每个路径最多 255 字节（不含 NUL）。
- 源路径必须为绝对路径，普通目标必须为快应用 files 根目录下的 `themes/...` 相对路径；目录映射的源和目标都以 `/` 结尾；文件映射的源和目标都不以 `/` 结尾，并且只匹配完整路径。精确文件规则的目标也可写为 `@system`，表示保留固件原资源，并屏蔽覆盖该文件的更宽目录规则；目录规则不能使用该保留目标。
- 模块将普通目标补全到所选设备的原生 `themes/` 子树，旧绝对目标直接拒绝；长度限制包含补全的根目录。拒绝 `.`、`..`、重复分隔符，
  反斜杠、冒号、ASCII 控制字符和 DEL。重复源前缀也会拒绝整份配置。
- 最长匹配优先：目录规则按最长前缀匹配，精确文件规则只匹配完整路径；映射结果超过 255 字节时保持原始路径；不递归映射。
- 只重定向 LVGL POSIX driver 的读模式（mode=2），不改变写入/读写操作。
- 路径约束是**词法约束**，不是文件系统沙箱；不要在主题树下放置指向其他
  位置的符号链接。配置、主题文件及其父目录只应允许可信主体修改。

首次激活读取 `/data/quickapp/files/ng.lst.corona/mappings.tsv`；缺配置、空配置、仅注释配置或配置读取/校验/分配失败会以零规则安装透明 hook，并启动轮询，资源仍透传到原始 driver。若安装 hook 前曾成功 prepare，后续失败的 prepare 保留这份有效快照。有效规则配置同样安装 hook；激活后每秒由 UI-owner timer 检查
`/data/quickapp/files/ng.lst.corona/control.request`。兼容快应用控制客户端使用 `internal://files/`；Vela 按
当前 app ID 和设备固件将其映射到对应 native 文件根（11：`/data/quickapp/files/ng.lst.corona/`；10 Pro：`/data/files/ng.lst.corona/`）。只有新的重载信号才读取同目录
`mappings.tsv`；重载格式仍为 `resource-hook-reload-v1<TAB>[ng.lst.corona<TAB>]<revision><LF>`。`resource-hook-status-v1` 请求只读取内存，不读取/校验配置、不刷新资源；回复共用预创建的 `control.response`，完整协议见 [MODULE_CONTROL.md](MODULE_CONTROL.md)。
新配置必须完整读取并通过校验；运行时允许 0 条规则以清除全部映射。零规则配置会保持透明 hook 和轮询器常驻。读取失败或非法配置会保留 last-known-good，并在后续轮询重试。更新原子发布紧凑索引快照，并在定向刷新中同时
考虑旧、新规则，因此删除映射也会刷新回原始资源。快照按实际 TSV 字节数和有效规则数
分配；刷新按需保留旧快照引用，引用释放后回收，不为第二份最大容量规则表永久预留额外
32 KiB Umem。读取和解析期间仍需临时缓冲；常驻及峰值内存取决于实际配置、规则数和仍被
引用的快照，配置上限保持 32 KiB。分配失败会保留当前规则并在后续轮询重试。
三个目标的默认构建均包含字体热重载事务，无需额外开关；其受支持的 owner 和安全边界见[字体重载说明](FONT_RELOAD.md)，不代表任意 UI/自定义字体均可热重载。

Manager 的 Interconnect 页面接收解包后的 CRPack v1 文件树，要求唯一根目录 `corona.json`（或旧输入 `canora.json`，不得同时存在），并将元数据和资源逐个写入原相对路径；接收时验证 manifest 映射目标确实存在于包文件清单，保存每包文件清单并生成派生 `mappings.tsv`。资源管理页保留现有列表样式，长按即可调序；新增包置顶。“系统样式”是分界项，默认在底部；分界上方参与生成，下方保留但不生效。每次放下顺序写入 app-scoped `resource-order.json`，排序不立即切换当前主题。混搭微调按实际文件路径列出所有已安装包注册的资源，可选择默认、系统原资源或指定包，写入 `resource-overrides.json`；所选包被删除时回退默认。逐文件选择序列化为精确映射，系统选项使用模块的 `@system` 透传规则，无需为混搭选择复制资源；普通排序产生目录重叠时也优先生成直接文件映射，超出配置预算才按优先级构建静态叠加。首页重载时先从分界上方的 `corona.json` 按顺序生成常规结果，再应用逐文件选择，最后生成活动 `mappings.tsv`：跨包重叠的源路径按顶部优先解析，预算内直接指向包文件、超预算才静态复制到不可变活动代次；高层只覆盖其实际提供的文件，因此缺项可以回退到下一层；同一包内重叠源规则仍遵循模块最长前缀匹配。超过模块 256 条规则、32 KiB 配置限制或生成失败时不发送信号。配置写入后，Manager 等待与本次请求版本匹配的模块回执，再提示结果；成功确认后清理旧活动代次，并释放旧包保护；可能仍被模块使用的新旧直接映射包会先记录到已有代次索引，信号/回执失败不允许删除或替换这些包。相同包快照、排序和混搭选择复用最近一次成功发布的配置，不重复扫描/复制/写入 TSV，但仍发送新版本信号；接收和删除包使应用会话缓存失效，应用重启重新加载，不检测外部文件修改。复制仅依赖原生完成回调，不再逐文件检查大小/类型。接收端使用 `internal://files/` 访问 app-scoped 文件区。字体重载是默认模块能力，不附带设置图标或字体测试资产/操作；字体资源包或其他受信任工具仍须写入不可变字体代次、映射和重载信号。模块侧使用 native 绝对路径；快应用文件 API 应使用 `internal://files/`，不要通过 `system.file` 传 native 绝对路径（QJS 会按快应用规则改写）。
stop/deactivate 仍需完整重启卸载，不支持热卸载。

## 状态与错误

文件通道状态返回 RHST1：正常为 `running`，真实配置错误为 `config_error`；缺失/空/仅注释的启动配置健康。配置错误保持到成功的显式配置发布，后台快应用解析错误独立恢复。刷新待完成时仍可查询，超时只能表示无响应，不能证明模块已停止。详见 [MODULE_CONTROL.md](MODULE_CONTROL.md)。

描述符 query 默认返回 RHQ1 v6，共 48 字节，小端序的十二个 uint32；前 40 字节保持历史 v5 偏移兼容：

| 偏移 | 字段 |
|---|---|
| 0 | `RHQ1` magic（0x31514852） |
| 4 | 状态格式版本，6 |
| 8 | installed，0/1 |
| 12 | rule_count |
| 16 | redirected，替代资源打开成功次数 |
| 20 | fallback，替代资源打开失败后的回退次数 |
| 24 | images_dropped，成功完成定向图片退休的轮次（.139/.155 均支持目标专属适配） |
| 28 | redraws，固件接受整屏脏区的次数（非同步刷新） |
| 32 | rebuilds，保留字段，当前实现始终为 0 |
| 36 | fonts_retargeted，已提交字体 family 数（饱和累计） |
| 40 | font_result，有符号结果：0 完成、1 busy/pending、负值失败 |
| 44 | font_pending，字体阶段待处理时为 1 |

计数器到 UINT32_MAX 后饱和；未命中规则和非法路径不计入 fallback。
query 需要至少 48 字节可写剩余空间，成功后发布 writer；40 字节的旧缓冲区会被拒绝，不发生部分写入。状态格式版本
从 1 升到 2 时新增偏移 24 的 `images_dropped`，从 2 升到 3 时新增偏移 28 的
`redraws`，从 3 升到 4 时新增偏移 32 的 `rebuilds`，从 4 升到 5 时新增偏移 36 的
`fonts_retargeted`，从 5 升到 6 时新增偏移 40/44 的字体结果与待处理状态。

| 返回值 | 含义 |
|---|---|
| -2004 | 已安装，不能重新 prepare |
| -2005 | `config.fallback` 日志及 RHST1 configError：配置文件打开失败（不含 ENOENT）；不作为启动返回值 |
| -2006 | `config.fallback` 日志及 RHST1 configError：配置读取/快照展开内存不可用；不作为启动返回值 |
| -2007 | `config.fallback` 日志及 RHST1 configError：配置读取/解析/校验失败；不作为启动返回值 |
| -2008 | driver 标识/布局不符；即使零规则也必须通过此安全检查 |
| -2009 | 原始 callback slot 未知，不覆盖 |
| -2010 | 重绑定状态不一致 |
| -2011 | 刷新定时器分配失败（重定向已驻留，可再次激活重试） |
| -2012 | 轮询定时器分配失败（hook 已驻留，可再次激活重试） |

stop/deactivate 在安装后返回 SDK 的 `CANOPUS_RESULT_REBOOT_REQUIRED`。
这表示**尚未停止**，并非已经拆除 Hook。installed 表示模块曾成功发布回调，
不是第三方没有改动 slot 的持续健康检查。

## 回退与升级

- 通过 Manager 禁用模块，再按 Manager 的 reboot-required 流程执行完整
  **设备重启**。禁用不应被视作立即恢复已缓存资源或已经卸载回调。
- 若 UI 异常，使用预先验证的 Supervisor safe-mode/设备恢复路径；不要继续
  用损坏主题尝试重启 miwear。没有可用恢复手段时，不进行首次实机安装。
- `scripts/restart_miwear.sh` 故意退出 78；不要替换成 kill + sleep + start。
- 旧模块运行时 ID（如 `manager_resource_hook` 或 `resource_hook`）与新 ID `corona` 不同。
  先禁用旧模块并完整重启，确认它不再恢复后再安装新模块；不能同时启用两者。
- 项目/Manager 包标识与 registration、receipt、registry 的运行时模块 ID 统一为 `corona`（包名为 `ng.lst.corona`）。
- semver 为 0.3.0，CMI1 模块整数版本为 3，CMI1 格式版本仍为 1。
  当前 Supervisor 不保证防降级，升级策略应由发布/安装流程另行控制。

## 实机验收门禁

以下必须在目标设备上逐项留下记录后，才可称为设备可用版本：

- exact firmware 与签名 identity guard；真实 ELF load 和 /dev/canopus 注册。
- 真实 install、enable、query、disable 和完整设备 reboot/recovery。
- UI 所有者上下文，以及 Hook 安装与首次资源读取的先后顺序。
- 正常/缺失/错误格式的主题资源；字体及图片分别进行显示验证。
- 旧缓存行为、内存余量、watchdog 与安全模式恢复。

自动 miwear 重启、强制同步整屏刷新、全局动画 stop、watchface Hook、强制页面重建
以及任意图片/字体格式兼容保证均不属于当前实现。需验证新旧尺寸、错误头信息、持有 decoder
的最终 release、动画播放意图、离屏页面采用、状态样式、重复激活与内存压力。
宿主测试与 Unicorn 指令测试不能替代上述实机验收。
