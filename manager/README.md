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

## 了解更多

你可以通过我们的[官方文档](https://iot.mi.com/vela/quickapp)熟悉和了解快应用。
