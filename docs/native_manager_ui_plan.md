# Corona 原生管理器 UI 设计与实现方案

状态：原生传输状态与界面已接入设备端 CanopusConnect 接收、校验、解压和登记链路；手机端传输与真机验收仍待完成。对应 [CanopusConnect 精简协议](canopusconnect_proto.md) 与 [CanopusConnect 两端实现方案](canopusconnect_implementation.md)。

实施时以实际 SDK 与原管理器代码为准：原生页面与桌面入口通过 `publish_native_app_stage` 分阶段注册；`ui.post` 仅安排前台 UI 工作，不是后台线程；连接是否就绪由未来传输层推送，不能把 `remote_listening` 当作设备已连接。

已知缺口：Canopus 已移除 TOAST API（`CANOPUS_UI_NODE_TOAST` / `CANOPUS_UI_CAP_TOAST`）。重载结果与活动保护记录失败已改由 §4.1.1 状态卡结果层承载，删除相关反馈由 §4.6 确认页承载；仍无 UI 承载的只剩安装完成包名揭示与排序/选择保存失败文案。现存提示面为页面级错误标签、首页状态卡重载结果层与资源管理页的传输横幅。

---

## 1. 背景与总体决策

1. **合并进 `corona` 模块本体（C 语言，Canopus ABI 2.0）**：
   - 移除独立的 Vela 快应用前端，将 Corona Manager 以 C 语言直接构建进 `corona` 模块（`Canopus.toml` 升级为 `canopus_abi = "2"`，描述符升级为 `CANOPUS_ABI_MAJOR = 2, CANOPUS_ABI_MINOR = 0`）。
   - 通过 `publish_native_app_stage` 的第一阶段调用 `ctx->ui->set_entry` 注册模块原生页面，第二阶段通过 `ctx->launcher->register_app`（ADR-CAN-011，页面数组为空）登记 `Corona` 应用及启动器入口；应用 ID 由 Supervisor 统一分配并持久化，不在启动激活回调中直接发布 UI。
2. **模块数据通讯改为纯内部状态（零文件轮询）**：
   - 废弃快应用时期通过 `control.request` / `control.response` 的每秒文件轮询机制（原 `queryModuleStatus` 与 `waitForReloadOutcome` 轮询）。
   - Manager UI 与 Resource Hook 运行于同一模块实例及同一 serialized module/UI owner 任务：状态读取直接访问模块内存快照，配置重载直接调用模块内部发布事务，异步刷新完成与传输进度变更通过内部回调 + `ctx->ui->post` 推送驱动 UI 更新。
3. **视觉与组件原则**：
   - **首页状态卡片**：1:1 复刻既有视觉（尺寸、圆角裁剪、四态背景色、主副标题字号、右下角状态装饰图）。
   - **其余页面与控件**：采用 `../Canopus-Private` 原生 UI 宿主（`canopus_ui.h` / `canopus_ui_host.c`）提供的固件系统预制组件；底部胶囊操作按钮（重载、删除）及空状态插图继续复用既有切图资源（与固件原生底部图片按钮机制一致）；传输状态插图随独立传输页一并移除，进度改由资源管理页内嵌横幅承载。
   - **混搭微调增强**：突破快应用沙盒限制，在「选择来源」页新增对 **「默认」(`@default`)** 与 **「系统」(`@system`)** 选项的真实 `.bin` 图标预览。
4. **首批支持与模拟器验证目标**：
   - `xiaomi-band-10-pro-3.101.043`（`336×480` 方屏）
   - `xiaomi-band-11-4.100.155`（`212×520` 胶囊屏）
   - 新增构建目标 `xiaomi-band-11-4.100.179`：复用 Band 11 布局与 ABI 2 模块入口，使用独立的精确固件地址和回执指纹。宿主检查、静态固件核对与交付校验单独记录；现有 `.043` / `.155` Mirage 场景不代表 `.179` 验收，`.179` 实机与 Mirage 状态仍为 `NOT_PROBED`。见 [.179 适配记录](../targets/xiaomi-band-11-4.100.179/README.md)。

---

## 2. 模块内纯内部状态通讯（替代轮询）

### 2.1 运行状态快照与首页直读

Manager 与 Hook 共享模块内部状态，不再写 `control.request` 或轮询 `control.response`。模块内部暴露只读状态查询接口：

```c
enum rh_manager_module_state {
    RH_MGR_STATE_RUNNING = 0,
    RH_MGR_STATE_CONFIG_ERROR,
    RH_MGR_STATE_UNAVAILABLE,
};

struct rh_module_status_snapshot {
    uint8_t state;             /* enum rh_manager_module_state */
    uint8_t refresh_pending;   /* 1 while cache/font/calendar/image refresh is in flight */
    uint16_t active_rules;     /* S.count from active_snapshot */
    int32_t error_code;        /* latched config_error / quickapp / calendar / background error */
    int32_t last_font_result;  /* 1 = pending, 0 = ok, <0 = failed */
    uint32_t last_changed;     /* committed font/resource changes in latest transaction */
};
```

- **首次进入与重绘**：`on_open` 或返回 `ROUTE_INDEX` 时直接调用 `rh_module_get_status_snapshot(&snap)` 同步填充首页状态卡，零 I/O 延迟、零定时器轮询。
   - 生产原生构建关闭旧控制文件请求/回执轮询；兼容性 host 测试保留旧通道。日期变化、快应用图标变化与刷新重试属于 Hook 自身维护，不承担 Manager 状态通讯。

### 2.2 内部重载事务与事件驱动通知

当用户在首页点击「重载」按钮时：

```text
用户点击重载按钮
   │
   ▼
Manager 生成新代次配置 (calendar.cfg / backgrounds.cfg / mappings.tsv)
   │
   ▼
直接调用内部接口 rh_module_request_reload(generation_id)
   ├── 同步校验并 stage/commit 新快照、背景与日历配置
   ├── 立即更新内存 active_snapshot 与规则计数 S.count
   └── 启动 refresh_step / 50ms refresh_timer 执行缓存退休与重绘
   │
   ▼ (UI 心跳完成全部刷新阶段，或发生错误拒绝)
触发内部回调 rh_on_reload_complete(result, &status_snapshot)
   │
   ▼
ctx->ui->post(ctx, rh_ui_async_update, NULL)
   ├── 刷新首页状态卡（正在工作 / 已加载 N 条规则 / 配置错误）
   ├── 结果文案无临时提示面（TOAST API 已移除，见文首已知缺口）
   └── 若刷新成功，执行 cleanup_inactive_generations 清理旧代次叠加目录
```

