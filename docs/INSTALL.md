# Resource Hook 安装与验收

适用交付：Canopus-Module-Resource-Hook **0.3.0**，仅限 Xiaomi Band 11
**4.100.139**。这是 ELF + CMI1 开发集成交付，不是 canonical `.canopus`
压缩包，也不是已通过实机验收的完整主题产品。实机状态：**NOT_PROBED**。

## 安装前提

- 固件 SHA-256 必须为
  `31ce82257f7c127950dc5070b86316730cf468a41f0d004559e41e7d923b2c74`。
- 已部署兼容 Canopus ABI **1.2**、CMI1 的 Supervisor；其可信签名公钥
  必须与本 payload 的签名者一致。不要为安装此模块关闭签名或固件校验。
- 操作方必须具备文件传输、Manager 控制及故障后的设备重启/恢复手段。
- 模块激活必须由已串行化的 UI 所有者流程执行。短临界区只保护回调发布，
  不提供 UI teardown、在途调用排空或缓存重建协议。

先用**独立可信来源**的 Supervisor 签名公钥验证：

```sh
python3 verify-payload.py . --public-key /path/to/trusted-supervisor-public.pem
shasum -a 256 -c SHA256SUMS
```

`signer-public.pem` 是供比对的公钥，不是信任根。单独使用包内公钥只能检查
自洽性，不能确认来源。SHA256SUMS 检查传输损坏；ELF 的真实性由 CMI1
签名验证。说明文件和配置样例没有独立签名。私钥不在交付包中。

## 配置和启用

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
   激活模块，安装重定向、退休图片缓存并请求一次整屏刷新。安全模式、未签名
   模块和仅重启类模块不提供该入口；已常驻的模块也不再显示（不能重复激活）。
   若不使用“立即激活”，则按第 7 步重启。
7. 若走重启路径：按框架的已验证启动流程完整重启设备，重新载入 Canopus
   Supervisor，然后进入 Manager，让 UI 上下文执行启用模块的恢复。不能只重启
   设备就假定框架会自动加载；当前模块不提供系统级开机引导。
8. 查看详情是否为“启动时常驻”，且没有激活错误。只有之后发生的、经过
   LVGL POSIX driver 的新 open 才会被映射。

## 图片重载（已实现）与字体（未实现）

**图片：** 每次 activate 成功安装重定向后，模块会在 UI 所属线程**逐项退休**
图片缓存和图片头缓存（`lv_cache_drop`，.139 地址 0x0c8b8cae）。LVGL 图片缓存以
“源路径”为键，若不退休，已解码过的 `/resource/...` 图片会在 open 之前直接命中
旧缓存，重定向永远不生效。退休后，模块紧接着调用 `_lv_inv_area`（0x0c382428）
把整块活动屏标记为脏，让固件自身的刷新定时器在下一次 tick 重绘全屏、经过重定向
重新解码。要点：
- **逐项退休，不是整表清空。** 仍被持有的缓存项只被标记失效并摘链，载荷等最后
  一个引用释放时（`lv_cache_entry_release_data`，0x0c8b9790）才回收，**不会把
  数据从正在使用它的控件下面抽走**；引用计数为 0 的项立即释放。无论哪种情况，
  下一次按该路径查找都会落空，从而走重定向。
  固件自带的 `drop_all` 会连带释放被持有项的节点存储，因此**不**使用它。
- 刷新请求是**一次性且会合并的**。UI 忙碌（正在渲染 `rendering_in_progress`、
  失效被禁用 `lv_display_is_invalidation_enabled` 为 0、尚无默认显示器或活动屏）
  时请求不会被丢弃：模块创建一个临时 LVGL 定时器重试，成功后立刻删除自己。
  这**不是**周期性反复刷新。
- 只有确认固件真的接受了覆盖整屏的脏区，才算一次成功并计数。
- 模块只把整屏标记为脏（`_lv_inv_area` 传入超大区域，由固件裁剪到屏幕大小），
  实际重绘仍由固件刷新定时器完成——**不强制同步刷新**。
- 活动屏指针为 `disp+696`（由真实 accessor `lv_display_get_screen_active`
  0x0c3807ec 验证）。`disp+24` 是 DPI，非活动屏，不能用来判断。
- 若图片缓存尚未创建，或缓存类不是已恢复的 LRU/RB 类，退休被安全跳过。
- query 的 `images_dropped` 每成功退休一次加一，`redraws` 每成功请求一次整屏
  失效加一。
- 图片主路径已追踪确认：`lv_draw_image` 每帧调用 `lv_image_decoder_get_info` /
  `lv_image_decoder_open`，绘制完即 `lv_image_decoder_close`，**不跨帧持有解码器**；
  未命中缓存时走 `lv_fs_open`，正好经过本模块的重定向。
- **未追踪**：`lv_draw_image` 的延迟绘制任务分支，以及自带解码缓冲的动画/画布类
  控件。这些可能仍显示旧图，需页面重建。

**字体：** 模块**不会**自动重载字体。字体 wrapper 由页面对象按引用计数持有，
盲目释放会破坏正在使用它的页面；安全重载字体需要页面重建生命周期，尚未实现。
因此已加载的字体对象不会被 activate 替换。见
`targets/xiaomi-band-11-4.100.139/lifecycle-recovery.json` 与 `ui-reload-audit.md`。

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
sh scripts/build.sh
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

query 返回 32 字节，小端序的八个 uint32：

| 偏移 | 字段 |
|---|---|
| 0 | `RHQ1` magic（0x31514852） |
| 4 | 状态格式版本，3 |
| 8 | installed，0/1 |
| 12 | rule_count |
| 16 | redirected，替代资源打开成功次数 |
| 20 | fallback，替代资源打开失败后的回退次数 |
| 24 | images_dropped，成功清空图片缓存的次数 |
| 28 | redraws，成功请求整屏失效（强制重绘）的次数 |

计数器到 UINT32_MAX 后饱和；未命中规则和非法路径不计入 fallback。
query 需要至少 32 字节可写剩余空间，成功后发布 writer。状态格式版本
从 1 升到 2 时新增偏移 24 的 `images_dropped`，从 2 升到 3 时新增偏移 28 的
`redraws`。

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

自动 miwear 重启、原生字体路径全覆盖、字体重载、强制同步整屏刷新，以及重建
仍被实时引用的资源，均不属于当前实现（模块会请求整屏失效，但实际重绘仍交给
固件刷新定时器）。宿主测试与 Unicorn 固件指令测试不能替代上述实机验收。
