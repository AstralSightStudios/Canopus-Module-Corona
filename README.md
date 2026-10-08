# Canopus-Module-Resource-Hook

> **Xiaomi Band LVGL 资源路径动态重定向模块 (`ng.lst.corona`)**  
> 运行于 Canopus 模块框架之上，免修改系统只读分区即可实现对手环 UI 图片、图标及主题资源的无感重定向与实时热重载。

[![License: AGPL v3](https://img.shields.io/badge/License-AGPL_v3-blue.svg)](LICENSE)
[![Version](https://img.shields.io/badge/Version-0.3.0-green.svg)](Canopus.toml)

---

## 背景配置支持范围

Manager 始终生成 `version=1` 的 `launcher`、`control_center`、`notifications`、
`widgets` 四个独立配置项，支持图片、`blur` 和 `system`；缺省项恢复系统背景。
Band11 的 `widgets` 仅解析、不执行，不触发该区域的图片检查、加载、模糊或原生挂钩。

p67 3.101.043 默认不启用原生背景替换。精确目标可通过
`RH_P67_LIVE_BACKGROUNDS=1` 构建四区域静态背景 LIVE 前景实验；
`RH_P67_LIVE_LAUNCHER=1` 保留原有仅启动器实验。
背景固定在屏幕坐标并按移动页面裁剪；其余三区保留原生快照生命周期，
隐藏快照图像并显示实时前景。`blur` 在 REFR_START 按需主动采集到模块自有 BGR888 缓冲，
生成模块自有 RGB565 静态模糊背景；失败在 REFR_READY 重试、不访问原生源池、不做 inline patch，
不使用透明快照、不改日历。模糊不是持续采集的实时效果；必须显式等待 raw GPU fence，不得猜测缓存 API。
静态模糊的设备绘制、GPU/cache 行为与帧时间均为 **NOT_PROBED**；既有图片背景的
用户反馈不代表新增模糊路径已获设备验证。
详见 [p67 审计记录](docs/P67_BACKGROUNDS.md)。

## 架构概览

```text
+-------------------------------------------------------------+
|             Corona Editor (PC 端主题包编辑器)                |
|            可视化制作资源包 / 映射编辑 / 导出主题包           |
+-------------------------------------------------------------+
                               | 传输
                               v
+-------------------------------------------------------------+
|                     Manager (快应用前端)                     |
|          资源包管理 / 优先级排序 / 混搭配置 / 发送重载信号       |
+-------------------------------------------------------------+
                               |
               control.request | control.response
              (mappings.tsv)   | (RHST1 / RHRS2)
                               v
+-------------------------------------------------------------+
|             Canopus-Module-Resource-Hook (C 核心)           |
|  +---------------------+  +-------------------------------+ |
|  | 紧凑规则索引 (二分)  |  | 定向缓存退休 (lv_cache_drop)   | |
|  | 最长前缀 / @system   |  | UI 脏区重绘与 Owner 刷新      | |
|  +---------------------+  +-------------------------------+ |
+-------------------------------------------------------------+
                               | Hook
                               v
+-------------------------------------------------------------+
|                 LVGL POSIX Driver ('/' 读模式)              |
|        透明重定向: ROMFS 原生资源  ==>  themes/... 替代资源    |
+-------------------------------------------------------------+
```

---

## 支持设备与固件

| 设备型号 | 固件版本 | 验证状态 | 图片重定向 | 快应用图标 | 日历动态图标 | 字体热重载 |
|---|---|---|:---:|:---:|:---:|:---:|
| **Xiaomi Band 11** | `4.100.177` | 实机通过（用户反馈） | 静态 | 静态 | ✅ | 静态 |
| **Xiaomi Band 11** | `4.100.155` | 实机通过（用户反馈） | ✅ | ✅ | ✅ | ✅ |
| **Xiaomi Band 11** | `4.100.139` | 实机通过（用户反馈，默认目标） | ✅ | ✅ | ✅ | ✅ |
| **Xiaomi Band 10 Pro** | `3.101.043` | 实机通过（用户反馈） | ✅ | ✅ | ✅ | ✅ |

> **当前源码日历变更：** 新透明、可配置日历仅支持 `.177`，用户于 2026-10-07 反馈实机验证通过（USER_REPORTED_PASS）。新原生 API 已获用户审批并登记于精确目标包（revision 2）；此反馈不等同于逐像素一致性、故障恢复或逐 API 的设备证明。其他目标不支持 `calendar` 配置，模块已移除主动调用旧生成/通知函数，系统自己的日历更新仍照常运行。详见 [日历配置与限制](docs/CALENDAR_RELOAD.md)。
>
> **注意：** 各固件目标使用独立的 ELF、内存布局和地址白名单，**严禁混用**。  
> 升级说明：本模块与旧版 `resource_hook` / `manager_resource_hook` 不兼容。安装前请先停用并完整重启手环。

---

## 核心特性

- **高性能紧凑重定向**：基于内存优化快照（256 条规则仅占 ~13 KiB），二分查找精确命中 + 最长前缀目录回退，支持 `@system` 原生透传与遮蔽。
- **精准定向缓存退休**：使用 `lv_cache_drop` 逐键退休受影响的文件缓存并申请局部脏区重绘，拒绝粗暴的全局清空，杜绝掉帧与白屏。
- **动态图标支持**：
  - **快应用包名图标**：支持 `@quickapp-icon/<package>` 声明，UI 线程只读查询真实应用注册记录并精准映射。
  - **透明日历图标（.177，用户反馈实机通过）**：独立 ARGB8888 合成与文件读取重定向；`corona.json` 的 `calendar` 支持 month/day/weekday 文字或图片，底图仍使用原来的资源映射。配置示例见 `examples/calendar/corona.json`。
- **字体热重载**：三个目标默认启用受检查的字体事务，保留现有字体 wrapper、fallback 与原厂路径基线，支持字体切换和规则移除后的原厂恢复。需使用不可变字体代次；GPU 故障恢复、框架重启及自定义字体 owner 不在支持范围内。
- **优雅的配置与控制通道**：模块与 Manager 通过 `control.request` / `control.response` 进行全异步双向通信，无需重启手环即可无感热更新规则。
- **独立容错与启动诊断**：配置缺失或语法错误自动退回 pass-through 零规则模式，绝不阻断系统启动；启动各阶段独立记入 `/data/offlinelog/resource-hook-startup.log`。

---

## 规则配置 (`mappings.tsv`)

配置文件位于快应用文件根目录下的 `mappings.tsv`（以真实 `\t` 制表符分隔，最多 256 条）：

```tsv
# 目录映射：必须以 '/' 结尾，自动追加子路径
/system/images/icons/	themes/my_theme/icons/

# 精确文件重定向：以 .bin 结尾，完整路径匹配
/system/fonts/default.bin	themes/my_theme/fonts/custom.bin

# 系统遮蔽：强行透传原厂文件，不被上层目录规则覆盖
/system/images/icons/battery.bin	@system

# 快应用包名图标：按 package 名称自动解析替换启动器图标
@quickapp-icon/ng.lst.corona	themes/my_theme/icons/corona.bin
```

---

## 构建与测试

### 环境依赖

- 相邻 `../Canopus` 仓库（需先编译 `cargo build -p canopus-cli`）
- Clang / LLVM (支持 ARM target), `ld.lld`, Python 3.11+, OpenSSL (带 Ed25519 支持)

### 常用命令

```sh
# 1. 编译指定目标 (生成 build/resource-hook.elf)
sh scripts/build.sh xiaomi-band-11-4.100.155
sh scripts/build.sh xiaomi-band-10-pro-3.101.043

# Signed normal installer (font reload is included by default)
python3 scripts/build-watchface.py --target xiaomi-band-10-pro-3.101.043

# 2. 运行宿主单测 (包含紧凑算法差分与边界测试)
python3 tests/test_delivery.py

# 3. 完整签名打包交付 (产物生成到 dist 目录)
python3 scripts/build-delivery.py dist/0.3.0-155 --target xiaomi-band-11-4.100.155
```

---

## 生态与工具

- 🎨 **[Corona Editor](https://github.com/leset0ng/Corona-editor)**：官方可视化主题包编辑器，支持图形化配置资源映射、打包主题包并配合 Manager 快速导入。
- 🛠️ **[资源图片处理工具](tools/RESOURCE_IMAGE.md)**：用于只读 ROMFS 提取、LVGL v9 I8 BIN 与 PNG 互相转换及预览的脚本套件。

---

## 深度专题文档

如果你需要深入了解实现细节、逆向审计证据或协议标准，请参阅：

- 📦 [安装、回退与实机验收指南](docs/INSTALL.md)
- 🔌 [模块控制协议规范 (`control.request/response`)](docs/MODULE_CONTROL.md)
- 🩺 [启动诊断日志与错误排查](docs/STARTUP_DIAGNOSTICS.md)
- ⚙️ [底层生命周期边界、内存基准与固件仿真](docs/LIFECYCLE_AND_INTERNALS.md)
- 📅 [原生日历图标动态重载机制](docs/CALENDAR_RELOAD.md)
- 🔤 [字体热重载、实机验收与安全限制](docs/FONT_RELOAD.md)
- 🎨 [资源图片转换与解包工具说明](tools/RESOURCE_IMAGE.md)

---

## 许可证

本项目采用 GNU Affero General Public License v3.0（`AGPL-3.0-only`），完整条款见 [LICENSE](LICENSE)。
