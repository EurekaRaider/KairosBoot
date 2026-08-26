// SPDX-License-Identifier: MIT
#include "src/fastboot/update_plan.hpp"

#include <algorithm>
#include <charconv>
#include <limits>
#include <string>
#include <unordered_map>
#include <utility>

namespace kairosboot::fastboot {
namespace {

constexpr std::uint32_t kFrozenFastbootInfoVersion = 1;

struct Line final {
    UpdateManifestKind manifest;
    std::size_t number;
    std::size_t byte_offset;
    std::string_view text;
};

struct Token final {
    std::string_view text;
    std::size_t column;
};

struct Assignment final {
    std::string_view name;
    std::size_t name_offset;
    std::string_view raw_options;
    std::size_t options_offset;
};

struct RequirementSyntax final {
    RequirementAction action{RequirementAction::Require};
    Assignment assignment{};
    std::optional<std::string_view> product{};
    std::size_t product_offset{0};
};

struct ParserState final {
    ParsedUpdateManifest manifest;
    std::size_t total_options{0};
    std::unordered_map<std::string, UpdateSourceLocation> flash_targets;
};

[[nodiscard]] constexpr bool is_ascii_space(const char value) noexcept {
    return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
           value == '\f' || value == '\v';
}

[[nodiscard]] std::size_t skip_ascii_space(
    const std::string_view value,
    std::size_t offset) noexcept {
    while (offset < value.size() && is_ascii_space(value[offset])) {
        ++offset;
    }
    return offset;
}

[[nodiscard]] UpdateSourceLocation location(
    const Line& line,
    const std::size_t offset_in_line) noexcept {
    return {
        .manifest = line.manifest,
        .line = line.number,
        .column = offset_in_line + 1,
        .byte_offset = line.byte_offset + offset_in_line,
    };
}

[[nodiscard]] UpdateSourceLocation location_at_offset(
    const UpdateManifestKind manifest,
    const std::string_view input,
    const std::size_t requested_offset) noexcept {
    const auto offset = std::min(requested_offset, input.size());
    std::size_t line = 1;
    std::size_t line_offset = 0;
    for (std::size_t index = 0; index < offset; ++index) {
        if (input[index] == '\n') {
            ++line;
            line_offset = index + 1;
        }
    }
    return {
        .manifest = manifest,
        .line = line,
        .column = offset - line_offset + 1,
        .byte_offset = offset,
    };
}

[[nodiscard]] std::unexpected<UpdatePlanError> fail(
    const UpdatePlanErrorCode code,
    const UpdateSourceLocation& where,
    std::string message) {
    return std::unexpected(UpdatePlanError{
        .code = code,
        .location = where,
        .message = std::move(message),
    });
}

template <typename Callback>
[[nodiscard]] std::expected<void, UpdatePlanError> for_each_line(
    const UpdateManifestKind manifest,
    const std::string_view input,
    const UpdatePlanLimits& limits,
    Callback&& callback) {
    if (input.size() > limits.maximum_file_bytes) {
        return fail(
            UpdatePlanErrorCode::LimitExceeded,
            location_at_offset(manifest, input, limits.maximum_file_bytes),
            "manifest exceeds maximum byte size");
    }

    if (const auto nul = input.find('\0'); nul != std::string_view::npos) {
        return fail(
            UpdatePlanErrorCode::EmbeddedNul,
            location_at_offset(manifest, input, nul),
            "manifest contains an embedded NUL byte");
    }

    std::size_t line_number = 1;
    std::size_t line_offset = 0;
    while (true) {
        if (line_number > limits.maximum_lines) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                UpdateSourceLocation{
                    .manifest = manifest,
                    .line = line_number,
                    .column = 1,
                    .byte_offset = line_offset,
                },
                "manifest exceeds maximum line count");
        }

        const auto newline = input.find('\n', line_offset);
        const auto line_end =
            newline == std::string_view::npos ? input.size() : newline;
        const auto line_size = line_end - line_offset;
        const Line line{
            .manifest = manifest,
            .number = line_number,
            .byte_offset = line_offset,
            .text = input.substr(line_offset, line_size),
        };
        if (line_size > limits.maximum_line_bytes) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                location(line, limits.maximum_line_bytes),
                "manifest line exceeds maximum byte size");
        }
        if (auto result = callback(line); !result) {
            return std::unexpected(std::move(result.error()));
        }

        if (newline == std::string_view::npos) {
            break;
        }
        line_offset = newline + 1;
        ++line_number;
    }
    return {};
}

