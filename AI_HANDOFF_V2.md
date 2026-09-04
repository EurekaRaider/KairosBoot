# KairosBoot 开发交接 V2（2026-08-28 21:36 CST）

> 本文是当前唯一的继续开发入口，替代 `AI_HANDOFF_UNFINISHED.md`（2026-08-28 20:19 快照）。
> 原《第 2 节 public reconnect 复审修复》与《第 3 节 final HEAD capture》的第一轮执行已完成，
> 但 **capture 需要在新的 HEAD 上重做一次**，原因见第 2 节。
>
> 用户最新流程要求不变：不要每完成一个切片就跑一次 CI。全部本地集成与复审完成后，
> 只做一次 Release 全量本地检查、一次 push、一次 exact-head 远端 CI。

## 0. 立即接手结论

- 聚合 worktree：`/private/tmp/kairosboot-worktrees/root-compat-aggregate-v1`
- 聚合分支：`codex/compat-aggregate-v1`
- 当前 clean HEAD：`573da5998c92019a112ba6a3250daa094fa3abdd`
- 相对 `origin/main`（`162b304b17a0bbcfe1ab748a91970886db94c94c`）ahead 53；
  相对远端聚合分支 ahead 38。尚未 push，本轮没有触发远端 CI。
- 最后已知 PR：[#60](https://github.com/EurekaRaider/KairosBoot/pull/60)。
- 用户已明确授权最终 exact-head 全绿后使用 `gh pr merge --admin`；这不是跳过测试或复审的授权。
- 主工作区 `/Users/williamji/Documents/GitHub/KairosBoot` 仍在 `main@162b304`，
  保留其中未跟踪的 `.artemis/`、`AI_HANDOFF_UNFINISHED.md` 和本文，不要提交、删除或清理。

最新本地提交链（均在 aggregate）：

```text
573da59 test: track AOSP-aligned raw flash wire and 19-scenario capture
5dbd8d0 compat: recapture official differential evidence at product HEAD   # 绑定 21d4ac7，已被 573da59 穿透，见第 2 节
21d4ac7 fix: enforce one absolute USB deadline across reconnect sessions   # 原《第 2 节》修复的 cherry-pick
187b612 test(compat): require expanded official differential
d33c2ac compat: close raw boot differential gaps
1762fd5 compat: match fastboot default flash files
```

## 1. 本轮已完成并验证的内容

### 1.1 public reconnect 复审修复（原交接第 2 节）— 完成

在 `/private/tmp/kairosboot-worktrees/public-reconnect-review-fixes-v1`
（分支 `agent/compatibility/public-reconnect-review-fixes-v1`，HEAD `d632131`，clean）形成 commit
`d632131 fix: enforce one absolute USB deadline across reconnect sessions`，
并干净 cherry-pick 到 aggregate 成为 `21d4ac7`。四项复审项全部落地：

1. **initializer source compatibility（P2）**：function-like 宏改为
   `static inline *_init_current` wrapper + object-like 别名；普通调用、`&kb_*_init`、
   括号 function designator 都初始化当前完整结构；仅显式定义
   `KAIROSBOOT_DISABLE_SIZED_INITIALIZER_MACROS` 才能访问 legacy V1-prefix symbol；
   新增独立 translation unit `tests/c_api_legacy_initializer_test.c` 证明 legacy
   小结构 canary 不被越界写。ABI manifest `headerContractSha256` 已同步
   （`6a6fe966...8fe8`，128 个 `kb_*` symbols 不变）。
2. **preflight transfer certainty（P2）**：`FastbootDevicePreflightProbe::probe` 聚合
   product/mode 两次查询的 certainty（`aggregate_probe_certainty`），exception 路径用
   `probe_in_flight` 判定 `PartialOrUnknown`，不再把已发送字节降级成 `NotTransferred`。
3. **absolute operation deadline（P1）**：`UsbFastbootTransportOptions::absolute_deadline`
   新字段；`UsbFastbootTransport` 的 open/write/read/read_data 全部 clamp 到同一
   steady-clock 绝对截止时间；preflight session opener（`src/fleet/device_preflight.cpp`）
   与 fastbootd reconnect replacement session（`src/fastboot/libusb_reconnect_adapters.cpp`）
   都用 `std::min(已有 deadline, 操作 deadline)` 传播。新增跨 preflight、多 task、
   replacement session 的确定性 timeout 测试。
4. public header 中 fastbootd transition 说明已更新为 fail-closed reconnect 语义。

实现补充（相对原交接时的未提交 diff）：`tests/fastboot/libusb_reconnect_adapters_tests.cpp`
的 `test_preflight_session_retains_absolute_deadline_after_probe` 需要 fake macOS topology
resolver（fleet preflight 的 `normalize_snapshot_device` 强制每个设备恰好一条完整平台
topology）。已加 `topology_functions(fake)` helper + `create_runtime(LibusbFunctions)` 重载，
模式与 `tests/libusb_runtime/test_main.cpp` 的 fake resolver 相同。

**定向验证证据**（在 public-reconnect worktree 与 cherry-pick 后的 aggregate 各跑一遍）：

```text
c_api / abi_tooling / abi_symbols / device_preflight / libusb_reconnect_adapters /
libusb_runtime / primitive_update_device / c_fleet_run → 8/8 Passed (Release)
net10 scripted shim：439 checks，net48 Release compile 0 warning / 0 error
```

### 1.2 final HEAD official capture 第一轮 — 已执行但需重做

在 aggregate `21d4ac7` 上：

```text
python3 scripts/run_official_fastboot_differential.py ... --require
→ PASS: 19/19 TCP/UDP scenarios matched
python3 scripts/generate_compatibility_inventory.py
→ implemented=79, intentional-deviation=15, missing=0, partial=25,
   required-gaps=0, official-evidence=22, claim=false   # 22/87 与预期一致
```

evidence + generated inventory 已提交为 `5dbd8d0`（evidence-only commit，
diff 仅 `compat/evidence/**`、`compat/generated-inventory.json`、`compat/compatibility.yaml`）。

### 1.3 全量 ctest 首跑与两个失败项的根因修复 — 完成

在 `5dbd8d0` 上 `ctest -j 6`：**63 个测试 61 通过，2 失败**（本机 shell 非沙箱，
loopback 可用，之前“6 个沙箱 bind 失败”的情况不适用）。两个失败均已根因分析并在
`573da59` 修复（均为测试期望过期，产品行为正确且有 official trace 背书）：

1. **`cli_contract`**：`tests/cli/scripted_cli_test.py` 的 `flashed_raw_over_tcp`
   期望旧的 `is-userspace → has-slot → is-logical → max-download-size → download → flash`
   前置序列。`f87e32c` 已把 `flash:raw` 对齐 AOSP（`src/kairosboot.cpp` 中
   `aosp_raw_profile` 分支只查 `has-slot:<partition>`），冻结 37.0.1 官方 trace
   （evidence `official-tcp-flash-raw` 场景）确认真实序列就是
   `getvar:has-slot:boot → download → DATA → flash:boot`。已更新测试为该序列。
   注意：普通 `flash`（非 raw）保持 KairosBoot 完整前置序列（`1762fd5` 新增的
   `flashed_default_boot` 测试使用它且通过）——两条路径的期望不要混。
   修复后 `cli_contract` 单独重跑通过。
2. **`compatibility_inventory_tooling`**：
   `test_only_exact_head_capture_counts_as_official_evidence` 硬编码旧 16-scenario
   capture 的 `matchedScenarios=16 / requiredEntriesWithEvidence=19`；fixture 复制仓库
   真实 evidence，新 capture 是 19/22。已更新为 19/22。
   另一个 `test_checked_outputs_are_exact_and_have_no_unknown_state` 期望
   `status == "partial"` 但当时拿到 `not-run` —— 这是 **工作树 dirty 或 capture 绑定
   被穿透时 generator 的正确降级行为**（`_repository_commit` 要求 clean tree），
   不是 bug；recapture 后自然恢复。

## 2. ⚠️ 第一优先级：在新 HEAD 重做 official capture（必做）

`573da59`（test-only）提交在 evidence commit `5dbd8d0` **之后**，而 provenance 规则
（`generate_compatibility_inventory.py` 的 `_capture_matches_current_repository`）只接受
capture 绑定 commit 是当前 HEAD 的 exact match 或 **evidence-only descendant**。
`21d4ac7..573da59` 的 diff 含 tests 路径，所以 5dbd8d0 里绑定的 capture 已失效，
当前 inventory 会降级为 `not-run / 0`。历史里的 `5dbd8d0` 无害，新 evidence commit
直接覆盖同名文件即可。

执行顺序（在 aggregate，工作树当前 clean）：

```sh
cd /private/tmp/kairosboot-worktrees/root-compat-aggregate-v1

# 1) 确认 clean（generator 在 dirty tree 下会得到 not-run/0，这是设计行为）
git status --short   # 必须为空

# 2) 重做 capture（build-release 已配置好，src/ 自 21d4ac7 未变，二进制仍是当前源码的产物；
#    capture 会把 metadata 的 kairosboot.sourceCommit 重绑到 573da59）
python3 scripts/run_official_fastboot_differential.py \
  --repository-root . \
  --fastboot /private/tmp/kairosboot-official-platform-tools-37.0.1/platform-tools/fastboot \
  --kairosboot build-release/kairosboot \
  --output-dir compat/evidence/official-differential-37.0.1-darwin \
  --require

# 预期：PASS 19/19 matched scenarios

# 3) 重新生成 inventory 并提交 evidence-only commit
python3 scripts/generate_compatibility_inventory.py
# 预期：implemented=79, intentional-deviation=15, missing=0, partial=25,
#       required-gaps=0, official-evidence=22, claim=false
python3 scripts/generate_compatibility_inventory.py --check
git add compat/evidence/ compat/generated-inventory.json compat/compatibility.yaml
git commit -m "compat: recapture official differential evidence at product HEAD"

# 4) 验证两个此前失败的测试恢复
ctest --test-dir build-release --output-on-failure \
  -R '^(cli_contract|compatibility_inventory_tooling)$'
# 预期 2/2 通过
```

注意：如果 recapture 前发现 src/、CLI、构建文件又有任何产品级修改，必须先提交产品
commit、再重跑 capture（capture 只能绑定最终产品 HEAD 或其 evidence-only descendant）。

## 3. 第二优先级：唯一一次最终全量检查（recapture 之后）

按原交接第 5 节顺序执行，不要每修一个小问题就重跑全套：

```sh
cd /private/tmp/kairosboot-worktrees/root-compat-aggregate-v1

# Release configure/build（build-release 已用以下参数配置；若删除过需重建）
cmake -S . -B build-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DKAIROSBOOT_LIBUSB_ROOT=/private/tmp/kairosboot-libusb-1.0.30-macos-arm64 \
  -DFETCHCONTENT_SOURCE_DIR_BOOST=/private/tmp/kairosboot-r10-build/_deps/boost-src \
  -DFETCHCONTENT_SOURCE_DIR_KAIROSBOOT_MINIZ_SOURCE=/private/tmp/kairosboot-r10-build/_deps/kairosboot_miniz_source-src \
  -DFETCHCONTENT_SOURCE_DIR_KAIROSBOOT_YAML_CPP=/private/tmp/kairosboot-cli-update-build/_deps/kairosboot_yaml_cpp-src
cmake --build build-release --parallel 6

ctest --test-dir build-release --output-on-failure -j 6     # 全量一次，预期 63/63
# 本机 shell 非沙箱，loopback 可用；若仍有个别网络测试偶发失败，先单跑复现再判断

python3 scripts/check_repository_policy.py
python3 -m unittest tests.tooling.test_release_tools
python3 -m unittest tests.tooling.test_compatibility_inventory
python3 -m unittest tests.compat.test_official_fastboot_differential
python3 scripts/check_abi.py --library build-release/libkairosboot.dylib
python3 scripts/generate_compatibility_inventory.py --check
python3 bindings/dotnet/KairosBoot.ContractTests/run_update_shim_tests.py --framework net10.0
git diff --check
```

## 4. push 与远端验收（全部本地工作完成后才做）

1. 确认 aggregate clean。
2. push 一次到 PR #60 分支（`git push origin codex/compat-aggregate-v1`）。
3. 只跑一次 exact-head `ci-required` + `policy`；HIL 不可跑（见第 6 节）。
4. exact-head 全绿后才可按用户授权 `gh pr merge --admin`。
5. 合并后验证 `HEAD == origin/main`、ahead/behind `0 0`、clean status。

## 5. GitHub 治理与 Release 状态（不变，沿用原交接第 6 节）

- `main` ruleset active：PR required、1 approval、CODEOWNER review、dismiss stale、
  last-push approval、conversation resolution、linear history、strict
  `ci-required`/`policy`、禁止 deletion/non-fast-forward。
- `EurekaRaider` 是唯一 collaborator（admin/push）且为 always-bypass actor。
- release tag ruleset 保护 `v*`；`github-release` Environment reviewer 为
  `EurekaRaider`，deployment policy 只允许 `v*` tag。
- 版本仍为 `0.1.0-dev`。未完成第 6 节 HIL/性能/soak 与最终 exact-head CI 前，
  不要创建 `v1.0.0` tag 或 Release，不要声称生产可用。

## 6. 仍不可在当前机器完成的外部验收（不能伪造）

- `KAIROSBOOT_HIL_ENABLED` 未配置，无 exact-head HIL run。
- Windows/Linux/macOS 真实 USB bootloader + fastbootd HIL。
- 单设备 raw bulk ceiling ≥90% 与 AOSP 条件性 ≥10% 对比。
- 32 台、多 root controller 的 makespan/吞吐/Jain fairness。
- unplug、hub reset、重复 serial、重枚举故障注入。
- 24 小时硬件 soak、无泄漏/串线/持续 RSS 增长。
- Windows/macOS 正式签名与 notarization 凭据。

## 7. 重要纪律（不变）

- 公开 C ABI、根 CMake、`.github/**`、compat schema/generator 由集成 agent 统一处理。
- 不复制 AOSP 原始代码到 MIT 核心；必要复用进入 `third_party/aosp/<sha>` 并保留许可证。
- 不通过修改 golden、降低门禁、把 missing 改 intentional deviation 掩盖失败。
  （对照：本轮把 16/19 改 19/22、把 raw-flash 帧序列改成官方 trace 序列，是
  “让测试跟上有证据背书的真实行为”，不是放松门禁。）
- 只用 Boost 网络实现，不重新引入 Winsock/BSD socket 产品路径。
- Release 产物必须 `Release`：macOS/Linux `-O3 -DNDEBUG`，Windows `/O2 /DNDEBUG`。
- 保留主工作区 `.artemis/` 与交接文档；不要删除任何
  `/private/tmp/kairosboot-worktrees/*`，至少在最终 PR 合并前保留。

## 8. worktree 与外部资产清单

```text
/private/tmp/kairosboot-worktrees/root-compat-aggregate-v1          # 聚合主战场，HEAD 573da59, clean
/private/tmp/kairosboot-worktrees/public-reconnect-review-fixes-v1  # 已完成，HEAD d632131, clean，可留档
/private/tmp/kairosboot-worktrees/boot-diff-v1                      # HEAD 304bcc4，3 个未提交 evidence 文件
                                                                    # 仍是过期 capture，不要提交、不要 cherry-pick
/private/tmp/kairosboot-official-platform-tools-37.0.1/platform-tools/fastboot  # 官方 37.0.1 二进制
/private/tmp/kairosboot-libusb-1.0.30-macos-arm64                   # libusb root
/private/tmp/kairosboot-r10-build/_deps/boost-src                   # Boost 源
/private/tmp/kairosboot-r10-build/_deps/kairosboot_miniz_source-src # miniz 源
/private/tmp/kairosboot-cli-update-build/_deps/kairosboot_yaml_cpp-src  # yaml-cpp 源
/tmp/diag_raw_flash.py                                              # 本轮调试脚本，可删
```

## 9. 快速上下文：关键机制备忘

- `_repository_commit()`（generate_compatibility_inventory.py）在 dirty tree 返回 None
  → coverage 降级 not-run/0。看到 0 先查 `git status`，不要改 generator。
- provenance：capture metadata 的 `kairosboot.sourceCommit` 必须等于 HEAD，或 HEAD 是
  其 evidence-only descendant（diff 仅 `compat/evidence/**`、index、generated 输出）。
  任何产品/测试/构建 commit 之后都要 recapture。
- `flash:raw` 走 `aosp_raw_profile`（只查 `has-slot:<p>`）；普通 `flash` 走完整
  KairosBoot 前置（`is-userspace / has-slot / is-logical / max-download-size`）。
  scripted_cli_test.py 中两条路径的 fake device 序列分别对应，别混用。
- fleet preflight（`make_libusb_device_preflight_session_opener` 路径）要求
  enumerate 结果带恰好一条完整平台 topology；单测里用注入的 fake
  `resolve_macos_topology`（见 `topology_functions` helper）提供。
