## 快速上手

### 1. 开发

```
npm install
npm run start
```

开发构建也会压缩 JavaScript 并缩短局部变量名，保留日志和源码映射。未压缩的大型页面包可能在 Vela 模拟器加载阶段触发 `OutOfMemory`，随后框架报告 `TypeError: not a function`。

### 2. 构建

```
npm run build
npm run release
```

### TypeScript

Vela 的 `.ux` `<script>` 仍然使用 JavaScript；独立的 `src/**/*.ts` 模块由 `quickapp.config.js` 配置的 Rspack `builtin:swc-loader` 直接编译，并可从 `.ux` 页面导入。TypeScript 编译不负责类型检查，因此 `tsc --noEmit` 作为单独步骤校验类型；`start`、`build` 和 `release` 会先运行类型检查。

注意：aiot-toolkit 对 `@system.*` / `@service.*` 的特殊模块转换目前由 `.ux` / `.js` 处理；独立 `.ts` 模块里先不要直接导入这些模块。

开发时开两个终端：先启动快应用，再在另一个终端监听 TS 类型错误；aiot 的 watch 会在 TS 源文件变动时重新打包：

```
# Terminal 1
npm run start

# Terminal 2
npm run ts:watch
```

手动检查类型：

```
npm run typecheck
```

## Receiver performance and persistence

The receiver caches successfully confirmed parent directories within the current transfer/connection epoch. Replace, resume, reconnect, stop and I/O errors invalidate the cache; callbacks from an older epoch cannot repopulate it. Progress is reconstructed once from durable state after launch and then updated only after successful saves. Duplicate chunks do not increase received bytes.

New progress is stored in two binary slots, `internal://files/interconnect-transfer.json.journal-0` and `.journal-1`. A checkpoint contains the complete v1 state; subsequent records contain header changes and at most one changed file. Each frame has a boundary, checksum and sequence chain. Logs rotate after 1,024 records or 1 MiB of deltas, writing the inactive slot before switching. A failed append does not commit in-memory progress or send an ACK; retry uses a fresh checkpoint rather than relying on native writes to truncate a torn tail. Identifiable final partial writes recover the validated prefix; complete corruption (including an older slot), short reads and non-missing native I/O errors fail closed instead of rolling back acknowledged progress. Resource bytes are still written before the corresponding durable state and ACK.

Existing `interconnect-transfer.json` v1 snapshots remain readable and undergo the same receiver validation. The legacy file is not rewritten: downgrading Manager cannot recover new journal progress. Do not remove journal slots independently or run old/new receivers against the same file area. Native write completion is the durability boundary available to Manager; hardware power-loss guarantees require device testing.

Host regressions cover complete transfers, duplicate/out-of-order chunks, restart/resume, cache invalidation, delayed callbacks, failed/partial writes and checkpoint rotation. Run `node tests/test_interconnect_performance.cjs` and `node tests/test_transfer_storage.cjs` from the repository root. Reported byte counts are API payloads, not physical flash writes or device timings.

## 重载性能

重叠目录先按资源包优先级和混搭选择解析到具体文件；生成的配置满足 256 条规则、32 KiB 限制时直接映射到包内文件，不复制图标。超出预算时才生成不可变的活动合并目录。

同一应用会话共享资源快照，并缓存最近一次成功写入的活动映射。包内容、排序和混搭未变化时重载不重复扫描、复制或写入映射，但仍发送新的请求版本并等待对应模块回执。接收、替换或删除包使快照失效；应用重启后重新加载。不检测快应用文件区的外部修改。

目录合并仅等待原生 `file.copy` 完成，不做复制前后的逐文件大小/类型检查，也不计算内容哈希；保留清单和路径校验、复制错误回滚，以及成功回执之后的旧代次清理。直接映射仍受活动包禁止替换/删除的保护；已有活动代次索引会持久化可能仍被模块使用的新旧包，只有匹配的成功回执才释放旧包保护。更新包前需先切换并成功重载。

## 界面背景（启动器、控制中心、通知中心）

`corona.json` 可选 `backgrounds: {"launcher":"assets/l.bin","controlCenter":"assets/c.bin","notifications":"assets/n.bin"}`，也可为每个项指定 `{"mode":"image","image":"..."}` 或 `{"mode":"system"}`；兼容旧版单项 `launcherBackground`。image 必须是安全的包内相对文件路径且实际存在；不指定则继承。系统样式分界以上按现有优先级逐区域取第一个声明生效；没有声明则使用系统背景。

激活时生成独立的 `internal://files/backgrounds.cfg`，内容为换行分隔的 `version=1` 及各区域键值（如 `launcher=themes/<id>/<path>`、`control_center=system`）。与 mappings 一起准备并写入，再走现有 reload 请求/回执；缓存命中不重复写入，没有逐帧检测。两个文件不是原子事务：任一写入失败不发送 reload，缓存失效后重试修复。背景所在包与直接映射包一样受保护，成功回执后释放旧包。需要支持此配置的运行时；旧运行时不应用自定义背景。

## 首页模块状态

首页状态卡片适配 212×520 窄屏及 336×480 宽屏，使用提供的 PNG 图标。图标框以右下定位匹配 Figma，图片另作透明边距补偿，超出部分由圆角卡片裁切。

- 绿色：正在工作 / 已加载 x 条规则（模块实际生效数量，零规则也正常）。
- 黄色：配置错误 / 模块已安装（不把字体刷新失败当成配置错误）。
- 红色：模块异常 / 未检测到模块（未收到有效响应，不证明已卸载）。
- 检测期间及文件 I/O 失败使用中性占位，不误报模块未启动。

同一应用会话首次进入首页仅发送一次状态查询，返回首页复用最近结果。重载和查询串行共用 `control.request` / `control.response`；reload v2 回执直接更新状态及规则数，不新增定时查询或重载后查询。只有成功重载回执允许清理旧资源代次。Manager 必须与支持 reload v2 的模块配套更新，协议见 [MODULE_CONTROL.md](../docs/MODULE_CONTROL.md)。

## 了解更多

你可以通过我们的[官方文档](https://iot.mi.com/vela/quickapp)熟悉和了解快应用。
