# Resource Hook 安装与验收

适用交付：Canopus-Module-Resource-Hook **0.3.0**，仅限 Xiaomi Band 11
**4.100.139 / 4.100.155**，两个固件使用独立构建的 ELF 和收据，不能混用。
这是 ELF + CMI1 开发集成交付，不是 canonical `.canopus` 压缩包，也不是已通过
实机验收的完整主题产品。`.155` 有下述**用户报告的单项成功**；`.139` 仍为
**NOT_PROBED**，不能跨目标或跨资源 owner 推广结论。

## .155 用户报告的单项实机记录

用户确认：在 **4.100.155** 上，通过本地临时安装器
`~/develop/temp/settings-icon-installer-155`，使用当前签名模块，完成安装和主题路径
配置后，自定义**设置启动器图标实际可见**。该流程包含 execute 恢复路径与 `mkdir`
目录创建；这是历史测试流程记录，不是通用安装器已支持这些操作的承诺。
本仓库不整合该临时安装器；后续资源传输使用其他方式。
本次原始资源路径为 `/resource/app/settings/launcher.bin`，替代文件为
`/data/canopus/themes/current/app/settings/launcher.bin`（112×112 I8）。映射配置入口为
`/data/canopus/themes/mappings.tsv`；不要把临时安装器的本地目录当作设备主题路径。
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
不会从不可信收据自动猜测目标；`.155` 必须显式指定。固件升级前先禁用旧模块并完整
重启，再按新固件重新部署匹配的 Supervisor 和模块，不要只给旧 ELF 换收据。

`signer-public.pem` 是供比对的公钥，不是信任根。单独使用包内公钥只能检查
自洽性，不能确认来源。SHA256SUMS 检查传输损坏；ELF 的真实性由 CMI1
签名验证。说明文件和配置样例没有独立签名。私钥不在交付包中。

## 配置和启用

需要提取或编辑图片时，先阅读[资源图片工具说明](../tools/RESOURCE_IMAGE.md)：
工具仅支持受限 LVGL v9 I8 格式，先做 PNG 预览，再传输 BIN；它不负责模块安装或激活。

1. 备份已有 `/data/canopus/themes/mappings.tsv` 和原主题文件。
2. 在 `/data/canopus/themes/current/` 下上传兼容固件的资源文件。
   例如 `/resource/icons/a.bin` 对应
   `/data/canopus/themes/current/icons/a.bin`。
3. 检查 `mappings.tsv.example`，按实际资源源路径修改，然后上传为
   `/data/canopus/themes/mappings.tsv`。样例里的 `/resource/` 只是示例规则，
   不意味着固件的所有字体和图片都经过这个目录。
4. 使用兼容 Manager 的已验签安装流程导入 ELF 和 receipt；对应的 inbox
   文件名为 `resource_hook.ko` 和 `resource_hook.cmi`，目录是
   `/data/canopus/inbox/`。**仅复制文件不等于完成安装**，还必须执行 Manager
   的 install 操作。不要直接编辑 registry.bin，也不要直接调用 ELF。
5. 在 Manager 的模块列表打开 `resource_hook` 详情，点击“启用”并确认。
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

### .139 的保守行为

`.139` 保留读路径重定向、字体注册表重定向和整屏失效请求，但其旧直接-key
缓存遍历未被独立验证，现已停用。不调用 .155 专用图片/样式适配器，不猜测对应
地址/布局。`images_dropped` 保持 0；旧缓存/旧尺寸可能继续可见，需未来独立适配器
或 owner 自己的正常生命周期。不能把 `.155` 的重载测试当作 `.139` 的证据。

### 字体注册路径（不是即时字体重载）

字体走原生 access()/FreeType，不经过本模块 LVGL POSIX Hook。模块对命中映射的
已注册字体名执行 remove/add，再解析确认路径才计入 `fonts_retargeted`。
直接 add 不能覆盖首次同名匹配；remove 前会复制将被释放的名字。

此操作只影响**未来实际走注册路径解析**的请求。活跃 wrapper 和 idle 缓存复用
可以绕过它；模块不释放旧 face、不重建页面，不能保证字体立即变化。remove/add
不是事务：add 分配失败可能丢失注册条目并回落默认路径。也不验证主题字体格式；
打开/解析失败不保证自动恢复旧 face。

当前证据与测试说明见 `targets/xiaomi-band-11-4.100.155/ui-reload-audit.md`
和 `tests/firmware_reload.py`。旧 page/restart 固件测试保留为生命周期反例，
不是激活路径仍会重建页面或重启 miwear 的声明。

当前 Manager 没有主题选择器、资源上传页面，也没有展示模块 RHQ1 计数器
（“立即激活”只负责加载激活模块，不负责传输主题文件）。
下文的 query 是开发者描述符接口，不是现有 UI 能直接看到的统计页面。
不能按“读取计数器”当作普通用户的操作步骤。