### 2.3 传输状态内部订阅接口（对接 `canopusconnect_proto.md`）

对齐 [CanopusConnect 精简协议](canopusconnect_proto.md) §4 与 §8 的进度结构（`durableOffset`、`writtenOffset`、`archiveBytes`、`phase`、`detail`），定义纯内存传输 UI 状态模型：

```c
enum rh_transfer_phase {
    RH_XFER_PHASE_IDLE       = 0, /* 对应 UI: 等待连接 (未监听) / 等待文件 (remote_listening=1) */
    RH_XFER_PHASE_RECEIVING  = 1, /* 正在接收 ZIP 数据帧 */
    RH_XFER_PHASE_VERIFYING  = 2, /* 正在回读磁盘核对完整 ZIP SHA-256 */
    RH_XFER_PHASE_INSTALLING = 3, /* 正在流式解压、校验 corona.json 与提交注册表 */
    RH_XFER_PHASE_INSTALLED  = 4, /* 接收并安装完成 */
    RH_XFER_PHASE_ERROR      = 5, /* 传输或安装中断 (detail 对应协议错误码 1..11) */
    RH_XFER_PHASE_CANCELED   = 6, /* 任务已取消 */
};

struct rh_transfer_ui_state {
    uint8_t phase;             /* enum rh_transfer_phase */
    uint8_t detail;            /* 协议 code: OK/BAD/BUSY/STALE/OFFSET/IO/HASH/PACK/LIMIT/ACTIVE/... */
    uint8_t remote_connected;  /* UI readiness supplied by the future transport owner */
    uint8_t percent;           /* 0..100, derived from written_offset / archive_bytes */
    uint32_t written_offset;
    uint32_t durable_offset;
    uint32_t archive_bytes;
};
```

- **本轮范围**：实现 `rh_manager_set_transfer_state(const struct rh_transfer_ui_state *state)` 内部更新入口与资源管理页内嵌传输横幅渲染（见 §4.7）；后续接入 `canopusconnect_implementation.md` 的接收状态机时，只需在状态跳变或累计 ACK 写入时调用该接口，`ctx->ui->post` 会自动刷新横幅。传输不再占用独立路由：接收器与页面解耦，UI 关闭不影响接收与续传。

---

## 3. Canopus UI ABI 2.0 能力映射与双目标差异

### 3.1 `.043` 与 `.155` 原生宿主能力对照

| 能力项 | Band 11 (`.155`, `212×520`) | Band 10 Pro (`.043`, `336×480`) | 实现约束与避坑方案 |
|---|---|---|---|
| **原生顶栏 (`NAVIGATION_HEADER`)** | 高 `68px`；上下双层居中（顶弧时间 `08:03` + 下层居中标题） | 高 `56px`；单行左右分布（左侧 `< 标题`，右侧时间 `08:05`） | 统一使用 `CANOPUS_UI_NODE_NAVIGATION_HEADER`，由宿主自动适配各家族原生构造器 |
| **内容视口 (`content_root`)** | `y = 68`，`212×444` | `y = 56`，`336×424` | 根 `NAVIGATION_PAGE` key 设为 `1u + route * 10000u`，切换路由自动重置共享视口 |
| **原生列表行 (`ACTION_ROW` / `STATUS_ROW`)** | `204×96`，圆角 `24px`，`#222222`，行距 `8px` | `336×96`，圆角卡片，`#222222`，行距 `8px` | 两端均为**主标题（28px 白字）在上、副标题（24px 灰字）在下**的双行左对齐布局；原生行禁止设置非零 `padding` |
| **样式属性 (`CAP_STYLE` / `CAP_VISUAL`)** | 支持圆角、裁剪、边框色/宽、透明度 (`FW_UI_STYLE_OPA`) | **仅支持圆角 (`12`) 与圆角裁剪 (`45`)**，无 `OPA` 与 `BORDER` | `.043` 提交 `opacity` 或 `border` 会触发整树 `ERR_UNSUPPORTED`；状态卡副标题统一使用底色预乘后的等效 RGB |
| **任意字号 (`CAP_FONT`, `font_px`)** | 已支持（`native_font_create` + `DELETE` 事件引用计数回收） | 已支持（同左） | 仅用于 `TEXT`、`SECTION` 标题与 `CHECKBOX` 主文本；不可用于原生 `ACTION_ROW` / `STATUS_ROW` |
| **图片缩放 (`CAP_IMAGE_FIT_CONTAIN`)** | 已支持（Q8 独立缩放 + 尺寸解码查询 + 裁剪视口） | 已支持（同左） | 渲染前先确保目标 `.bin` 存在且非空，防止解码预检返回 `ERR_BACKEND` |
| **指针捕获与位移 (`CAPTURE` / `TRANSLATE`)** | 已支持 | 已支持 | 用于资源管理页长按拖拽排序，以及底部图片按钮（`reload*.bin` / `del*.bin`）的点击识别 |
| **虚拟列表 (`VIRTUAL_CONTENT` / `SCROLL`)** | 已支持 | 已支持 | 单次快照节点上限 `CANOPUS_UI_MAX_NODES = 32`、字符串池 `1536B`；所有长列表仅提交当前可见窗口 |

### 3.2 底部图片按钮（`reload*.bin` / `del*.bin`）的事件承载机制

在 `canopus_ui.c` 中，`CANOPUS_UI_EVENT_ACTIVATE` 仅派发给预制交互行，但当目标支持 `CANOPUS_UI_CAP_POINTER_EVENTS`（`.043` 与 `.155` 均支持）时，`CANOPUS_UI_EVENT_PRESS`、`CANOPUS_UI_EVENT_RELEASE` 与 `CANOPUS_UI_EVENT_CANCEL` 会派发给任何带非零 `event_id` 的可点击对象：

