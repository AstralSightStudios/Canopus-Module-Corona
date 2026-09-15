# Canopus-Module-Resource-Hook

**0.3.0 · Xiaomi Band 11 / 4.100.139 · 资源路径重定向集成交付**

提供经过宿主测试和固件指令仿真验证的驻留资源 Hook、签名 ELF/CMI1 payload、
安装表盘 Lua/资源 ZIP，以及可重复执行的完整交付构建。
**实机状态仍为 NOT_PROBED；不能视作已经通过设备验收的生产主题模块。**

**图片重载已实现：** activate 成功安装重定向后，模块在 UI 所属线程逐项退休
图片缓存（`lv_cache_drop`，0x0c8b8cae），再调用 `_lv_inv_area`（0x0c382428）
把整块活动屏标记为脏，让固件自身的刷新定时器在下一次 tick 重绘全屏、经重定向
重新解码——不必再等用户翻页。**逐项退休而非整表清空**：仍被持有的缓存项只被
标记失效并摘链，载荷等最后一个引用释放时才回收，不会把数据从活控件下面抽走；
引用计数为 0 的项立即释放。这样下一次查找必定落空，从而走重定向。

刷新请求是**一次性且会合并的**：UI 忙碌（正在渲染、失效被禁用、尚无活动屏）时
请求不会被丢弃，模块用一个临时 LVGL 定时器重试，成功后立即删除自己——不是周期
性反复刷新。

**功能闭环仍未完成：** 字体重载、miwear 安全重启、首个资源访问前安装 Hook 仍
缺失。字体 wrapper 由页面按引用计数持有，盲目释放会破坏 UI，安全重载需页面重建
生命周期。页面重建（仅栈顶页）和字体重定向现已实现：字体走原生 `access()`/FreeType，
不经过本模块 hook 的 LVGL POSIX open，因此模块改为直接改写字体管理器的注册表
（先 remove 再 add——直接 add 会被启动时注册的同名条目遮蔽而静默无效）。
图片主路径已追踪确认逐帧重开
（`lv_draw_image` 每帧 open/close 解码器，未命中即走 `lv_fs_open` 到重定向）；
但 `lv_draw_image` 的延迟绘制任务分支、以及自带解码缓冲的动画/画布类控件
**未追踪**，可能仍显示旧图。资源文件和配置仍需手动传输，没有主题选择器。
具体使用方式见 [配置和启用](docs/INSTALL.md)。

目录已由 `Canopus-Manager-Resource-Hook` 改为 `Canopus-Module-Resource-Hook`。
项目标识为 `org.canopus.resource-hook`；运行时/收据标识为 `resource_hook`。
旧标识 `manager_resource_hook` 不会被自动迁移：先禁用旧模块并完整重启，
再安装新模块，不能同时启用两者。

## 已实现

- 最多 64 条最长前缀目录映射；路径最多 255 字节，配置文件最多 32 KiB。
- 从 `/data/canopus/themes/mappings.tsv` 加载；先完整校验，再提交配置。
- 替代资源必须位于 `/data/canopus/themes/` 词法路径下；拒绝路径穿越、
  重复规则、控制字符和非法目录前缀。主题树不得包含逃逸符号链接。
- 只 Hook `.139` 的 `/` LVGL POSIX driver 读操作；正确处理去首斜杠 ABI
  和 `fd + 1` 文件句柄；替代文件打不开时回退原始资源，不递归重映射。
- 已知 callback 才允许安装/重绑定；短临界区发布状态和 slot，不覆盖未知 Hook。
- activate 安装重定向后逐项退休图片缓存（`lv_cache_drop`），随后调用
  `_lv_inv_area` 把整屏标记为脏，让固件刷新定时器在下一次重绘经重定向重新解码，
  无需翻页。被持有项只标记失效并摘链，载荷在最后一次引用释放时回收。
- 刷新请求一次性、可合并、不丢失：UI 忙碌时用临时 LVGL 定时器重试，成功即自删；
  只有确认整屏脏区被固件接受才计数成功。定时器分配失败返回 -2011，重定向保持驻留。
- 改写字体管理器注册表，把路径命中映射规则的字体名指向主题文件（先
  `font_manager_remove_path` 再 `font_manager_add_path`，并重新解析确认）。
  只影响之后新建的字体 wrapper，因此排在页面重建之前。
- 强制重建**栈顶页面**（`exec_pop_lifecycle_without_cachepolicy` + `on_resume_wrapped`），
  让持有资源的控件被重建而非重绘——普通导航按缓存策略只 pause/stop，控件不变，
  这正是"翻页再回来"无效的原因。策略 2、异步销毁、屏幕关闭等情况安全跳过；
  只有页面真正销毁并拿到不同根视图才计入 `rebuilds`。
- 活动屏指针取自真实 accessor 校验过的 `disp+696`（`disp+24` 是 DPI，非活动屏）。
- 驻留规则在首次激活后锁定；状态查询（RHQ1，v5，40 字节）提供规则数、
  饱和的成功/回退计数、图片缓存退休次数 `images_dropped`、整屏失效成功次数
  `redraws`、栈顶页重建次数 `rebuilds` 和字体重定向条目数 `fonts_retargeted`。