[[nodiscard]] std::expected<std::vector<Token>, UpdatePlanError>
tokenize_fastboot_line(
    const Line& line,
    const UpdatePlanLimits& limits) {
    std::vector<Token> result;
    std::size_t offset = 0;
    while (offset < line.text.size()) {
        while (offset < line.text.size() && line.text[offset] == ' ') {
            ++offset;
        }
        if (offset == line.text.size()) {
            break;
        }
        const auto start = offset;
        while (offset < line.text.size() && line.text[offset] != ' ') {
            ++offset;
        }
        const auto size = offset - start;
        if (size > limits.maximum_field_bytes) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                location(line, start + limits.maximum_field_bytes),
                "fastboot-info field exceeds maximum byte size");
        }
        if (result.size() >= limits.maximum_tokens_per_line) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                location(line, start),
                "fastboot-info line exceeds maximum token count");
        }
        result.push_back(Token{
            .text = line.text.substr(start, size),
            .column = start + 1,
        });
    }
    return result;
}

[[nodiscard]] bool parse_aosp_uint32(
    const std::string_view value,
    std::uint32_t* output) noexcept {
    if (value.empty()) {
        return false;
    }

    int base = 10;
    std::size_t offset = 0;
    bool negative = false;
    if (value.size() >= 2 && value[0] == '0' &&
        (value[1] == 'x' || value[1] == 'X')) {
        base = 16;
        offset = 2;
    } else {
        offset = skip_ascii_space(value, 0);
        if (offset < value.size() &&
            (value[offset] == '+' || value[offset] == '-')) {
            negative = value[offset] == '-';
            ++offset;
        }
    }
    if (offset == value.size()) {
        return false;
    }

    std::uint64_t parsed = 0;
    const auto begin = value.data() + static_cast<std::ptrdiff_t>(offset);
    const auto end = value.data() + static_cast<std::ptrdiff_t>(value.size());
    const auto conversion = std::from_chars(begin, end, parsed, base);
    if (conversion.ec != std::errc{} || conversion.ptr != end ||
        parsed > std::numeric_limits<std::uint32_t>::max() ||
        (negative && parsed != 0)) {
        return false;
    }
    *output = static_cast<std::uint32_t>(parsed);
    return true;
}

[[nodiscard]] std::optional<Assignment> parse_assignment(
    const std::string_view line,
    const std::size_t requested_start,
    std::size_t* farthest) noexcept {
    const auto name_start = skip_ascii_space(line, requested_start);
    *farthest = std::max(*farthest, name_start);
    if (name_start == line.size()) {
        return std::nullopt;
    }

    std::size_t token_end = name_start;
    while (token_end < line.size() && !is_ascii_space(line[token_end])) {
        ++token_end;
    }
    *farthest = std::max(*farthest, token_end);

    auto equals = line.rfind('=', token_end - 1);
    if (equals == std::string_view::npos || equals < name_start) {
        equals = std::string_view::npos;
    }

    std::size_t name_end = token_end;
    if (equals != std::string_view::npos && equals > name_start) {
        name_end = equals;
    } else {
        auto next = skip_ascii_space(line, token_end);
        *farthest = std::max(*farthest, next);
        if (next == line.size() || line[next] != '=') {
            return std::nullopt;
        }
        equals = next;
    }

    const auto options_start = skip_ascii_space(line, equals + 1);
    *farthest = std::max(*farthest, options_start);
    return Assignment{
        .name = line.substr(name_start, name_end - name_start),
        .name_offset = name_start,
        .raw_options = line.substr(options_start),
        .options_offset = options_start,
    };
}