- LVGL v9 的 `lv_image` 默认不置位 `LV_OBJ_FLAG_CLICKABLE`，而 `CANOPUS_UI_NODE_LIST` 容器（由 `host_container_create` 创建的 `lv_obj`）默认具备 `LV_OBJ_FLAG_CLICKABLE`。
- 因此将 `reload.bin` / `del.bin`（Band 11: `150×78`）或 `reload-wide.bin` / `del-wide.bin`（Band 10 Pro: `324×80`）作为子节点放入一个居中定宽高的 `CANOPUS_UI_NODE_LIST` 容器，将 `event_id`（`EV_RELOAD` / `EV_DELETE`）绑定在该容器上。
- 当手指在该容器上按下并松开且位移未超过 `MOVE_SLOP (8px)` 时，触发重载或进入删除确认页。

---

## 4. 六大路由页面 UI 设计与交互规范

### 4.1 首页 (`ROUTE_INDEX`)

1. **顶部标题栏**：`NAVIGATION_HEADER`，`title = "Corona"`，`show_back = 1`（点击返回调用 `ctx->ui->close_page(ctx)` 退出应用）。
2. **模块状态卡片（1:1 复刻原视觉）**：
   - 容器：`CANOPUS_UI_NODE_SECTION`（`title = ""`），Band 11 尺寸 `204×100`，Band 10 Pro 尺寸 `324×100`；`style.corner_radius = 32`；`visual.flags = CANOPUS_UI_VISUAL_FLAG_BACKGROUND_RGB | CANOPUS_UI_VISUAL_FLAG_CLIP`。
   - 状态配色、文案与装饰图标：
     | 状态 | 背景色 (`background_rgb`) | 主标题 (`#FFFFFF`) | 副标题文案 | 副标题预乘色 (`foreground_rgb`) | 右下角图标 |
     |---|---|---|---|---|---|
     | `running` | `0x1DA362` | `"正在工作"` | `"已加载{N}条规则"` | `0xC6E8D8` | `status-running.bin` |
     | `config_error` | `0xDF9725` | `"配置错误"` | `"模块已安装"` | `0xF7E5C8` | `status-config-error.bin` |
     | `unresponsive` | `0xDA593C` | `"模块异常"` | `"未检测到模块"` | `0xF6D5CE` | `status-unresponsive.bin` |
     | `unavailable` | `0x262626` | `"检测失败"` | `"请稍后重载"` | `0xBFBFBF` | 无 |
   - 字号与坐标：
     - Band 11 (`.155`)：主标题 `(14, 14) 134×40 font_px=30`；副标题 `(14, 54) 134×32`，`running` 时 `font_px=17`、`unresponsive` 时 `font_px=21`、其余 `font_px=24`；装饰图保留原 PNG，布局框为 `(133, 1) 92×97`，由状态卡裁剪。
     - Band 10 Pro (`.043`)：主标题 `(14, 12) 183×42 font_px=32`；副标题 `(14, 54) 183×34`，`running` 时 `font_px=24`、其余 `font_px=26`；装饰图布局框为 `(186, -7) 138×145`，保留原图尺寸并由状态卡裁剪。坐标以旧页面实际 CSS 计算修正了计划初版近似值。
3. **功能入口行**：
   - `CANOPUS_UI_NODE_ACTION_ROW`：`label = "资源管理"`，`detail = ""`，点击推入 `ROUTE_RESOURCES`。
   - `CANOPUS_UI_NODE_ACTION_ROW`：`label = "混搭微调"`，`detail = ""`，点击推入 `ROUTE_MIX_MATCH`。
4. **底部重载按钮**：
   - 居中 `LIST` 容器 + `IMAGE`（Band 11 为 `reload.bin` `150×78`，Band 10 Pro 为 `reload-wide.bin` `324×80`），距屏幕底 `10px`（Band 11 内容坐标 `y=364`，Band 10 Pro `y=334`），与详情页删除按钮同位。
   - 点击触发内部重载事务，结果由状态卡的重载结果层展示（见 4.1.1）。

#### 4.1.1 重载结果层

状态卡是重载动作唯一的反馈面：传输横幅属于另一个路由，页面级错误标签会把卡片和按钮一起顶掉。结果层复用同一组节点（`20` 卡片 / `21` 标题 / `22` 副标题 / `24` 装饰图），不新增节点、不改尺寸。

| 结果 | 触发条件 | 主标题 (`font_px` 30/32) | 副标题 (`font_px` 17/24) | 背景色 |
|---|---|---|---|---|
| 进行中 | 事务已受理 | `"正在重载"` | `"重新读取映射"` | `0x1DA362` |
| `DONE` | `reload_result==0` 且 `last_changed>0` | `"重载完成"` | `"更新{N}项资源"` | `0x1DA362` |
| `DONE`（无变化） | `reload_result==0` 且 `last_changed==0` | `"重载完成"` | `"模块已响应"` | `0x1DA362` |
| `PROTECT_FAILED` | 刷新成功但活动保护记录写入失败 | `"重载完成"` | `"保护记录未存"` | `0x1DA362` |
| `LOAD_FAILED` | `stat` / `refresh` 失败 | `"重载失败"` | `"读取资源失败"` | `0xDF9725` |
| `PREPARE_FAILED` | `prepare_apply` 失败 | `"重载失败"` | `"映射生成失败"` | `0xDF9725` |
| `REJECTED` | `request_reload_from` 返回负值 | `"重载失败"` | `"错误码 -{N}"` | `0xDF9725` |
| `MODULE_FAILED` | 完成但 `reload_result<0` | `"重载失败"` | `"错误码 -{N}"` | `0xDF9725` |
| `TIMEOUT` | `5000ms` 内无本代完成 | `"重载失败"` | `"模块无响应"` | `0xDF9725` |

约束与取舍：

1. **文案预算**：主标题 ≤4 汉字（`font_px=30`、列宽 134），副标题 ≤7 汉字（`font_px=17` 在 134px 内可容 7.8 字）。所有结果文案按窄屏预算编写，宽屏复用同一字符串。
2. **失败用琥珀不用红**：`0xDA593C` 在现有语义里是「模块异常 / 未检测到模块」，重载失败时模块仍在运行，用红即误报状态。
3. **装饰图不随结果切换**：图是「模块状态」的视觉锚点，沿用 `U.status.state` 对应图片（`unavailable` 仍不绘制），结果只改文字与底色。
4. **驻留时长**：成功 `2000ms`，失败 `3500ms`（带错误码，需要更多阅读时间）。到期后回到真实模块状态；结果存续期间 notice 优先于状态渲染，但绝不超过驻留时长。
5. **看门狗（必需）**：`reload_generation` 永不回报时没有别的东西会清除 `busy`，底部按钮会永久置灰。`5000ms` 到期后三件事必须同时做：回滚暂存事务（`complete_apply(data, 0)`，否则数据层卡在 applying，后续 `prepare_apply` 全部失败）、从磁盘重新 `refresh`（模块可能仍会提交一个本页未观察到的事务）、清除 `busy` 并展示 `TIMEOUT`。
6. **降级**：`timer_create` 不可用（headless / 无心跳）时不设驻留与看门狗，结果保留到下一次点击重载或离开页面。
7. **不与页面级错误混用**：重载结果绝不写入 `U.error`，否则整页被错误标签顶掉，连重试按钮一起消失。