首次启用前配置文件必须存在且至少包含一条规则。缺失、超长或非法配置使
激活失败，不修改原始 driver。替代文件不存在/无法打开时退回原资源。
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
Mapped path: /data/canopus/themes/current/icons/a.bin
```

工具直接使用设备模块的 C 解析器和映射函数，不是另一份近似校验逻辑。
它不检查设备上文件是否存在，也不检查图片或字体格式。

例如要映射整个 `/resource/icons/` 目录，在电脑上生成真实 TAB 的配置：

```sh
printf '/resource/icons/\t/data/canopus/themes/my-theme/icons/\n' > mappings.tsv
./build/check-config mappings.tsv /resource/icons/a.bin
```

然后用已有、已授权的设备文件传输工具创建父目录并分别传输：

```text
本地 mappings.tsv       -> /data/canopus/themes/mappings.tsv
本地兼容格式的 a.bin    -> /data/canopus/themes/my-theme/icons/a.bin
```

这里的 `a.bin` 是路径示例，不是包内附带的图片。原始资源路径和格式必须先
确认；没有现成资源的情况下，单独安装 Hook 不会产生任何主题效果。

## 配置规则

- 每行 `源目录<TAB>目标目录`，必须使用真实制表符；没有 `\\t` 转义语法。
- LF 或 CRLF；允许末行无换行；空行以及首字节 `#` 的注释行被忽略。
- 最多 64 条；文件最多 32768 字节；每个路径最多 255 字节（不含 NUL）。
- 源和目标必须是绝对目录前缀，末尾 `/` 不可省略。
- 目标必须位于 `/data/canopus/themes/`；拒绝 `.`、`..`、重复分隔符、
  反斜杠、冒号、ASCII 控制字符和 DEL。重复源前缀也会拒绝整份配置。
- 最长源前缀优先；拼接结果超过 255 字节时保持原始路径；不递归映射。
- 只重定向 LVGL POSIX driver 的读模式（mode=2），不改变写入/读写操作。
- 路径约束是**词法约束**，不是文件系统沙箱；不要在主题树下放置指向其他
  位置的符号链接。配置、主题文件及其父目录只应允许可信主体修改。

首次激活后规则锁定在驻留模块内。修改磁盘文件不会立即生效；同一 resident
image 的再次激活只重绑回调，不重新读取配置。更换规则需要安全禁用并完整
设备重启后重新加载；不支持热卸载。

## 状态与错误

query 返回 40 字节，小端序的十个 uint32：

| 偏移 | 字段 |
|---|---|
| 0 | `RHQ1` magic（0x31514852） |
| 4 | 状态格式版本，5 |
| 8 | installed，0/1 |
| 12 | rule_count |
| 16 | redirected，替代资源打开成功次数 |
| 20 | fallback，替代资源打开失败后的回退次数 |
| 24 | images_dropped，成功完成定向图片退休的轮次（.139 为 0） |
| 28 | redraws，固件接受整屏脏区的次数（非同步刷新） |
| 32 | rebuilds，保留字段，当前实现始终为 0 |
| 36 | fonts_retargeted，成功改写字体注册路径的条目数 |

计数器到 UINT32_MAX 后饱和；未命中规则和非法路径不计入 fallback。
query 需要至少 40 字节可写剩余空间，成功后发布 writer。状态格式版本
从 1 升到 2 时新增偏移 24 的 `images_dropped`，从 2 升到 3 时新增偏移 28 的
`redraws`，从 3 升到 4 时新增偏移 32 的 `rebuilds`，从 4 升到 5 时新增偏移 36 的
`fonts_retargeted`。

| 返回值 | 含义 |
|---|---|
| -2004 | 已安装，不能重新 prepare |
| -2005 | 配置文件打开失败 |
| -2006 | 临时内存不可用 |
| -2007 | 配置读取/解析/校验失败 |
| -2008 | 空规则或 driver 标识/布局不符 |
| -2009 | 原始 callback slot 未知，不覆盖 |
| -2010 | 重绑定状态不一致 |
| -2011 | 刷新定时器分配失败（重定向已驻留，可再次激活重试） |

stop/deactivate 在安装后返回 SDK 的 `CANOPUS_RESULT_REBOOT_REQUIRED`。
这表示**尚未停止**，并非已经拆除 Hook。installed 表示模块曾成功发布回调，
不是第三方没有改动 slot 的持续健康检查。

## 回退与升级

- 通过 Manager 禁用模块，再按 Manager 的 reboot-required 流程执行完整
  **设备重启**。禁用不应被视作立即恢复已缓存资源或已经卸载回调。
- 若 UI 异常，使用预先验证的 Supervisor safe-mode/设备恢复路径；不要继续
  用损坏主题尝试重启 miwear。没有可用恢复手段时，不进行首次实机安装。
- `scripts/restart_miwear.sh` 故意退出 78；不要替换成 kill + sleep + start。
- 旧模块运行时 ID `manager_resource_hook` 与新 ID `resource_hook` 不同。
  先禁用旧模块并完整重启，确认它不再恢复后再安装新模块；不能同时启用两者。
- 项目 ID 是 `org.canopus.resource-hook`（manifest 的 reverse-DNS 约束）；
  registration、receipt、registry 使用 `resource_hook`。
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
以及主题字体格式检查均不属于当前实现。需验证新旧尺寸、错误头信息、持有 decoder
的最终 release、动画播放意图、离屏页面采用、状态样式、重复激活与内存压力。
宿主测试与 Unicorn 指令测试不能替代上述实机验收。
