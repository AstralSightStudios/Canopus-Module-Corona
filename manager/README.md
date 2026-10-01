## 快速上手

### 1. 开发

```
npm install
npm run start
```

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

## 重载性能

重叠目录先按资源包优先级和混搭选择解析到具体文件；生成的配置满足 256 条规则、32 KiB 限制时直接映射到包内文件，不复制图标。超出预算时才生成不可变的活动合并目录。

同一应用会话共享资源快照，并缓存最近一次成功写入的活动映射。包内容、排序和混搭未变化时重载不重复扫描、复制或写入映射，但仍发送新的请求版本并等待对应模块回执。接收、替换或删除包使快照失效；应用重启后重新加载。不检测快应用文件区的外部修改。

目录合并仅等待原生 `file.copy` 完成，不做复制前后的逐文件大小/类型检查，也不计算内容哈希；保留清单和路径校验、复制错误回滚，以及成功回执之后的旧代次清理。直接映射仍受活动包禁止替换/删除的保护；已有活动代次索引会持久化可能仍被模块使用的新旧包，只有匹配的成功回执才释放旧包保护。更新包前需先切换并成功重载。

## 了解更多

你可以通过我们的[官方文档](https://iot.mi.com/vela/quickapp)熟悉和了解快应用。