```text
Band 11 (.155, 212x520)                  Band 10 Pro (.043, 336x480)
   .--------------------.                +------------------------------------+
  /        08:03         \               | Corona                       08:05 |
 |        Corona          |              +------------------------------------+
 |------------------------|              | .--------------------------------. |
 | .--------------------. |              | | 正在工作               [Icon]  | |
 | | 正在工作     [Icon]| | [1:1状态卡]  | | 已加载3条规则          [Art ]  | |
 | | 已加载3条规则[Art ]| |  h=100 r=32  | '--------------------------------' |
 | '--------------------' |              | .--------------------------------. |
 | .--------------------. |              | | 资源管理                     > | |
 | | 资源管理         > | | [ACTION_ROW] | '--------------------------------' |
 | '--------------------' |  h=96 #222   | .--------------------------------. |
 | .--------------------. |              | | 混搭微调                     > | |
 | | 混搭微调         > | | [ACTION_ROW] | '--------------------------------' |
 | '--------------------' |  h=96 #222   |  .------------------------------.  |
 |    .--------------.    |              |  |      ( reload-wide.bin )     |  |
 |    | (reload.bin) |    | [150x78 Img] |  '------------------------------'  |
  \   '--------------'   /               +------------------------------------+
   '--------------------'
```

---

### 4.2 资源管理页 (`ROUTE_RESOURCES`)

1. **顶部标题栏**：`NAVIGATION_HEADER`，`title = "资源管理"`，`show_back = 1`。
2. **列表渲染（原生双行列表项 + 虚拟滚动窗口）**：
   - 容器：`CANOPUS_UI_NODE_SCROLL`，启用 `CANOPUS_UI_VISUAL_FLAG_VIRTUAL_CONTENT | CANOPUS_UI_VISUAL_FLAG_CLIP`，`content_height = row_count * ROW_STEP`（`ROW_STEP = 104`，即 `96px` 行高 + `8px` 间距），可见窗口 `WINDOW = 10`。
   - **已安装主题行**：使用 `CANOPUS_UI_NODE_ACTION_ROW`（右侧带 `>` 箭头）：
     - `label`（主标题）：`manifest.name`（若 manifest 损坏则回退为 `themeId`）
     - `detail`（副标题）：合并显示 `"{author} · v{version} · {description}"`（无作者/版本时显示 `"资源包详情 · {description}"`，manifest 损坏时显示 `"corona.json 不可用 · 包信息不可用"`）
     - 读取已安装包信息时优先 `corona.json`，缺失时才读取 `canora.json`，不额外探测或解析备用清单。列表和详情只读取元数据，不编译映射、背景、日历规则；规则校验留在混搭和应用路径。
     - 短按（未进入拖拽）推入 `ROUTE_RESOURCE_DETAIL`。
   - **系统样式分界行 (`@system`)**：使用 `CANOPUS_UI_NODE_STATUS_ROW`（无右侧箭头）：
     - `label`：`"系统样式"`
     - `detail`：`"设备内置样式 · 长按拖动排序"`
   - **空状态**：若无任何行，居中显示 `none.bin`（`205×152`）。
3. **长按拖拽排序交互**：
   - 按下某行达到长按阈值（收到 `CANOPUS_UI_EVENT_LONG_PRESS`）时：
     1. 调用 `ctx->haptics->vibrate_preset(ctx, CANOPUS_HAPTIC_TICK)` 触发短震反馈。
     2. 调用 `ctx->ui->control(ui, e->generation, e->key, CANOPUS_UI_CONTROL_CAPTURE, 0, 0, NULL)` 捕获指针并自动锁定父滚动容器。
     3. 将被拖拽行背景色设为高亮 `0x444444`。
   - 移动过程中（`CANOPUS_UI_EVENT_MOVE`）：
     1. 计算当前拖拽偏移 `drag_delta` 与目标槽位 `drag_to = clamp(round((origin_y + drag_delta) / ROW_STEP), 0, row_count - 1)`。
     2. 通过 `CANOPUS_UI_CONTROL_TRANSLATE` 实时更新被拖拽行（偏移 `drag_delta`）及受影响邻项（偏移 `±ROW_STEP`）的垂直位置。
     3. 当手指进入视口上下边缘 `42px` 区域时，由 40ms 周期定时器调用 `CANOPUS_UI_CONTROL_SCROLL_TO` 平滑推进列表滚动。
   - 松开或取消（`RELEASE` / `CANCEL`）：
     1. 调用 `CANOPUS_UI_CONTROL_RELEASE_CAPTURE` 释放捕获。
     2. 若为正常 `RELEASE` 且 `drag_from != drag_to`，重排数组并写入 `resource-order.json`；若写盘失败则回滚顺序（当前无失败文案，见文首已知缺口）。

```text
Band 11 (.155, 212x520)                  Band 10 Pro (.043, 336x480)
   .--------------------.                +------------------------------------+
  /        08:03         \               | < 资源管理                   08:05 |
 |      < 资源管理        |              +------------------------------------+
 |------------------------|              | .--------------------------------. |
 | .--------------------. |              | | 霓虹主题                     > | |
 | | 霓虹主题         > | | [ACTION_ROW] | | Alice · v1.2.0 · 赛博风格图标  | |
 | | Alice · v1.2 · ... | |  204x96      | '--------------------------------' |
 | '--------------------' |              | .================================. |
 | .====================. |              | || 系统样式                     || |
 | || 系统样式         || | [STATUS_ROW] | || 设备内置样式 · 长按拖动排序  || |
 | || 设备内置样式 ... || |  (长按拖动中)| '================================' |
 | '====================' |              | .--------------------------------. |
 | .--------------------. |              | | 极简黑白                     > | |
 | | 极简黑白         > | | [ACTION_ROW] | | Bob · v1.0.0 · 纯白线条图标    | |
 | | Bob · v1.0 · ...   | |  204x96      | '--------------------------------' |
  \                      /               +------------------------------------+
   '--------------------'
```

