# Canopus-Module-Resource-Hook

**0.3.0 · Xiaomi Band 11 / 4.100.139、4.100.155 · 资源路径重定向**

两个目标使用独立 ELF、地址配置和签名收据，不能混用；`.139` 仍是默认构建目标。
**.155 已有用户报告的单项实机成功：签名模块安装、主题路径配置及自定义设置启动器图标可见。**
这不是全部资源 owner 的硬件验收；`.139` 仍为 **NOT_PROBED**。流程与证据边界见
[实机记录](docs/INSTALL.md#155-用户报告的单项实机记录)。宿主测试和固件指令仿真不替代设备验收。

## 已实现

- 最多 64 条最长前缀目录映射，从 `/data/canopus/themes/mappings.tsv` 完整校验后提交。
  路径最多 255 字节，配置最多 32 KiB。替代资源限于 `/data/canopus/themes/`
  词法路径；拒绝路径穿越、重复规则和非法前缀，主题树不得含逃逸符号链接。
- 只 Hook 所选目标的 `/` LVGL POSIX driver、读模式 2；处理相对 driver 路径和
  `fd + 1` 句柄。替代文件打不开时退回原资源；成功打开但解码失败不自动回退。
- 已知 callback 才允许安装/重绑定；短临界区发布状态。首次激活后映射锁定，
  open hook 和重载适配器共享同一份映射，不支持热更新规则或热卸载。
- **.155 定向图片退休：** 只退休源类型为文件、绝对路径命中有效映射的图片头和
  解码缓存项；不动其他路径、其他驱动、内存描述符或符号源。使用原生逐键
  `lv_cache_drop`，不是全局清空；持有项摘链并标失效，载荷由原生最后一次引用
  释放。遍历有类、布局、环路/数量及摘链进度检查。
- **.155 图片尺寸/元数据刷新：** 在退休后扫描 LVGL 已注册屏幕树（含离屏屏幕）。
  仅对**精确 image class** 的受影响文件源，先查询新头信息，成功后重新传入原来的
  source 指针调用原生 setter，更新宽高/尺寸失效，不换字符串。头缓存关闭、
  查询失败或对象/源已变化时跳过，不冒充完成任意资源替换。
- **.155 图片样式：** 普通对象/图片按钮的当前状态、主 part 的属性 40 文件源命中
  映射时，显式调用原生 style refresh。保持原有样式指针、selector 和状态；不依赖
  “再设置同一个指针”触发刷新。其他 part/属性及私有样式资源尚未覆盖。
- UI 请求一次性且可合并：忙碌、缓存未就绪、快照分配失败等情况由临时 LVGL timer
  重试；成功请求整屏脏区后自删。已经完成的退休/元数据阶段不因脏区被拒绝而反复执行。
  定时器分配失败返回 -2011，重定向仍驻留。
- 字体注册路径按映射 remove/add 并重新解析确认；**不是 live/idle 字体替换**。
  不自动清字体缓存，也**不再强制重建栈顶页面**。
- RHQ1 保持 **v5、40 字节**兼容：`images_dropped` 是完成的定向退休轮次（不是
  图片数量/全局清空次数），`redraws` 是被接受的整屏脏区请求，`rebuilds` 保留为 0。
  计数器不证明所有图片、字体或 GPU 工作已采用新资源。详见 [安装文档](docs/INSTALL.md)。

**.139 保守兼容：** 保留路径重定向、字体注册路径重定向和重绘请求，但停用旧的
未经验证的全缓存遍历，也不套用 .155 图片/样式对象布局。当前 `.139` 不保证已缓存
图片立即采用映射，`images_dropped` 为 0；需要独立固件证据后才能启用对应适配器。

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
- 不自动重启 miwear，不保证 Hook 早于首次资源读取，没有主题选择器/资源上传 UI。
  `restart_miwear.sh` 故意退出 78；stop/deactivate 的 reboot-required 不表示已拆除 Hook。

项目标识为 `org.canopus.resource-hook`；运行时/收据标识为 `resource_hook`。
旧 `manager_resource_hook` 不会自动迁移：先禁用并完整重启，再安装新模块，不能同时启用。

新适配器的可重复验证：

```sh
RESOURCE_HOOK_TARGET=xiaomi-band-11-4.100.155 \
  "$FIRMWARE_PYTHON" tests/firmware_reload.py
```

该测试临时编译源文件，不读取签名密钥，不依赖已签名 payload。原生 cache drop/release、
屏幕遍历、image setter 和 style refresh 执行真实 .155 指令，I/O/decoder/GUI 叶节点
按测试说明建模；**没有测试真实显示器或 GPU 完成**。

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
配置文件不存在时激活为成功 no-op，不安装 hook 或刷新 UI；补齐后再次激活即可
重试。空配置、非法配置及其他读取错误仍会报错。构建需要含已批准 errno veneer
的 Canopus 目标包（`.139` / `.155`）。
完整协议、错误码、版本映射和回退步骤见 [docs/INSTALL.md](docs/INSTALL.md)。
固件证据：[.139 审计](targets/xiaomi-band-11-4.100.139/ui-reload-audit.md)、
[.155 迁移证据](targets/xiaomi-band-11-4.100.155/)。
