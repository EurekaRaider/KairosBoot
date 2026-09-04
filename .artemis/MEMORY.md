# Artemis Memory

## KairosBoot 交付流程与跨平台 CI 陷阱
Keywords: kairosboot, designated initializer, missing-field-initializers, admin merge, worktree, release build, ci, handoff

KairosBoot 仓库（EurekaRaider/KairosBoot，本地 /Users/williamji/Documents/GitHub/KairosBoot）多 PR 交付流程的关键事实与教训（2026-08-27 会话验证）：

1. 跨平台 CI 陷阱：g++（Linux/sanitizer/CodeQL，-Wall -Wextra -Wpedantic -Werror）对 designated initializer 列表跳过无 NSDMI 成员报 -Wmissing-field-initializers，Apple Clang 完全不报。本地 macOS 绿 ≠ CI 绿。规则：designated initializer 按声明顺序完整初始化全部成员；用 `clang++ -std=c++23 -Wmissing-field-initializers -Werror -fsyntax-only` 显式探测。有 NSDMI 的省略安全。
2. CLI 子进程输出断言必须归一化 CRLF→\n（Windows 文本模式），见 tests/cli/scripted_cli_test.py 的 local() helper。
3. 内嵌 SHA-256 oracle 常量易转录笔误（K[4]=0x3956c25b、IV state[5]=0x9b05688c 两个实例）；优先断言冻结 golden digest 992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def3823f4。
4. 合并协议：main 要求 PR+owner 审批+required checks+up-to-date+线性历史；用户已授权 `gh pr merge --squash --admin` 仅用于 owner 自审循环，前提 exact-head 全部检查通过+独立复审无 P1/P2；每次 merge 后其余 open PR 必须 rebase 新 main 重跑 CI；merge 后验证 `git rev-list --left-right --count HEAD...origin/main` 为 0 0。macOS x64 runner 偶发 file_source 超时，与本分支无关时 `gh run rerun <run> --failed`。
5. 本机构建（macOS ARM64 Release，依赖缓存 /private/tmp/kairosboot-libusb-1.0.30-macos-arm64、/private/tmp/kairosboot-r10-build/_deps/boost-src、…_deps/kairosboot_miniz_source-src、/private/tmp/kairosboot-cli-update-build/_deps/kairosboot_yaml_cpp-src）：cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release -DKAIROSBOOT_LIBUSB_ROOT=… -DFETCHCONTENT_SOURCE_DIR_BOOST=… 等；PR 检查另有 git diff --check、scripts/check_repository_policy.py、ctest -R abi_symbols（check_abi.py 经 ctest abi_symbols 挂接）。dotnet 10 SDK 在 /usr/local/share/dotnet/dotnet（net48 仅编译验证）。localhost bind 可用。
6. 并行代理模式：实现代理在 /private/tmp/kairosboot-worktrees/<slice> 独立 worktree 单一可审查 commit，不 push；Root（主代理）唯一远端 writer，负责复审、push、PR、merge。当前状态（main=a8369f6，ctest 基线 50）：fleet validate/plan 已在 C/C++23/C#/CLI 四级落地；下一步是 coordinator/controller arbitration 切片与 C run/cancel/report ABI，详见仓库根 AI_HANDOFF_UNFINISHED.md（每轮会话后更新）。

## KairosBoot 兼容聚合分支交接要点（V2）
Keywords: kairosboot, compat-aggregate-v1, official capture recapture, provenance evidence-only, flash:raw aosp preflight, ai_handoff_v2

KairosBoot aggregate worktree /private/tmp/kairosboot-worktrees/root-compat-aggregate-v1（分支 codex/compat-aggregate-v1）。本轮（2026-08-28 晚）完成：public reconnect 复审修复（commit 21d4ac7，含 initializer 宏→static inline、preflight certainty 聚合、UsbFastbootTransportOptions::absolute_deadline 全链路 clamp）、19/19 official capture（evidence commit 5dbd8d0）、全量 ctest 63 中 2 个失败项根因修复（commit 573da59：scripted_cli_test.py 的 flash:raw 帧序列改为 AOSP has-slot-only 前置；test_compatibility_inventory.py fixture 期望 16/19→19/22）。关键机制：1) generate_compatibility_inventory.py 的 _repository_commit 在 dirty tree 返回 None，coverage 降级 not-run/0；2) capture 只能绑定最终产品 HEAD 或 evidence-only descendant（diff 仅 compat/evidence/**、index、generated 输出），任何产品/测试/构建 commit 后必须 recapture——573da59 穿透了 5dbd8d0 的绑定，接手后第一件事就是按 AI_HANDOFF_V2.md 第 2 节重跑 capture；3) flash:raw 走 aosp_raw_profile（只查 has-slot:<p>），普通 flash 保留完整前置序列，scripted 测试期望不能混用；4) fleet preflight 单测需注入 fake resolve_macos_topology（topology_functions helper）。构建依赖路径：libusb=/private/tmp/kairosboot-libusb-1.0.30-macos-arm64，Boost/miniz/yaml-cpp 见 /private/tmp/kairosboot-r10-build/_deps 与 kairosboot-cli-update-build/_deps，官方 fastboot=/private/tmp/kairosboot-official-platform-tools-37.0.1。boot-diff-v1 里 3 个未提交 evidence 文件是过期 capture，不要提交。