---

### 4.3 资源详情页 (`ROUTE_RESOURCE_DETAIL`)

1. **顶部标题栏**：`NAVIGATION_HEADER`，`title = "资源详情"`，`show_back = 1`。
2. **元数据展示（6 条原生 `STATUS_ROW`）**：
   - 依次渲染 6 个固定字段（主标题在上、内容在下）：
     1. `主题包`：`manifest.name`
     2. `作者`：`manifest.author`（缺省 `"未声明作者"`）
     3. `版本`：`v{manifest.version}`（缺省 `"未声明版本"`）
     4. `简介`：`manifest.description`（缺省 `"暂无简介"`）
     5. `适用设备`：`manifest.targets` 以 `"、"` 拼接（缺省 `"未声明目标设备"`）
     6. `资源包 ID`：`themeId`
   - 元数据长度超过系统双行项可显示宽度时，使用同字段标题的系统 SECTION/TEXT 分块完整展示；长说明与适用设备均可滚动，不截断原文。
3. **底部删除按钮进入确认页**：
   - `del.bin`（`150×78`）或 `del-wide.bin`（`324×80`）**悬浮于滚动列表之上**（复刻旧快应用 `position:absolute; bottom:10px`）：滚动视口铺满整个内容区（Band 11 `212×444` / Band 10 Pro `336×424`），按钮作为滚动容器之后的兄弟节点覆盖其上（Band 11 内容坐标 `y=364`，距屏幕底 `10px`；Band 10 Pro `y=334`）；滚动内容尾部保留 `90px`（Band 11）/ `100px`（Band 10 Pro）spacer，滚到底时末卡停在按钮上方 `10px`，滚动过程中卡片从按钮下方穿过。
   - 单击只做一次页面跳转：`navigate(ROUTE_DELETE, 0)` 打开 §4.6 确认页，删除事务永不从详情页发起。

```text
Band 11 (.155, 212x520)                  Band 10 Pro (.043, 336x480)
   .--------------------.                +------------------------------------+
  /        08:03         \               | < 资源详情                   08:05 |
 |      < 资源详情        |              +------------------------------------+
 |------------------------|              | .--------------------------------. |
 | .--------------------. |              | | 主题包                         | |
 | | 主题包             | | [STATUS_ROW] | | 霓虹主题                       | |
 | | 霓虹主题           | |  204x96      | '--------------------------------' |
 | '--------------------' |              | .--------------------------------. |
 | .--------------------. |              | | 作者                           | |
 | | 作者               | | [STATUS_ROW] | | Alice                          | |
 | | Alice              | |              | '--------------------------------' |
 | '--------------------' |              |   ... (版本/简介/适用设备/ID) ...  |
 |   ... (其余 4 项) ...  |              |  .------------------------------.  |
 |    .--------------.    |              |  |       ( del-wide.bin )       |  |
 |    |  (del.bin)   |    | [150x78 Img] |  '------------------------------'  |
  \   '--------------'   /               +------------------------------------+
   '--------------------'
```

---

### 4.4 混搭微调页 (`ROUTE_MIX_MATCH`)

1. **顶部标题栏**：`NAVIGATION_HEADER`，`title = "混搭微调"`，`show_back = 1`。
2. **列表构造与置顶规则（100% 保持原逻辑）**：
   - 包含 5 个固定功能项：
     1. `feature:launcher` → `"启动器背景"`
     2. `feature:controlCenter` → `"控制中心背景"`
     3. `feature:notifications` → `"通知中心背景"`
     4. `feature:widgets` → `"快捷组件背景"`
     5. `feature:calendar` → `"日历样式"`
   - 以及所有已安装资源包注册的具体替换路径（按路径字典序排列，遵循同包内最长前缀遮蔽规则）。
   - **稳定分区置顶**：所有当前选择不为 `"默认"` (`@default`) 的项排在列表前部，其余 `"默认"` 项排在后部，同组内保持原有相对顺序。
3. **虚拟滚动与位置恢复**：
   - 使用 `CANOPUS_UI_NODE_SCROLL`（`VIRTUAL_CONTENT | CLIP`），可见窗口 `WINDOW = 10`，每项为一个原生 `ACTION_ROW`（`label = 功能名或路径`，`detail = "默认" | "系统" | "模糊" | 主题名称`）。
   - 进入 `ROUTE_MIX_MATCH_SELECT` 前保存当前 `mix_scroll_y`，返回时通过 `visual.scroll_y` 原位恢复。

---

### 4.5 选择来源页 (`ROUTE_MIX_MATCH_SELECT`) —— 含「默认」与「系统」图标预览

1. **顶部标题栏**：`NAVIGATION_HEADER`，功能项显示对应名称（如 `"控制中心背景"`），普通资源路径显示 `"选择来源"`，`show_back = 1`。
2. **「系统」(`@system`) 与「默认」(`@default`) 真实图标预览机制**：
   - **`rh_wrapper` 原厂直通前缀 `".rh_sys/"`**：
     - 在 `src/module.c` 的 `rh_wrapper(void *d, const char *p, int m)` 中，若 `p` 以 `".rh_sys/"` 开头（对应 LVGL 路径 `"/.rh_sys/<real_path>"`），直接跳过 `active_snapshot` 与日历重定向，调用 `S.original(d, p + 8, m)` 打开固件原厂资源。
     - `"/.rh_sys/..."` 在 LVGL 图片缓存中拥有独立缓存键，既不会命中已被主题替换的同名缓存，也不会污染系统正常渲染。
   - **各选项预览图解析规则**：
     - **「系统」(`@system`)**：
       - 普通 `.bin` 路径（如 `/system/images/battery.bin`）：预检原厂文件可打开后，预览图设为 `"/.rh_sys/system/images/battery.bin"`。
       - 快应用图标（`@quickapp-icon/<pkg>`）：通过 `rh_platform_quickapp_icon_path` 解析出快应用原始图标路径 `<icon_path>`，预览图设为 `"/.rh_sys<icon_path>"`。
       - 日历样式（`feature:calendar`）：预览图设为 `"/.rh_sys" RH_CALENDAR_BACKGROUND`。
       - 其余四个背景功能项（`launcher` / `controlCenter` / `notifications` / `widgets`）：系统默认无单一静态图，保持紧凑无图单选行。
     - **四项背景功能（`launcher` / `controlCenter` / `notifications` / `widgets`）**：所有选项均使用紧凑无图单选行，仅显示名称和选中状态；不解析、探测或加载背景原图预览，避免大图解码开销。背景选择与重载应用保持原有行为。以下「默认」和候选主题预览规则仅适用于普通资源、快应用图标及日历样式。
     - **「默认」(`@default`)**：
       - 按当前 `resource-order.json` 顺序，在 `@system` 分界线以上的激活主题中查找第一个接管该路径（或声明了该功能项 `mode == "image"`）的主题；若找到，则预览图设为该胜出主题的 `.bin` 文件路径；若 `@system` 以上无主题接管（或胜出模式为 `system`），则回退使用与「系统」选项相同的 `"/.rh_sys/..."` 预览图（若胜出模式为 `blur` 则无预览图）。
     - **「模糊」(`@blur`)**：仅在非日历的 4 个背景功能项中出现，无预览图，使用紧凑单选行。
     - **各候选主题**：按 `resource-order.json` 优先级排序，普通 `.bin` 资源和日历底图展示对应主题包内的 `.bin` 预览图。四项背景功能不展示预览图。
