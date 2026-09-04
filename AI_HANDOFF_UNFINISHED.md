# KairosBoot 开发交接（2026-08-28 20:19 CST）

> 本文是当前唯一的继续开发入口，替代 2026-08-27 的旧交接快照。
>
> 用户最新流程要求：**不要每完成一个切片就跑一次 CI**。先完成全部本地集成与复审，只在最终聚合提交上做一次 Release 全量本地检查、一次 push、一次 exact-head 远端 CI。

## 0. 立即接手结论

- 不要从主工作区 `main` 开发；继续使用聚合 worktree：
  `/private/tmp/kairosboot-worktrees/root-compat-aggregate-v1`
- 聚合分支：`codex/compat-aggregate-v1`
- 当前 clean HEAD：`187b612ed3f8679cf5eca4ff1417215d2e67b1e3`
- 尚未 push，本轮没有触发远端 CI。
- 本地 `origin/main`：`162b304b17a0bbcfe1ab748a91970886db94c94c`
- 本地 `origin/codex/compat-aggregate-v1`：`38a8dc4c7899f368893c80c882eaf4211a8cf209`
- 聚合相对 `origin/main` ahead 50；相对远端聚合分支 ahead 35。
- 最后已知 PR：[#60](https://github.com/EurekaRaider/KairosBoot/pull/60)。继续前可只读回查，但在全部本地工作完成前不要 push。
- 用户已明确授权最终 exact-head 全绿后使用 `gh pr merge --admin`；这不是跳过测试或复审的授权。

主工作区 `/Users/williamji/Documents/GitHub/KairosBoot` 仍在 `main@162b304`，包含用户文件：

```text
?? .artemis/
?? AI_HANDOFF_UNFINISHED.md
```

保留 `.artemis/`，不要提交、删除或清理。

## 1. 聚合分支已完成内容

当前聚合已经包含此前全部兼容/API/性能/Release 切片，重点包括：

- C11、C++23 header-only wrapper、C# net48/net10、CLI 四级 API。
- USB/TCP/UDP、typed primitives、update/flashall、fleet run/cancel/report。
- controller-aware DATA permits、32 actor 生命周期、生产 fastbootd reconnect、failure isolation。
- slot policy、sparse limit、vendor ID、verbose、force/filesystem、format、wipe-super、signature、AVB flags。
- Android boot v0-v4 raw image 构建、vendor_boot v3/v4 repack。
- dynamic super optimizer 与 fail-closed/fallback 修复。
- AOSP Platform-Tools 37.0.1 differential runner、exact-evidence provenance 门禁。
- Release-mode 六平台资产、NuGet、symbols/source/SBOM/provenance/libusb NOTICE、SIGNING_MODE=off 门禁。
- C ABI sized initializer 修复，当前冻结清单为 **128 个 `kb_*` 导出**。

最新关键提交（均已在 aggregate）：

```text
187b612 test(compat): require expanded official differential
d33c2ac compat: close raw boot differential gaps
1762fd5 compat: match fastboot default flash files
f87e32c fix: match AOSP raw boot image bytes
0e0e8e4 fix(abi): add size-aware public initializers
3cf5c15 compat: report flash and boot differential gaps
9d6d69f fix: reconnect public USB updates through fastbootd
886fb5b test(compat): bind claims to exact evidence
079ebd1 fix: fail safe for unsupported super layouts
75eb40d test: harden final release contracts
```

当前生成清单：

```text
implemented=79
intentional-deviation=15
missing=0
partial=25                  # 均为可选 image 条目
requiredGaps=[]
official evidence=0         # 当前是正确结果，见第 3 节
claimCompatibility=false
```

不要为了得到漂亮数字直接改 generated inventory；唯一 source-of-truth 是 capability evidence、compatibility contract、真实 capture 和 generator。

## 2. 第一优先级：完成未提交的 public reconnect 复审修复

工作树：

```text
/private/tmp/kairosboot-worktrees/public-reconnect-review-fixes-v1
branch: agent/compatibility/public-reconnect-review-fixes-v1
base:   0e0e8e45fb8c045048384e3cceef7cc1e043c565
```

该 worktree 有未提交修改，不能丢弃：

```text
 M CMakeLists.txt
 M abi/kairosboot-v1.json
 M include/kairosboot/kairosboot.h
 M src/fastboot/libusb_reconnect_adapters.cpp
 M src/fleet/device_preflight.cpp
 M src/transport/usb_fastboot.cpp
 M src/transport/usb_fastboot.hpp
 M tests/c_api_test.c
 M tests/fastboot/libusb_reconnect_adapters_tests.cpp
 M tests/fleet/device_preflight_tests.cpp
 M tests/libusb_runtime/test_main.cpp
?? tests/c_api_legacy_initializer_test.c
```

正在修的复审项：

1. **initializer source compatibility（P2）**
   - 已有 7 个 `*_init_sized(ptr, struct_size)` 和 7 个 legacy 一参数符号。
   - 需要把 function-like 宏改成 `static inline` current-size wrapper + object-like alias。
   - 普通调用、`&kb_*_init` 和 parenthesized function designator 都必须初始化当前完整结构。
   - 只有显式定义 `KAIROSBOOT_DISABLE_SIZED_INITIALIZER_MACROS` 才能访问 legacy V1-prefix symbol。
   - 测试必须同时证明 current source 调用得到 current `struct_size`，legacy 小结构 canary 不被越界写。

2. **preflight transfer certainty（P2）**
   - `FastbootDevicePreflightProbe` 必须聚合 product/mode 两次查询的 certainty。
   - exception/catch 不能把已经发送或 `PartialOrUnknown` 的查询错误降级成 `NotTransferred` / `KB_TRANSFER_NOT_SENT`。

3. **absolute operation deadline（P1）**
   - `9d6d69f` 的 public USB reconnect 路径绕开了原 `UpdateDeadlineTransport`。
   - 打开时计算一次 I/O timeout 不等于整次操作绝对截止时间。
   - 初始 session 与重连后的 replacement session，每次 I/O 都必须按同一个 absolute deadline 截断。
   - preflight/slot/super optimization 已消耗的时间必须计入；不能只在 task 边界检查。
   - 增加跨 preflight、多 task、replacement session 的确定性 timeout 测试。

4. 更新 public header 中“fastbootd transition 不支持”的过时说明。

接手动作：

```sh
cd /private/tmp/kairosboot-worktrees/public-reconnect-review-fixes-v1
git diff --check
git diff
```

先审完当前 diff，再完成实现和定向 Release 测试。形成一个可审查 commit 后，切到 aggregate cherry-pick。因为该分支基于 `0e0e8e4`，合入最新 `187b612` 时保留 boot/flash differential 的后续提交。

建议定向验证：

```sh
cmake --build build-release --parallel 6
ctest --test-dir build-release --output-on-failure -R \
  '^(c_api|abi_tooling|abi_symbols|device_preflight|libusb_reconnect_adapters|libusb_runtime|primitive_update_device|c_fleet_run)$'
python3 bindings/dotnet/KairosBoot.ContractTests/run_update_shim_tests.py --framework net10.0
```

需要 loopback 的测试在受限沙箱中会因 `bind: Operation not permitted` 失败；只把相应测试放到沙箱外重跑，不要重跑整个 suite。

## 3. 第二优先级：在最终产品 HEAD 上重新生成 official capture

`boot-diff-v1` 目前有 3 个未提交 evidence 文件：

```text
/private/tmp/kairosboot-worktrees/boot-diff-v1
branch: agent/compatibility/boot-diff-v1
HEAD:   304bcc414569de6bcdecfee2aaf6015c3ad1b9de

 M compat/evidence/official-differential-37.0.1-darwin/aosp-fastboot-normalized-trace.json
 M compat/evidence/official-differential-37.0.1-darwin/kairosboot-normalized-trace.json
 M compat/evidence/official-differential-37.0.1-darwin/official-capture-metadata.json
```

**这 3 个未提交文件是过期 capture，不要提交、不要 cherry-pick。**

原因：它们绑定 `boot-diff` 分支的源码提交，而最终 aggregate 还包含 `0e0e8e4` sized ABI 和即将完成的 reconnect/deadline 修复。原 capture commit 不会是最终产品 HEAD 的有效 exact/evidence-only ancestor。

已经合入 aggregate 的真实修复：

- `f87e32c`：v0 raw boot bytes 与冻结 AOSP `mkbootimg` 对齐。
  - 删除 KairosBoot 额外写入的旧 SHA-1 id。
  - 恢复官方 v0 模板的 `dtb_addr=0x01100000` bytes。
- `1762fd5`：实现 AOSP `flash <partition>` 默认文件语义。
  - 只匹配冻结 AOSP image nickname。
  - 文件来自 `$ANDROID_PRODUCT_OUT/<mapped-file>`，不猜 `<partition>.img`。
  - slot 展开在文件解析后进行；vendor_boot 使用 `vendor_boot.img`。
- `d33c2ac`：恢复真实 implemented/required-gap 状态。
- `187b612`：differential catalog 扩展到 19 场景。

子代理已用真实 Platform-Tools 37.0.1 跑通 19/19，但还没有针对最终 aggregate 持久化合法 evidence。

最终 capture 必须在第 2 节修复合入、aggregate clean 后执行：

```sh
cd /private/tmp/kairosboot-worktrees/root-compat-aggregate-v1

cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIROSBOOT_LIBUSB_ROOT=/private/tmp/kairosboot-libusb-1.0.30-macos-arm64 \
  -DFETCHCONTENT_SOURCE_DIR_BOOST=/private/tmp/kairosboot-r10-build/_deps/boost-src \
  -DFETCHCONTENT_SOURCE_DIR_KAIROSBOOT_MINIZ_SOURCE=/private/tmp/kairosboot-r10-build/_deps/kairosboot_miniz_source-src \
  -DFETCHCONTENT_SOURCE_DIR_KAIROSBOOT_YAML_CPP=/private/tmp/kairosboot-cli-update-build/_deps/kairosboot_yaml_cpp-src
cmake --build build-release --parallel 6

python3 scripts/run_official_fastboot_differential.py \
  --repository-root . \
  --fastboot /private/tmp/kairosboot-official-platform-tools-37.0.1/platform-tools/fastboot \
  --kairosboot build-release/kairosboot \
  --output-dir compat/evidence/official-differential-37.0.1-darwin \
  --require

python3 scripts/generate_compatibility_inventory.py
python3 scripts/generate_compatibility_inventory.py --check
```

预期：19 matched scenarios，约 22/87 required entries 有 official evidence，coverage 仍为 partial，`claimCompatibility=false`。

capture 后只允许把 evidence/generated 路径作为单独 evidence commit；provenance 规则允许 exact product HEAD 或其 evidence-only descendant。任何产品、API、构建文件再变化都必须重新 capture。

## 4. 已有验证证据与边界

本轮曾在 aggregate 上完成一次 Release 构建：

- 62 个 CTest 中 56 个在沙箱内通过。
- 6 个仅因沙箱禁止绑定 `127.0.0.1` 失败，随后沙箱外定向重跑 6/6 通过。
- 该结果发生在后续 reconnect/ABI/boot 修复之前，不能当作最终 HEAD 的全量证明。

后续定向证据：

- public reconnect/super/CLI：7/7 Release tests 通过。
- compatibility tooling：15/15 通过。
- sized initializer aggregate：`c_api`、`cxx_api`、`c_fleet_run`、`abi_tooling`、`abi_symbols` 通过。
- ABI：128 个 `kb_*` symbols。
- net10 scripted shim：439 checks；net48 Release compile：0 warning / 0 error。
- raw boot/default flash：真实 37.0.1 定向差分通过；19-scene 临时差分 19/19。

由于第 2 节仍有未提交产品代码，最终 aggregate 尚未做 exact-head 全量 Release 检查。

## 5. 全部本地改动完成后的唯一一次最终检查

按此顺序执行，不要每修一个小问题就重跑全套：

1. 第 2 节修复 commit 合入 aggregate。
2. 在新的 clean 产品 HEAD 上完成第 3 节 19-scene official capture，并提交 evidence-only commit。
3. Release configure/build。
4. `ctest --test-dir build-release --output-on-failure -j 6` 全量一次。
5. 沙箱只阻止 loopback 时，仅在沙箱外重跑失败的网络测试。
6. 执行：

```sh
python3 scripts/check_repository_policy.py
python3 -m unittest tests.tooling.test_release_tools
python3 -m unittest tests.tooling.test_compatibility_inventory
python3 -m unittest tests.compat.test_official_fastboot_differential
python3 scripts/check_abi.py --library build-release/libkairosboot.dylib
python3 scripts/generate_compatibility_inventory.py --check
python3 bindings/dotnet/KairosBoot.ContractTests/run_update_shim_tests.py --framework net10.0
git diff --check
```

7. 独立只读 P1/P2 review，必须无未解决 finding。
8. 确认 aggregate clean 后才 push 一次到 PR #60。
9. 只跑一次 exact-head `ci-required` + `policy`；HIL 是否可跑见第 7 节。
10. exact-head 全绿后才可按用户授权 `--admin` 合并。
11. 合并后验证 `HEAD == origin/main`、ahead/behind `0 0`、clean status。

## 6. GitHub 治理与 Release 状态

2026-08-28 已直接用 GitHub API 回读：

- 只有 `EurekaRaider` collaborator，权限为 admin/push。
- `main` ruleset active：PR required、1 approval、CODEOWNER review、dismiss stale approvals、last-push approval、conversation resolution、linear history、strict `ci-required`/`policy`，并禁止 deletion/non-fast-forward。
- `EurekaRaider` 是 always-bypass actor。
- release tag ruleset 保护 `v*`。
- `github-release` Environment reviewer 是 `EurekaRaider`，`prevent_self_review=false`，deployment policy 只允许 `v*` tag。

仓库 fixture 已同步 strict checks、reviewer 和 `v*` policy。Release workflow/packaging 已覆盖 Release 优化、六平台 SDK/CLI/symbols、net48/net10 六 RID、nupkg/snupkg、source、SHA256SUMS、SPDX、provenance、libusb source/COPYING、UNSIGNED/signing-status，并禁止发布 registry。

## 7. 仍不可在当前机器完成的外部验收

以下不是再写本地单测就能关闭的事项，不能伪造通过：

- `KAIROSBOOT_HIL_ENABLED` 尚未配置，当前没有 exact-head HIL run。
- Windows/Linux/macOS 真实 USB bootloader + fastbootd HIL。
- 单设备 raw bulk ceiling ≥90% 与 AOSP 条件性 ≥10% 对比。
- 32 台、多个 root controller 的 makespan/吞吐/Jain fairness。
- unplug、hub reset、重复 serial、重枚举故障注入。
- 24 小时硬件 soak、无泄漏/串线/持续 RSS 增长。
- Windows/macOS 正式签名与 notarization 凭据。

版本仍为 `0.1.0-dev`。在上述 HIL/性能/soak/最终 exact-head CI 未完成前，不要创建 `v1.0.0` tag 或 Release，不要声称项目已经是生产可用的完整 Fastboot replacement。

## 8. 重要纪律

- 公开 C ABI、根 CMake、`.github/**`、compat schema/generator 由集成 agent 统一处理。
- 不复制 AOSP 原始代码到 MIT 核心；必要复用必须进入 `third_party/aosp/<sha>` 并保留许可证。
- 不通过修改 golden、降低门禁或把 missing 改 intentional deviation 来掩盖失败。
- 只用 Boost 网络实现，不重新引入 Winsock/BSD socket 的产品路径。
- Release 产物必须 `Release`：macOS/Linux `-O3 -DNDEBUG`，Windows `/O2 /DNDEBUG`。
- 保留用户主工作区里的 `.artemis/` 和其他无关文件。
- 不要删除任何 `/private/tmp/kairosboot-worktrees/*`，至少在最终 PR 合并前保留。