[[nodiscard]] std::optional<RequirementSyntax> parse_requirement_syntax(
    const std::string_view line,
    std::size_t* error_offset) noexcept {
    std::size_t farthest = 0;

    const auto try_prefixed = [&](const std::string_view keyword,
                                  const RequirementAction action)
        -> std::optional<RequirementSyntax> {
        if (!line.starts_with(keyword) || line.size() == keyword.size() ||
            !is_ascii_space(line[keyword.size()])) {
            return std::nullopt;
        }
        if (auto assignment =
                parse_assignment(line, keyword.size(), &farthest)) {
            return RequirementSyntax{
                .action = action,
                .assignment = *assignment,
            };
        }
        return std::nullopt;
    };

    if (auto parsed = try_prefixed("require", RequirementAction::Require)) {
        return parsed;
    }
    if (auto parsed = try_prefixed("reject", RequirementAction::Reject)) {
        return parsed;
    }
    if (auto assignment = parse_assignment(line, 0, &farthest)) {
        return RequirementSyntax{
            .action = RequirementAction::Require,
            .assignment = *assignment,
        };
    }

    constexpr std::string_view product_prefix{"require-for-product:"};
    if (line.starts_with(product_prefix)) {
        const auto product_start =
            skip_ascii_space(line, product_prefix.size());
        farthest = std::max(farthest, product_start);
        std::size_t product_end = product_start;
        while (product_end < line.size() &&
               !is_ascii_space(line[product_end])) {
            ++product_end;
        }
        farthest = std::max(farthest, product_end);
        if (product_end > product_start && product_end < line.size()) {
            const auto variable_start = skip_ascii_space(line, product_end);
            farthest = std::max(farthest, variable_start);
            if (auto assignment =
                    parse_assignment(line, variable_start, &farthest)) {
                return RequirementSyntax{
                    .action = RequirementAction::Require,
                    .assignment = *assignment,
                    .product = line.substr(
                        product_start, product_end - product_start),
                    .product_offset = product_start,
                };
            }
        }
    }

    *error_offset = farthest;
    return std::nullopt;
}

[[nodiscard]] std::expected<void, UpdatePlanError> check_field_size(
    const Line& line,
    const std::size_t offset,
    const std::string_view field,
    const UpdatePlanLimits& limits,
    const std::string_view description) {
    if (field.size() <= limits.maximum_field_bytes) {
        return {};
    }
    return fail(
        UpdatePlanErrorCode::LimitExceeded,
        location(line, offset + limits.maximum_field_bytes),
        std::string(description) + " exceeds maximum byte size");
}

[[nodiscard]] std::expected<std::vector<std::string>, UpdatePlanError>
parse_requirement_options(
    const Line& line,
    const Assignment& assignment,
    const UpdatePlanLimits& limits,
    std::size_t* total_options) {
    std::vector<std::string> options;
    std::size_t start = 0;
    while (true) {
        if (options.size() >= limits.maximum_options_per_requirement ||
            *total_options >= limits.maximum_total_options) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                location(line, assignment.options_offset + start),
                "android-info requirement exceeds maximum option count");
        }

        const auto separator = assignment.raw_options.find('|', start);
        const auto end = separator == std::string_view::npos
                             ? assignment.raw_options.size()
                             : separator;
        auto option_start = start;
        while (option_start < end &&
               is_ascii_space(assignment.raw_options[option_start])) {
            ++option_start;
        }
        auto option_end = end;
        while (option_end > option_start &&
               is_ascii_space(assignment.raw_options[option_end - 1])) {
            --option_end;
        }
        const auto option = assignment.raw_options.substr(
            option_start, option_end - option_start);
        if (auto checked = check_field_size(
                line,
                assignment.options_offset + option_start,
                option,
                limits,
                "android-info option");
            !checked) {
            return std::unexpected(std::move(checked.error()));
        }
        options.emplace_back(option);
        ++*total_options;

        if (separator == std::string_view::npos) {
            break;
        }
        start = separator + 1;
    }
    return options;
}