- Manager 模块详情页提供“立即激活”（需确认）：本次运行即加载并激活，不再只写
  下次启动意图；安全模式、未签名和仅重启类模块不提供该入口。
- 生成 `resource-hook.elf` 和 Ed25519 CMI1 收据，绑定目标、固件和 ELF 摘要。
- 安装表盘使用普通 IO，经 `/canopus/install` 导入；只安装为禁用状态，不启用
  模块、不加载框架、不执行 shell 命令，也不擅自覆盖配置文件。

## 明确不包含

- 自动停止/重启 miwear、完整 native UI teardown。
- 确保 Hook 早于所有字体/图片首次读取。
- 主题字体文件的格式有效性检查（只检查 access()，解析失败不回退）。
- 强制同步整屏刷新（模块只把整屏标记为脏，实际重绘仍由固件刷新定时器完成）。
- 重建栈顶页之外的页面；栈内其他页面仍持有旧资源。
- `lv_draw_image` 延迟绘制任务分支、以及自带解码缓冲的动画/画布类控件的验证；
  它们可能仍显示旧图，需页面重建。
- 成功打开但解码失败的资源自动回退。
- 热卸载、热更新映射或跨固件版本兼容。
- 已完成的 MPU/cache/display 实机验收，或 canonical `.canopus` 包格式。

`restart_miwear.sh` 故意退出 78；固件反例测试证明固定延时再启动 builtin entry
不会得到干净的 UI 重建。stop/deactivate 返回 reboot-required 时表示仍需
完整设备重启，不代表 Hook 已经拆除。

## 从源码构建

依赖：相邻 `../Canopus` 源码及其已构建 CLI、C 编译器、Clang ARM 后端、
`ld.lld`、Python 3.11+、支持 Ed25519 的 OpenSSL。完整交付还需要 Lua 5.3+、
Canopus 的固件测试 Python 环境、目标固件文件及已构建的 Band 11 Supervisor
测试资源。这些固件和工具不随本模块分发。

路径均可覆盖，不依赖 `/Volumes/EXT0`：

| 环境变量 | 默认值 |
|---|---|
| CANOPUS_ROOT | `../Canopus`（相对于模块目录） |
| CANOPUS_CLI | `$CANOPUS_ROOT/target/debug/canopus` |
| CC / CLANG / LD_LLD | `cc` / `clang` / `ld.lld` |
| MODULE_INSTALL_KEY | `$CANOPUS_ROOT/.canopus-local/module-installer-ed25519.pem` |
| FIRMWARE_PYTHON | `$CANOPUS_ROOT/build/band11-tests/bin/python` |
| CANOPUS_TEST_LUA | `lua` |
| RESOURCE_HOOK_PAYLOAD | 单独运行测试时覆盖 payload 目录 |

在 Canopus 仓库先执行 `cargo build -p canopus-cli`。在模块源码目录中：

```sh
# 三组 ASan/UBSan 宿主测试、ARM 编译、ELF verifier；不需要私钥
sh scripts/build.sh

# 完整签名交付：运行全部检查，然后生成 payload、安装表盘资源和 ZIP
# 输出目录必须尚不存在；不会覆盖以前的交付
python3 scripts/build-delivery.py dist/0.3.0
```

签名 key 必须已被 Supervisor 信任；完整交付构建会用 **Supervisor 源码中的
可信公钥**再验签。没有匹配的 key 时构建失败，不自动生成替代信任根，不修改
Supervisor 公钥，也不把私钥打包或上传。

输出结构：

```text
dist/0.3.0/
├── payload/xiaomi-band-11-4.100.139/
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
└── resource-hook-0.3.0-xiaomi-band-11-4.100.139.zip
```

安装表盘 ZIP 是**供表盘打包工具使用的 Lua/资源包**，不是已经封装好的厂商
表盘文件，也不是无需框架即可安装的引导包。只有 matching Supervisor 已就绪
时才可使用。配置和主题文件要单独传输；详见 [安装、回退和实机验收](docs/INSTALL.md)
（交付 ZIP 中是根目录的 `INSTALL.md`）。

## 单独验证

```sh
sh scripts/build-install-payload.sh xiaomi-band-11-4.100.139 \
  build/payload-0.3.0/xiaomi-band-11-4.100.139
python3 tests/test_delivery.py
../Canopus/build/band11-tests/bin/python tests/firmware_paths.py
../Canopus/build/band11-tests/bin/python tests/firmware_restart.py
../Canopus/build/band11-tests/bin/python tests/firmware_rebind.py
```

若该 payload 目录已存在，选择新输出路径并设置 `RESOURCE_HOOK_PAYLOAD`，不要
删除不明来源的目录。verify-payload.py 接受独立可信公钥；包内公钥仅供比对，
不能单凭“使用包内公钥验签成功”确认发布来源。

配置样例：[examples/mappings.tsv](examples/mappings.tsv)，其中分隔符是真实 TAB。
完整协议、错误码、版本映射和回退步骤见 [docs/INSTALL.md](docs/INSTALL.md)。
固件证据：[ui-reload-audit.md](targets/xiaomi-band-11-4.100.139/ui-reload-audit.md)。