3. **卡片与组件布局（对齐快应用原页）**：
   - **所有选项共用一张卡片**：底色 `#262626`、圆角 `24px`、左右内缩 `14/13`，行间 `8px`，与 `mix-match.ux` 的 `.resource-choice` / `.choice-card` 一致。
   - **带预览图项（高 `168px`）**：`CANOPUS_UI_NODE_SECTION` 作为卡片（`y` 偏移由提交后 `TRANSLATE` 施加），内含居中 `96×96` `CANOPUS_UI_NODE_IMAGE`（`image_fit = CANOPUS_UI_IMAGE_FIT_CONTAIN`，顶部偏移 `8`）+ 底部高 `48px` 的 `CANOPUS_UI_NODE_RADIO_ROW`。
   - **无预览图紧凑项（高 `76px`）**：`CANOPUS_UI_NODE_RADIO_ROW` 自身就是卡片（圆角 `24px`、底色 `#262626`、内边距 `14/13`）。
   - **选中标识**：右侧 `32px` 的 `check.bin` / `uncheck.bin`（由 `manager/src/common/images/true.png`、`false.png` 转出，走 `[resources]` 包资源），与快应用的 32px 勾选图一致；`RADIO_ROW` 用 `CANOPUS_UI_VARIANT_PLAIN` 关掉宿主合成的 `(●)`/`(○)` 文本，但保留其 44px 尾部列以便图标对齐。
   - **虚拟窗口容量**：带图项每项占 5 个节点（`SECTION` + `LIST` 包装层 + `IMAGE` + `RADIO_ROW` + 勾选图标），紧凑项占 2 个（`RADIO_ROW` + 图标）。位置表按 `2 × WINDOW + 4` 预留，供卡片与图标各自走一次提交后 `TRANSLATE`。点击任意项立即写入 `resource-overrides.json` 并刷新选中态。
   - **预览图点击承载**：裸 `IMAGE` 不可点击，预览图仿 §3.2 底部图片按钮套一层 `CANOPUS_UI_NODE_LIST` 包装层承载 `EV_CHOOSE+i`（松开且位移未超 `8px` 触发选择）；包装层、裸图片子节点与勾选图标的固件命中只报 `key`，由 `key → EV_CHOOSE+index` 兜底映射，整卡无死区，与快应用 `.choice-row` 整卡可点一致。

```text
ROUTE_MIX_MATCH (Band 11)                ROUTE_MIX_MATCH_SELECT (Band 11)
   .--------------------.                   .--------------------.
  /        08:03         \                 /        08:03         \
 |      < 混搭微调        |               |      < 选择来源        |
 |------------------------|               |------------------------|
 | .--------------------. |               | .--------------------. |
 | | 控制中心背景     > | | [ACTION_ROW]  | |    +----------+    | |
 | | 模糊               | |               | |    |  96x96   |    | | [按排序胜出预览图]
 | '--------------------' |               | |    +----------+    | |  (主题图或/.rh_sys/)
 | .--------------------. |               | | (●) 默认           | | [RADIO_ROW]
 | | /system/.../a.bin> | | [ACTION_ROW]  | '--------------------' |
 | | 霓虹主题           | |               | .--------------------. |
 | '--------------------' |               | |    +----------+    | |
 | .--------------------. |               | |    |  96x96   |    | | [/.rh_sys/ 原厂图]
 | | 启动器背景       > | | [ACTION_ROW]  | |    +----------+    | |  绕过Hook直读ROMFS
 | | 默认               | |               | | (○) 系统           | | [RADIO_ROW]
  \  '------------------' /               | '--------------------' |
   '--------------------'                 | .--------------------. |
                                          | |    +----------+    | |
                                          | |    |  96x96   |    | | [themes/<id>/... ]
                                          | |    +----------+    | |
                                          | | (○) 霓虹主题       | | [RADIO_ROW]
                                           \ '------------------' /
                                            '--------------------'
```

---

### 4.6 删除确认页 (`ROUTE_DELETE`)

1. **进入与退出**：资源详情页底部 `del*.bin` 单击 `navigate(ROUTE_DELETE, 0)`；返回头/返回手势回资源详情（`navigate(ROUTE_DETAIL, 0)`）。删除成功回资源管理页（`navigate(ROUTE_RESOURCES, 1)`），条目消失就是结果本身，不需要额外文案。
2. **顶部标题栏**：`NAVIGATION_HEADER`，`title = "删除资源"`，`show_back = 1`。
3. **内容**：
   - `STATUS_ROW`（`title = "主题包"`，副标题 `manifest.name`，缺省回退 `themeId`），宽 `row_width`，高 `96`，`y = 0`。
   - 说明段 `TEXT`（20px，最多 4 行，自 `y = 110` 起）：普通态 `"删除后，资源包文件与索引将一并移除，且无法恢复。"`（`0xC5CBD5`）；受保护态 `"正在使用中，无法删除。请先在「混搭微调」中改回「默认」。"`（`0xDA593C`）；预检遇到临时错误（目录忙、索引暂不可读）时改为 `"无法确认该资源包的使用状态，删除可能失败。"` 并保留按钮；事务失败态填 `rh_manager_data_error()`，缺省 `"删除失败，请稍后重试。"`。
   - 确认动作是 `ACTION_ROW`（宽 `row_width`，高 `96`，`y = content_height - 106`），不是详情页那张 `del*.bin` 位图按钮；位图涂不上前景色，也撑不起置灰态。