[[nodiscard]] std::expected<void, UpdatePlanError> parse_android_line(
    const Line& line,
    const UpdatePlanLimits& limits,
    ParserState* state) {
    if (line.text.empty()) {
        return {};
    }
    if (state->manifest.requirements.size() >= limits.maximum_requirements) {
        return fail(
            UpdatePlanErrorCode::LimitExceeded,
            location(line, 0),
            "android-info exceeds maximum requirement count");
    }

    std::size_t syntax_error_offset = 0;
    auto syntax = parse_requirement_syntax(line.text, &syntax_error_offset);
    if (!syntax) {
        return fail(
            UpdatePlanErrorCode::Syntax,
            location(line, syntax_error_offset),
            "invalid android-info requirement syntax");
    }
    if (auto checked = check_field_size(
            line,
            syntax->assignment.name_offset,
            syntax->assignment.name,
            limits,
            "android-info variable");
        !checked) {
        return checked;
    }
    if (syntax->product) {
        if (auto checked = check_field_size(
                line,
                syntax->product_offset,
                *syntax->product,
                limits,
                "android-info product");
            !checked) {
            return checked;
        }
    }

    auto options = parse_requirement_options(
        line, syntax->assignment, limits, &state->total_options);
    if (!options) {
        return std::unexpected(std::move(options.error()));
    }

    auto variable = std::string(syntax->assignment.name);
    if (variable == "board") {
        variable = "product";
    }
    state->manifest.requirements.push_back(PlannedRequirement{
        .action = syntax->action,
        .variable = std::move(variable),
        .product = syntax->product
                       ? std::optional<std::string>{std::string(*syntax->product)}
                       : std::nullopt,
        .options = std::move(*options),
        .location = location(line, syntax->assignment.name_offset),
    });
    return {};
}

