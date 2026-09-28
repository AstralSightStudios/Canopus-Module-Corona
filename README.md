# Canopus-Module-Resource-Hook

**0.3.0 · Xiaomi Band 11 / 4.100.139、4.100.155 · 资源路径重定向**

两个目标使用独立 ELF、地址配置和签名收据，不能混用；`.139` 仍是默认构建目标。
**.155 已有用户报告的实机成功：签名模块安装、自定义设置启动器图标，以及实验版字体替换/重载。**
字体测试的产物哈希、修复经过和验证范围见 [字体实机记录](targets/xiaomi-band-11-4.100.155/font-reload-device-report.md)。
这不是全部资源 owner 的硬件验收；`.139` 仍为 **NOT_PROBED**。流程与证据边界见
[实机记录](docs/INSTALL.md#155-用户报告的单项实机记录)。宿主测试和固件指令仿真不替代设备验收。

## 已实现

- 最多 64 条目录前缀或精确文件映射，最长匹配优先，从快应用私有文件根目录
  `/data/quickapp/files/ng.lst.corona/mappings.tsv` 完整校验后提交。路径最多 255 字节，
  配置最多 32 KiB。替代资源限于同根目录的 `themes/` 子树；拒绝路径穿越、重复规则和非法
  前缀，主题树不得含逃逸符号链接。Manager 对应 URI 根目录为 `internal://files/`。
- 只 Hook 所选目标的 `/` LVGL POSIX driver、读模式 2；处理相对 driver 路径和
  `fd + 1` 句柄。替代文件打不开时退回原资源；成功打开但解码失败不自动回退。
- 已知 callback 才允许安装/重绑定；短临界区发布状态。激活后每秒检查
  `/data/quickapp/files/ng.lst.corona/reload.request`，信号变化后读取同目录 `mappings.tsv`；
  完整校验后通过双规则快照切换，错误或部分配置保留 last-known-good。双快照增加
  32 KiB 常驻 Umem；读取变更配置时最多额外申请 32 KiB scratch，内存不足时下轮重试。
  配置缺失或为空时以零规则启动透明 pass-through hook 和轮询器，不重定向资源；之后写入映射并更新信号即可加载主题。
- **.139/.155 定向图片退休与 owner 刷新：** 两个精确目标现在都使用逐键
  `lv_cache_drop` 退休命中映射的文件 key，不做全局 cache drop，也不动其他路径、驱动、
  内存描述符或符号源。持有项仅失效/摘链，payload 由原生最后一次引用释放。遍历前检查
  目标专属类、布局、链路边界与摘链进度。
- 两目标均在已注册屏幕树（含离屏树）收集/重验 owner。精确 image class 的受影响
  文件源先经原生 get-info 验证，再用相同 source 指针调用原生 setter 更新宽高/尺寸失效；
  不复制或替换 source。头缓存关闭、查询失败、对象/源变化时跳过。主 part 当前状态的
  属性 40 文件源命中映射时显式 style refresh，不改变 style 指针、selector 或状态。
  其他 part/属性、动画/canvas 派生 owner 仍不保证刷新。
- 目标地址/类并不混用：`.139` image object class 为 `0x2ca14cb8`，image/header cache
  classes 为 `0x2ca168c4/0x2ca16944`；`.155` 分别为 `0x2ca14ca8`、
  `0x2ca168b4/0x2ca16934`。新用到的四个 owner API 已在两个精确 AP 中逐字节核对；
  `.139` 的 image setter 及 cache/drop 入口仍按自己的固件布局验证。
- UI 请求一次性且可合并：忙碌、缓存未就绪、快照分配失败等情况由临时 LVGL timer
  重试；成功请求整屏脏区后自删。已经完成的退休/元数据阶段不因脏区被拒绝而反复执行。
  定时器分配失败返回 -2011，重定向仍驻留。
- **默认构建**的字体注册路径按映射 remove/add 并重新解析确认；**不是 live/idle 字体替换**。
  不自动清字体缓存，也**不再强制重建栈顶页面**。
- 默认构建的 RHQ1 保持 **v5、40 字节**兼容：`images_dropped` 是完成的定向退休轮次（不是
  图片数量/全局清空次数），`redraws` 是被接受的整屏脏区请求，`rebuilds` 保留为 0。
  计数器不证明所有图片、字体或 GPU 工作已采用新资源。详见 [安装文档](docs/INSTALL.md)。

**.139 新逻辑已按独立固件证据启用：** 旧的全缓存/全局 drop 路径仍禁用；新适配器
仅对已验证的映射 key 和 owner 生效，不套用 `.155` 的对象或 cache class。精确 `.139`
AP 的 18 项 Unicorn 探针通过；这是静态/仿真验证，不是实机验收。完整 UI 重启、live
字体替换及未覆盖 owner 在默认构建中仍不支持。

## .155 实验性字体热重载

新增默认关闭的 `.155` 专用事务适配器：检查式加载与失败回滚、保留现有字体对象地址、
同步 active/idle/fallback 所有权、恢复原始字体路径，以及普通文字/矢量标签刷新。
只在 UI timer 中执行，不主动等待或复位 GPU；必须为新字体提供**不可变的新一代文件路径**。

```sh
RH_EXPERIMENTAL_FONT_RELOAD=1 sh scripts/build.sh xiaomi-band-11-4.100.155
```

独立产物为 `build/resource-hook-font-experimental.elf`，状态为 RHQ1 **v6 / 48 字节**。
仍须通过 Canopus 精确目标包的地址白名单校验；尚未批准新字体接口的目标包会拒绝构建验证。
**不能绕过校验安装，不能把空队列当作 GPU 故障后的安全证明。** GPU 异常、未观察到的框架
重启、自定义文字 owner 仍未验证；当前字体替换/重载已有用户报告的实机通过，不等于完整故障场景验收。标准签名交付脚本仍拒绝此实验选项；独立实验安装表盘构建脚本为 `scripts/build-font-experimental-watchface.py`，不绕过签名或目标校验。
详见 [实验开关、限制与验证说明](docs/FONT_RELOAD_EXPERIMENT.md)。

## 生命周期边界

- 必须在已串行化的 UI owner 上执行；主题文件在事务内保持不变。原生 setter 的
  回调不得在调用中删除其接收对象或使其 source 失效。模块在各次原生操作之间
  重新检查对象是否仍属于屏幕树、class/source 是否仍匹配，不跨 UI tick 保存裸对象。
- 快照上限 1024 对象、父链深度 32；超过限制不做部分元数据刷新，保留请求重试。
- 非渲染标志只是 UI 失效/变更门禁，**不是 GPU idle**。延迟 VG_LITE 分支确实会
  把 decoder 保留到原生 pending 队列清理；模块不手动释放 decoder payload、
  不假定每帧都即时关闭，也不主动调用全局 cache drop 或强制同步刷新。
- 不全局停止动画，不把 stop/update/start 称为无损 resume。动画派生类、回调帧、
  canvas/snapshot、打包资源和离开 LVGL 屏幕树的 owner，仍需各自的安全重载协议。
- **不 Hook watchface loader，不重建页面作为替代方案，不承诺任意 RAM/ROM 资源可替换。**
- 不自动重启 miwear，不保证 Hook 早于首次资源读取；Manager 可接收 CRPack v1 解包后的主题文件树，资源管理页可排序资源包并以“系统样式”划定活动范围，主页重载时生成并应用活动映射；Manager 不展示映射规则，也不包含 ZIP 发送端。
  `restart_miwear.sh` 故意退出 78；stop/deactivate 的 reboot-required 不表示已拆除 Hook。

项目/管理器包标识为 `ng.lst.corona`；运行时/收据标识为 `corona`。
旧 `manager_resource_hook` / `resource_hook` 不会自动迁移：先禁用并完整重启，再安装新模块，不能同时启用。

新适配器的可重复验证：

```sh
# .155 使用 Canopus 的目标 AP
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py

# .139 可从本地 OTA 包提取 AP，再用该精确文件运行探针
unzip -p ../../temp/miwear.watch.q66tc_v4.100.139_full_a02b7af5.bin vela_ap.bin \
  > build/firmware-analysis/vela_ap_4.100.139.bin
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.139 \
RESOURCE_HOOK_FIRMWARE=build/firmware-analysis/vela_ap_4.100.139.bin \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py
```

该测试临时编译所选目标源码，不读取签名密钥，不依赖已签名 payload。两个 AP 上的
cache drop/release、屏幕遍历、image setter 和 style refresh 运行真实固件指令；I/O、
cache lookup/unlink/free、decoder 与 GUI 叶节点按测试说明建模。**未测试真实显示器、GPU
完成或实机资源采用。**

## 编辑图片资源

[资源图片工具说明](tools/RESOURCE_IMAGE.md)介绍只读 ROMFS 提取、受限 LVGL v9 I8
BIN 与透明 PNG 转换、编辑后预览及主题映射。工具不负责签名、安装或激活；
不支持任意图片格式、表盘包或调整尺寸。

## 从源码构建

依赖：相邻 `../Canopus`（不存在时回退 `../Canopus-Private`）源码及其已构建 CLI、
C 编译器、Clang ARM 后端、
`ld.lld`、Python 3.11+、支持 Ed25519 的 OpenSSL。完整交付还需要 Lua 5.3+、
Canopus 的固件测试 Python 环境、目标固件文件及已构建的 Band 11 Supervisor
测试资源。这些固件和工具不随本模块分发。

路径均可覆盖，不依赖 `/Volumes/EXT0`：

| 环境变量 | 默认值 |
|---|---|
| CANOPUS_ROOT | `../Canopus`，不存在时 `../Canopus-Private`（相对于模块目录） |
| CANOPUS_CLI | `$CANOPUS_ROOT/target/debug/canopus` |
| CC / CLANG / LD_LLD | `cc` / `clang` / `ld.lld` |
| MODULE_INSTALL_KEY | `$CANOPUS_ROOT/.canopus-local/module-installer-ed25519.pem` |
| FIRMWARE_PYTHON | `$CANOPUS_ROOT/build/band11-tests/bin/python` |
| CANOPUS_TEST_LUA | `lua` |
| RESOURCE_HOOK_PAYLOAD | 单独运行测试时覆盖 payload 目录 |

在 Canopus 仓库先执行 `cargo build -p canopus-cli`。`.155` 目标包必须允许
本模块使用的**精确**固件接口，包括新增的 image set-src/get-info、object tree-walk、
style get/refresh；地址与证据见该目标 `ui-reload-audit.md` 的当前实现章节。
旧目标包可能报地址不在 allowlist，必须更新精确符号，不能关闭 verifier 或扩大地址范围。

在模块源码目录中：

```sh
# 三组 ASan/UBSan 宿主测试、ARM 编译、ELF verifier；不需要私钥
sh scripts/build.sh xiaomi-band-11-4.100.155
# 不传目标仍构建 .139；build/resource-hook.elf 是最近一次构建的目标

# 完整签名交付：运行全部检查，然后生成 payload、安装表盘资源和 ZIP
# 输出目录必须尚不存在；不会覆盖以前的交付
python3 scripts/build-delivery.py dist/0.3.0-155 --target xiaomi-band-11-4.100.155
```

签名 key 必须已被 Supervisor 信任；完整交付构建会用 **Supervisor 源码中的
可信公钥**再验签。没有匹配的 key 时构建失败，不自动生成替代信任根，不修改
Supervisor 公钥，也不把私钥打包或上传。

输出结构：

```text
dist/0.3.0-155/
├── payload/xiaomi-band-11-4.100.155/
│   ├── resource-hook.elf
│   ├── receipt.bin
│   ├── signer-public.pem
│   ├── verify-payload.py
│   ├── mappings.tsv.example
│   ├── INSTALL.md
│   ├── release.json
│   └── SHA256SUMS
├── watchface/xiaomi-band-11/       # main.lua + 两个 .bin 资源
│   └── build/resource-hook-prod.zip
├── evidence/                     # 固件证据及未解决生命周期边界
├── INSTALL.md
├── validation.json
├── validation.log
├── SHA256SUMS
└── resource-hook-0.3.0-xiaomi-band-11-4.100.155.zip
```

安装表盘 ZIP 是**供表盘打包工具使用的 Lua/资源包**，不是已经封装好的厂商
表盘文件，也不是无需框架即可安装的引导包。只有 matching Supervisor 已就绪
时才可使用。配置和主题文件要单独传输；详见 [安装、回退和实机验收](docs/INSTALL.md)
（交付 ZIP 中是根目录的 `INSTALL.md`）。

## 单独验证

```sh
export RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155
sh scripts/build-install-payload.sh "$RESOURCE_HOOK_TARGET" \
  "build/payload-0.3.0/$RESOURCE_HOOK_TARGET"
python3 tests/test_target_receipts.py  # 不需要生产签名私钥
python3 tests/test_delivery.py
"$FIRMWARE_PYTHON" tests/firmware_paths.py
"$FIRMWARE_PYTHON" tests/firmware_restart.py
"$FIRMWARE_PYTHON" tests/firmware_rebind.py
```

固件测试需要设置 `CANOPUS_ROOT`、`FIRMWARE_PYTHON`，并在框架中准备对应目标的
AP 固件及 stage1/stage2/Supervisor 测试资源；完整加载测试还需要 Supervisor 真正
信任的签名收据。临时测试密钥通过离线校验不等于设备或 Supervisor 信任。

若该 payload 目录已存在，选择新输出路径并设置 `RESOURCE_HOOK_PAYLOAD`，不要
删除不明来源的目录。verify-payload.py 接受独立可信公钥；包内公钥仅供比对，
不能单凭“使用包内公钥验签成功”确认发布来源。

配置样例：[examples/mappings.tsv](examples/mappings.tsv)，其中分隔符是真实 TAB。
目录映射的源/目标都以 `/` 结尾并追加剩余路径；文件映射两端都不以 `/` 结尾，只匹配完整路径。启动配置文件不存在、为空或仅含注释时，模块以零规则启动 pass-through hook 和轮询器，不重定向资源或刷新 UI；之后写入映射并更新信号即可加载主题。模块运行后也可用 0 条规则清除全部映射。非法配置及其他读取错误会保留
last-known-good。控制目录的变更标记格式为
`resource-hook-reload-v1<TAB>[ng.lst.corona<TAB>]<revision><LF>`；变更规则会退休旧、新映射命中的图片缓存并刷新可支持的 owner。默认构建不能替换页面已持有的字体对象。Manager 的 Interconnect 接收端要求 `canora.json`，逐文件保存资源、每包派生 `mappings.tsv` 与资源文件清单。资源管理页按“顶部优先”拖动排序，并提供可拖动的“系统样式”分界：其上资源包参与覆盖，其下资源包保留但不生效；顺序保存到 app 文件区，接收新包时置顶。首页重载按钮按已保存顺序重新生成活动 `mappings.tsv`，再发送版本化信号并等待本次模块回执后提示。跨包重叠的源路径按资源包优先级静态合并，低层文件填补高层缺项；同一包内部重叠规则仍按模块最长前缀语义解析。生成失败时不发送重载信号。Manager 通过 `internal://files/` 访问同一 app-scoped 文件区。固件实验字体热重载不再由 Manager 提供测试 UI。构建需要含已批准 errno veneer
的 Canopus 目标包（`.139` / `.155`）。
完整协议、错误码、版本映射和回退步骤见 [docs/INSTALL.md](docs/INSTALL.md)。
固件证据：[.139 审计](targets/xiaomi-band-11-4.100.139/ui-reload-audit.md)、
[.155 迁移证据](targets/xiaomi-band-11-4.100.155/)。