4. **受保护判定**：进入页面即调只读的 `rh_manager_data_delete_check()`；`RH_MANAGER_ERR_ACTIVE` 表示该 `themeId` 仍被 `mappings.tsv`、`resource-active-generations.json` 的 `protectedThemes` 或 `calendar.cfg` 引用。判断结果只做展示，删除事务在自己的保护区里重新校验。
5. **禁用态**：正常态 `variant = CANOPUS_UI_VARIANT_DESTRUCTIVE`（host 把标题映射成 `COLOR_DANGER = 0xFF5C5C`）；被引用时 `enabled = 0` + `foreground = CANOPUS_UI_COLOR_DISABLED`（`0x666666`）+ `style.opacity = 500`。
   - 透明度依赖目标绑定 `FW_UI_STYLE_OPA`，否则 `host_validate` 以 `CANOPUS_UI_ERR_UNSUPPORTED` 拒绝整次提交。三个出货目标现已全部具备：`.155`、`.179` 由 Band11 生成器输出，`.043` 由 `tools/canopus-core/src/target_config.rs` 的 `.043` 分支输出，证据为 `EVID-UI-1043-OPACITY-001`（固件 style prop 95 的 setter 与渲染乘法读取均为静态取证）。
   - `enabled = 0` 只拦事件（runtime 返回 `CANOPUS_UI_ERR_DISABLED`），外观必须自己写；`action_row` 拒绝 `event_id = 0`，所以保留 `EV_DELETE` 作为防御，处理器再兜一次 `RH_MANAGER_ERR_ACTIVE` 判断。
   - `style.opacity = 0` 表示继承，显式全透明需要同时置 `CANOPUS_UI_VISUAL_FLAG_OPACITY`；这里用非零值，不必置位。
6. **事务失败留在本页**：把说明段换成原因，按钮保留供重试；不跳到页面级错误标签，否则用户失去重试与返回两个入口。
7. **预算**：`NAVIGATION_PAGE` + `NAVIGATION_HEADER` 2 节点，`STATUS_ROW` 1，`ACTION_ROW` 1，说明 1–4，共 `≤ 8 / 32` 节点；字符串约 `200B / 1536B`；两屏内容均不需滚动。

### 4.7 传输状态横幅（资源管理页内嵌，无独立传输页）

接收链路在模块层独立运行（`rh_connect_start` / `rh_connect_poll`），不需要前台页面，原「主题传输」页已删除；传输进度改为钉在资源管理页列表顶部的横幅卡片展示。横幅不是列表行：不进入虚拟滚动窗口、不参与拖拽排序的 `KEY_ROW` 网格与 `EV_ORDER` 映射，仅压缩列表视口一个 `BANNER_H (96px) + 8px` 间距。

1. **出现规则**：仅 `ROUTE_RESOURCES` 且 `phase` 为 `RECEIVING / VERIFYING / INSTALLING`，或 `ERROR / CANCELED` 且未被点按消除；`IDLE` 与 `INSTALLED` 不渲染横幅。没有前台传输，就没有「等待连接 / 等待文件」常驻页。
2. **阶段视觉映射（对齐 `canopusconnect_proto.md` §4 & §8）**：
   | 内部 `phase` | 第一行 (`TEXT TITLE`, `font_px=22`) | 第二行 |
   |---|---|---|
   | `RECEIVING` (1) | `"正在接收 · {percent}%"` | 原生进度条 `PROGRESS (0..100)` |
   | `VERIFYING` (2) | `"正在校验完整性"` | 原生进度条 `PROGRESS (100)` |
   | `INSTALLING` (3) | `"正在解压并登记"` | 原生进度条 `PROGRESS (100)` |
   | `ERROR` (5) | `"传输中断"` | 错误码对应说明（`transfer_error` 映射，左对齐两行换行，`font_px=20`） |
   | `CANCELED` (6) | `"传输中断"` | `"任务已取消"` |

3. **`INSTALLED` (4)**：横幅消失并刷新资源列表，让新包立即出现。包名揭示（`"{包名}已安装"`）随 TOAST API 移除而取消；协议握手不携带名称，接收/校验阶段设备端不存在名字。
4. **错误常驻与消除**：`ERROR / CANCELED` 横幅常驻（后台传输失败本来就无人围观，不能一闪而过），点按横幅卡片（`EV_DISMISS`，点击语义为 slop 内松开，与底部图片按钮一致）消除；任意后续 `phase` 跳变自动恢复横幅。
5. **无常亮锁**：传输无需用户盯屏，`CANOPUS_WAKELOCK_SCREEN` 随传输页一并移除；接收进度由接收器持久化与跨重启续传保证，息屏无影响。

```text
Band 11 (.155, 212x520)                 Band 10 Pro (.043, 336x480)
   .--------------------.               +------------------------------------+
  /        08:03         \              | < 资源管理                   08:05 |
 |      < 资源管理        |              +------------------------------------+
 |------------------------|              | .--------------------------------. |
 | .====================. |              | | 正在接收 · 68%                   | |
 | || 正在接收 · 68%      || |            | | [===============>    ]           | |
 | || [========>    ]    || |            | '--------------------------------' |
 | '====================' |              | .--------------------------------. |
 | .--------------------. |              | | 霓虹主题                     > | |
 | | 霓虹主题         > | |              | | Alice · v1.2.0 · 赛博风格图标  | |
  \   (scroll list)      /               \  (scroll list)                    /
   '--------------------'                 +------------------------------------+
```

---

## 5. 代码模块划分与分步实施计划

### Step 1：模块契约升级、原厂直通前缀与内置 UI 资源
- **`Canopus.toml` & `src/module.c`**：
  - `canopus_abi` 升至 `"2"`，描述符版本升至 `CANOPUS_ABI_MAJOR (2)` / `CANOPUS_ABI_MINOR (0)`。
  - 在 `rh_wrapper` 中添加 8 字节 `".rh_sys/"` 直通分支，供混搭微调预览原厂资源。
  - 暴露内部状态读取 `rh_module_get_status_snapshot` 与内部重载触发 `rh_module_request_reload` 及完成回调钩子。