[[nodiscard]] std::optional<std::size_t> unsafe_artifact_offset(
    const std::string_view path) noexcept {
    if (path.empty()) {
        return 0;
    }
    if (path.front() == '/') {
        return 0;
    }
    if (path.size() >= 2 &&
        ((path[0] >= 'A' && path[0] <= 'Z') ||
         (path[0] >= 'a' && path[0] <= 'z')) &&
        path[1] == ':') {
        return 0;
    }
    if (const auto backslash = path.find('\\');
        backslash != std::string_view::npos) {
        return backslash;
    }

    std::size_t segment_start = 0;
    while (true) {
        const auto slash = path.find('/', segment_start);
        const auto segment_end =
            slash == std::string_view::npos ? path.size() : slash;
        const auto segment =
            path.substr(segment_start, segment_end - segment_start);
        if (segment.empty() || segment == "." || segment == "..") {
            return segment_start == path.size() ? path.size() - 1
                                                : segment_start;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        segment_start = slash + 1;
    }
    return std::nullopt;
}

[[nodiscard]] UpdateSourceLocation token_location(
    const Line& line,
    const Token& token,
    const std::size_t offset = 0) noexcept {
    return location(line, token.column - 1 + offset);
}

[[nodiscard]] std::expected<void, UpdatePlanError> add_flash_task(
    const Line& line,
    const std::vector<Token>& tokens,
    const std::size_t command_index,
    const bool conditional,
    const UpdatePlanLimits& limits,
    ParserState* state) {
    bool apply_vbmeta = false;
    bool slot_other = false;
    const Token* partition = nullptr;
    const Token* artifact = nullptr;
    for (std::size_t index = command_index + 1; index < tokens.size(); ++index) {
        const auto& token = tokens[index];
        if (token.text == "--apply-vbmeta") {
            if (apply_vbmeta) {
                return fail(
                    UpdatePlanErrorCode::Duplicate,
                    token_location(line, token),
                    "duplicate --apply-vbmeta flag");
            }
            apply_vbmeta = true;
        } else if (token.text == "--slot-other") {
            if (slot_other) {
                return fail(
                    UpdatePlanErrorCode::Duplicate,
                    token_location(line, token),
                    "duplicate --slot-other flag");
            }
            slot_other = true;
        } else if (partition == nullptr) {
            partition = &token;
        } else if (artifact == nullptr) {
            artifact = &token;
        } else {
            return fail(
                UpdatePlanErrorCode::Syntax,
                token_location(line, token),
                "flash has more than two positional arguments");
        }
    }
    if (partition == nullptr) {
        return fail(
            UpdatePlanErrorCode::Syntax,
            location(line, line.text.size()),
            "flash requires a partition");
    }

    std::string artifact_name;
    UpdateSourceLocation artifact_location;
    if (artifact != nullptr) {
        artifact_name = std::string(artifact->text);
        artifact_location = token_location(line, *artifact);
    } else {
        if (partition->text.size() > limits.maximum_field_bytes ||
            limits.maximum_field_bytes - partition->text.size() < 4) {
            return fail(
                UpdatePlanErrorCode::LimitExceeded,
                token_location(line, *partition),
                "derived flash artifact exceeds maximum byte size");
        }
        artifact_name = std::string(partition->text) + ".img";
        artifact_location = token_location(line, *partition);
    }
    if (artifact_name.size() > limits.maximum_field_bytes) {
        return fail(
            UpdatePlanErrorCode::LimitExceeded,
            artifact_location,
            "flash artifact exceeds maximum byte size");
    }
    if (const auto unsafe = unsafe_artifact_offset(artifact_name)) {
        auto where = artifact_location;
        where.column += *unsafe;
        where.byte_offset += *unsafe;
        return fail(
            UpdatePlanErrorCode::UnsafeArtifactPath,
            where,
            "flash artifact path is absolute, ambiguous, or traverses a parent");
    }

    const auto slot = slot_other ? PlannedSlot::Other : PlannedSlot::Default;
    std::string target_key;
    target_key.reserve(partition->text.size() + 2);
    target_key.push_back(slot_other ? 'O' : 'D');
    target_key.push_back(':');
    target_key.append(partition->text);
    const auto target_location = token_location(line, *partition);
    if (const auto previous = state->flash_targets.find(target_key);
        previous != state->flash_targets.end()) {
        return fail(
            UpdatePlanErrorCode::Duplicate,
            target_location,
            "flash target duplicates line " +
                std::to_string(previous->second.line) + ", column " +
                std::to_string(previous->second.column));
    }
    state->flash_targets.emplace(std::move(target_key), target_location);

    state->manifest.tasks.push_back(PlannedUpdateTask{
        .kind = UpdateTaskKind::Flash,
        .conditional_on_wipe = conditional,
        .location = token_location(line, tokens[command_index]),
        .partition = std::string(partition->text),
        .artifact = std::move(artifact_name),
        .slot = slot,
        .apply_vbmeta = apply_vbmeta,
    });
    return {};
}

[[nodiscard]] std::expected<void, UpdatePlanError> add_reboot_task(
    const Line& line,
    const std::vector<Token>& tokens,
    const std::size_t command_index,
    const bool conditional,
    ParserState* state) {
    if (tokens.size() > command_index + 2) {
        return fail(
            UpdatePlanErrorCode::Syntax,
            token_location(line, tokens[command_index + 2]),
            "reboot accepts at most one target");
    }

    PlannedRebootTarget target = PlannedRebootTarget::System;
    if (tokens.size() == command_index + 2) {
        const auto& target_token = tokens[command_index + 1];
        if (target_token.text == "bootloader") {
            target = PlannedRebootTarget::Bootloader;
        } else if (target_token.text == "recovery") {
            target = PlannedRebootTarget::Recovery;
        } else if (target_token.text == "fastboot") {
            target = PlannedRebootTarget::Fastboot;
        } else {
            return fail(
                UpdatePlanErrorCode::Syntax,
                token_location(line, target_token),
                "reboot target is not executable by frozen AOSP fastboot");
        }
    }

    state->manifest.tasks.push_back(PlannedUpdateTask{
        .kind = UpdateTaskKind::Reboot,
        .conditional_on_wipe = conditional,
        .location = token_location(line, tokens[command_index]),
        .reboot_target = target,
    });
    return {};
}

[[nodiscard]] std::expected<void, UpdatePlanError> parse_fastboot_task(
    const Line& line,
    const std::vector<Token>& tokens,
    const std::size_t command_index,
    const bool conditional,
    const UpdatePlanLimits& limits,
    ParserState* state) {
    if (state->manifest.tasks.size() >= limits.maximum_tasks) {
        return fail(
            UpdatePlanErrorCode::LimitExceeded,
            token_location(line, tokens[command_index]),
            "fastboot-info exceeds maximum task count");
    }

    const auto command = tokens[command_index].text;
    if (command == "flash") {
        return add_flash_task(
            line, tokens, command_index, conditional, limits, state);
    }
    if (command == "reboot") {
        return add_reboot_task(
            line, tokens, command_index, conditional, state);
    }
    if (command == "update-super") {
        if (tokens.size() != command_index + 1) {
            return fail(
                UpdatePlanErrorCode::Syntax,
                token_location(line, tokens[command_index + 1]),
                "update-super does not accept arguments");
        }
        state->manifest.tasks.push_back(PlannedUpdateTask{
            .kind = UpdateTaskKind::UpdateSuper,
            .conditional_on_wipe = conditional,
            .location = token_location(line, tokens[command_index]),
        });
        return {};
    }
    if (command == "erase") {
        if (tokens.size() != command_index + 2) {
            const auto where = tokens.size() > command_index + 2
                                   ? token_location(
                                         line, tokens[command_index + 2])
                                   : location(line, line.text.size());
            return fail(
                UpdatePlanErrorCode::Syntax,
                where,
                "erase requires exactly one partition");
        }
        state->manifest.tasks.push_back(PlannedUpdateTask{
            .kind = UpdateTaskKind::Erase,
            .conditional_on_wipe = conditional,
            .location = token_location(line, tokens[command_index]),
            .partition = std::string(tokens[command_index + 1].text),
        });
        return {};
    }

    return fail(
        UpdatePlanErrorCode::Syntax,
        token_location(line, tokens[command_index]),
        "unknown fastboot-info command");
}

[[nodiscard]] std::expected<void, UpdatePlanError> parse_fastboot_line(
    const Line& line,
    const UpdatePlanLimits& limits,
    ParserState* state) {
    auto token_result = tokenize_fastboot_line(line, limits);
    if (!token_result) {
        return std::unexpected(std::move(token_result.error()));
    }
    const auto& tokens = *token_result;
    if (tokens.empty() || tokens.front().text.starts_with('#')) {
        return {};
    }

    if (tokens.front().text == "version") {
        if (tokens.size() != 2) {
            const auto where = tokens.size() > 2
                                   ? token_location(line, tokens[2])
                                   : location(line, line.text.size());
            return fail(
                UpdatePlanErrorCode::Syntax,
                where,
                "version requires exactly one numeric value");
        }
        if (state->manifest.fastboot_info_version) {
            return fail(
                UpdatePlanErrorCode::Duplicate,
                token_location(line, tokens.front()),
                "fastboot-info contains more than one version declaration");
        }
        std::uint32_t version = 0;
        if (!parse_aosp_uint32(tokens[1].text, &version)) {
            return fail(
                UpdatePlanErrorCode::Syntax,
                token_location(line, tokens[1]),
                "version is not an AOSP uint32 value");
        }
        if (version > kFrozenFastbootInfoVersion) {
            return fail(
                UpdatePlanErrorCode::UnsupportedVersion,
                token_location(line, tokens[1]),
                "fastboot-info version is newer than frozen host version 1");
        }
        state->manifest.fastboot_info_version = version;
        return {};
    }

    std::size_t command_index = 0;
    bool conditional = false;
    if (tokens.front().text == "if-wipe") {
        conditional = true;
        if (tokens.size() == 1) {
            return fail(
                UpdatePlanErrorCode::Syntax,
                location(line, line.text.size()),
                "if-wipe requires a task");
        }
        command_index = 1;
    }
    return parse_fastboot_task(
        line, tokens, command_index, conditional, limits, state);
}

}  // namespace

