# Xiaomi Band 11 4.100.177

Experimental launcher backgrounds: [configuration, static evidence and device test guide](launcher-background.md). The new background-enabled artifact is NOT_PROBED; prior user feedback below applies only to the earlier build.

状态：STATIC_RECOVERED / build verified / device USER_REPORTED_PASS。
用户反馈：使用提供的 AstroBox Ed25519 证书签名的 .177 表盘安装包实机验证通过。未提供逐功能测试记录；此反馈不代表全部原生生命周期、故障注入或 GPU 安全验证通过。

AP SHA256：`ff74c6f467529963b6afd40669437d8ec39a02e48c10d0893739847b3ba3dc2b`。
源证据：`../../mifw/analysis-q66-177/177/vela_ap.bin`（相对仓库根），及相邻 `Canopus-Private/targets/xiaomi-band-11-4.100.177/evidence/fw-match/port-155-to-177.json`、`interior-global-review.json`、`critical-pseudocode.json`、`emulation-addresses.json`。完整归一化函数体/有序引用匹配是静态证据，不是语义或硬件安全证明；无全局地址差值迁移。

`include/resource_hook_target_177.h` 独立列出平台、图像缓存/owner refresh、字体事务、calendar 的精确地址；默认 .139 和现存 .155/.043 分支保持。`address-map.json` 保存生成时采用的逐项对应关系（包含框架测试地址，非全部直接调用许可）。函数对应来自 reviewed pack。独立读取 AP 的 literal witnesses：horizontal/vertical accessor `0xc38164c/0xc38166c` 指向 uikit `0x200bd08c`，image-create helper `0xc3ae130` 指向 class `0x2ca12d74`，calendar publish `0xc547848` 指向 slot `0x200c5d88`；POSIX/cache interior slots沿 reviewed uikit 对象布局访问。QuickApp service slot `0x20084e74`、active map `0x20084e6c` 由 critical consumer/install 路径建立；只直接调用 package lookup `0xc69fa6b`，service slot 3 `0x2ca3b4c0` 的 AP 内容独立核对。

验证：

```sh
python3 tests/test_target_177.py
CC=/usr/bin/clang CLANG=/usr/bin/clang sh scripts/build.sh xiaomi-band-11-4.100.177
```

本次三项专项测试通过（全部直接调用常量对应 reviewed pack，manifest/hash identity，精确 AP literal）；build.sh 的既有 host suites 通过，ARM 编译与未修改的 Canopus verifier PASS：19 sections / 0 undefined / 831 relocations。未签名 ELF SHA256 `8f7a8a5c365695ab49db5ad39031c47a461319027639d524e2f9bf4af5a61334`（加入互斥宏 guard 不改变有效目标代码）。

签名打包：已使用用户提供的 AstroBox 证书生成 .177 表盘安装包，签名、固件指纹和 Supervisor 公钥匹配校验通过；产物不含私钥。用户随后反馈实机验证通过。

局限：既有 firmware_support 的 .139/.155 emulation suites 尚未扩展到 .177，不能宣称 .177 原生指令生命周期/故障注入测试全部通过。字体/图像/日历/QuickApp 的逐项覆盖范围未记录，GPU 延迟生命周期及重复激活仍未独立验证。工具接受显式 .177；默认已发布双 Band11 delivery/watchface 组合不自动扩大。打包脚本仍将新产物标记为 NOT_PROBED，避免将当前用户反馈自动应用于未来构建。