- **`src/manager_assets.c`**：
  - 将 `status-running.png`、`status-config-error.png`、`status-unresponsive.png`、`reload.png`、`reload-wide.png`、`del.png`、`del-wide.png`、`none.png`、`true.png`、`false.png`、`logo.png` 转为 LVGL v9 `.bin`，经 `Canopus.toml` 的 `[resources]` 作为 `.canopus` 包内资源（`resources/common/<name>.bin`）发布；安装时由包安装器解压到模块私有目录 `/data/canopus/m.corona/<name>.bin`。模块 ELF 不携带任何图片字节；运行时仅按名校验已解压文件的大小并返回绝对路径，图片由 UI 图片节点懒加载。`true.png` / `false.png` 分别生成 `check.bin` / `uncheck.bin`，供「选择来源」的选中标识使用。

### Step 2：C 语言移植 Manager 数据与配置生成层
- **`src/manager_data.c`**：共享数据模型已移植排序、资源包元数据、混搭来源解析、配置生成、删除保护与旧数据导入。生产模型将小对象合并到可回收的 16 KiB 块中；读取文件和构造目录时显式传播存储或预算错误。
- **原生存储与发布**：`get_data_path` 必须对应 `/data/canopus/m.corona/`；每次重载先写完整 `active/<generation>/{mappings.tsv,backgrounds.cfg,calendar.cfg}`，再由内部事务验证并发布 `active-config` 指针。指针内容为 `active/<非零规范 uint32>\n`。旧、新资源引用在刷新完成前共同受保护；只有匹配成功通知才能缩小保护范围。
- **清理能力**：FS v2 提供 `rmdir`。默认清理逐文件调用 `fs.remove`，目录句柄关闭后按后序删除空目录，主题卸载覆盖该主题全部安装代次；只有先通过服务版本检查才访问新增接口。FS v1 或接口缺失时保留空目录兼容。文件、目录删除、索引写入或清理失败均返回错误，不把失败显示为成功；旧活动配置中的未知文件保留。
- **旧存储导入**：保留旧快应用数据，首次全量复制到原生根后最后发布原生索引；原生索引已存在时合并尚未登记的旧包，同名 ID 和已有原生配置保留。配置文件先复制到临时文件再 rename，失败可重试；完成标记 `resource-legacy-imported` 避免删除后复活，后续旧目录变化不持续同步。导入不代表已应用，仍需内部重载事务。框架目录枚举须按固件 `d_name +1` 解码，旧版 `+2` 会截掉首字节并阻断导入。新传输安装适配已接入 `manager_data.c`，实现边界见 CanopusConnect 实现文档。

### Step 3：实现 6 路由原生 UI 视图与事件状态机
- **`src/manager_ui.c`**：
  - 通过 `publish_native_app_stage` 的两个独立阶段登记 `ctx->ui->set_entry` 与 `ctx->launcher->register_app`；登记接口返回正数 app ID，模块对外仍将成功规范为 0。
  - 实现 `ROUTE_INDEX`、`ROUTE_RESOURCES`、`ROUTE_RESOURCE_DETAIL`、`ROUTE_MIX_MATCH`、`ROUTE_MIX_MATCH_SELECT`、`ROUTE_DELETE` 的树构建与增量提交；传输进度以横幅卡片形式内嵌 `ROUTE_RESOURCES`（见 §4.7），不占用独立路由。
  - 错误页保留标题返回，内部返回由模块消费；替换页面提交失败时恢复原路由、条目数和错误信息，连续返回不会因不可见的首页状态退出应用。仅首页的 `on_back` 允许框架关闭页面。配套 Supervisor 在 `.155` / `.179` / `.043` 的模块页面绑定系统右滑完成回调，复用相同层级；接管后阻止固件再次关闭页面，普通手势中间阶段不退栈。精确 ABI 静态证据见 Canopus `docs/investigations/native-system-back.md`。
  - 实现长按拖拽排序（`CAPTURE` + `TRANSLATE` + 边缘 `SCROLL_TO`）、删除确认页跳转与事务、虚拟滚动窗口切换与 `scroll_y` 记忆恢复。

### Step 4：Host 测试与 Mirage 模拟器双端验收（`.043` & `.155`）
1. **Host ASan/UBSan 测试 (`tests/test_manager_native.c`)**：
   - 验证 manifest 解析、排序归一化、混搭覆盖、`@default`/`@system` 预览路径计算、`.rh_sys/` 直通打开、活动包删除保护与 `rh_manager_data_delete_check` 预检、禁用 `ACTION_ROW` 的 opacity/前景色组合、6 路由 UI 树节点数/字符串容量边界（`<= 32` 节点、`<= 1536B`）及拖拽/删除确认事件状态机。
2. **Mirage 模拟器端到端验证 (`.043` Band 10 Pro & `.155` Band 11)**：
   - 使用隔离的 `MIRAGE_LIBRARY` 与生产安装器加载 Supervisor 并安装 `corona-0.3.0.canopus`，预置测试主题包。
   - 通过自动化触摸脚本依次打开并截图验收：
     - **首页**：验证状态卡颜色、圆角裁剪、字号、右下角装饰图，以及底部 `reload*.bin` 按钮触发内部重载后的结果层：进行中 / 成功（含变化条数）/ 保护记录未存 / 五种失败文案与琥珀底色，并在驻留到期后回到真实模块状态。
     - **资源管理 & 资源详情**：验证原生双行列表、长按拖拽排序、详情页 6 项 `STATUS_ROW` 及 `del*.bin` 进入删除确认页的受保护态。
     - **混搭微调 & 选择来源**：验证已微调项置顶、「默认」/「系统」/「主题包」的 `96×96 CONTAIN` 真实 `.bin` 图标预览与单选切换、返回后滚动位置恢复。
     - **传输横幅**：通过测试钩子（长按首页卡片，每次推进一个 phase）驱动 `RECEIVING / VERIFYING / INSTALLING / INSTALLED / ERROR / CANCELED` 状态切换，在资源管理页验证横幅文案、原生 `PROGRESS` 进度条与错误横幅点按消除；`INSTALLED` 只验证列表刷新（无安装完成提示）；`IDLE` 不渲染横幅（无「等待连接」页）。