std::expected<ParsedUpdateManifest, UpdatePlanError> parse_update_manifest(
    const std::string_view android_info,
    const std::string_view fastboot_info,
    const UpdatePlanLimits& limits) {
    ParserState state;
    if (auto parsed = for_each_line(
            UpdateManifestKind::AndroidInfo,
            android_info,
            limits,
            [&](const Line& line) {
                return parse_android_line(line, limits, &state);
            });
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    if (auto parsed = for_each_line(
            UpdateManifestKind::FastbootInfo,
            fastboot_info,
            limits,
            [&](const Line& line) {
                return parse_fastboot_line(line, limits, &state);
            });
        !parsed) {
        return std::unexpected(std::move(parsed.error()));
    }
    return std::move(state.manifest);
}

DeterministicUpdatePlan make_update_plan(
    const ParsedUpdateManifest& manifest,
    const bool wants_wipe) {
    DeterministicUpdatePlan result{
        .requirements = manifest.requirements,
        .fastboot_info_version = manifest.fastboot_info_version,
    };
    result.tasks.reserve(manifest.tasks.size());
    for (const auto& task : manifest.tasks) {
        if (!task.conditional_on_wipe || wants_wipe) {
            result.tasks.push_back(task);
        }
    }
    return result;
}

}  // namespace kairosboot::fastboot
