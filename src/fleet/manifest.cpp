// SPDX-License-Identifier: MIT
#include "src/fleet/manifest.hpp"

#include <yaml-cpp/eventhandler.h>
#include <yaml-cpp/parser.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <ranges>
#include <span>
#include <sstream>
#include <string_view>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace kairosboot::fleet {
namespace {

using namespace std::string_view_literals;

constexpr std::size_t kIdentifierBytes = 256U;
constexpr std::uint64_t kMinimumMemoryBudget = 1024U * 1024U;
constexpr std::uint64_t kMaximumMemoryBudget = 2ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr std::uint32_t kMinimumParallelDevices = 1U;
constexpr std::uint32_t kMaximumParallelDevices = 256U;
constexpr std::size_t kReadChunkBytes = 64U * 1024U;

[[nodiscard]] std::optional<ManifestSourceLocation> location_from_mark(
    const YAML::Mark& mark) noexcept {
    if (mark.line < 0 || mark.column < 0) {
        return std::nullopt;
    }
    const auto line = static_cast<std::uint64_t>(mark.line) + 1U;
    const auto column = static_cast<std::uint64_t>(mark.column) + 1U;
    if (line > std::numeric_limits<std::uint32_t>::max() ||
        column > std::numeric_limits<std::uint32_t>::max()) {
        return std::nullopt;
    }
    return ManifestSourceLocation{
        .line = static_cast<std::uint32_t>(line),
        .column = static_cast<std::uint32_t>(column),
    };
}

[[nodiscard]] ManifestSourceLocation required_location(const YAML::Node& node) {
    return location_from_mark(node.Mark()).value_or(ManifestSourceLocation{});
}

[[nodiscard]] ManifestError error(
    const ManifestErrorKind kind,
    std::string message,
    std::string path = {},
    std::optional<ManifestSourceLocation> location = std::nullopt,
    const int native_code = 0) {
    return ManifestError{
        .kind = kind,
        .native_code = native_code,
        .location = location,
        .path = std::move(path),
        .message = std::move(message),
    };
}

[[nodiscard]] std::optional<ManifestError> interruption_error(
    const ManifestParseOptions& options,
    const std::string_view path = {}) {
    if (options.cancellation.stop_requested()) {
        return error(ManifestErrorKind::Cancelled,
                     "fleet manifest parsing was cancelled",
                     std::string(path));
    }
    if (ManifestClock::now() >= options.deadline) {
        return error(ManifestErrorKind::TimedOut,
                     "fleet manifest parsing exceeded its deadline",
                     std::string(path));
    }
    return std::nullopt;
}

void invoke_fault(const ManifestParseOptions& options,
                  const ManifestFaultPoint point) {
    if (options.fault_hook != nullptr) {
        options.fault_hook(point, options.fault_context);
    }
}

[[nodiscard]] bool valid_utf8(const std::string_view value) noexcept {
    std::size_t index = 0U;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        if (first <= 0x7FU) {
            ++index;
            continue;
        }

        std::size_t continuation_count = 0U;
        std::uint32_t code_point = 0U;
        if ((first & 0xE0U) == 0xC0U) {
            continuation_count = 1U;
            code_point = first & 0x1FU;
        } else if ((first & 0xF0U) == 0xE0U) {
            continuation_count = 2U;
            code_point = first & 0x0FU;
        } else if ((first & 0xF8U) == 0xF0U) {
            continuation_count = 3U;
            code_point = first & 0x07U;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) {
            return false;
        }
        for (std::size_t continuation = 1U;
             continuation <= continuation_count;
             ++continuation) {
            const auto byte =
                static_cast<unsigned char>(value[index + continuation]);
            if ((byte & 0xC0U) != 0x80U) {
                return false;
            }
            code_point = (code_point << 6U) | (byte & 0x3FU);
        }
        if ((continuation_count == 1U && code_point < 0x80U) ||
            (continuation_count == 2U && code_point < 0x800U) ||
            (continuation_count == 3U && code_point < 0x10000U) ||
            code_point > 0x10FFFFU ||
            (code_point >= 0xD800U && code_point <= 0xDFFFU)) {
            return false;
        }
        index += continuation_count + 1U;
    }
    return true;
}

[[nodiscard]] bool implicit_tag(const std::string_view tag) noexcept {
    return tag.empty() || tag == "?" || tag == "!";
}

[[nodiscard]] bool yaml_core_plain_number(
    const std::string_view value) noexcept {
    if (value == ".nan" || value == ".NaN" || value == ".NAN") {
        return true;
    }

    auto body = value;
    bool signed_value = false;
    if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
        signed_value = true;
        body.remove_prefix(1U);
    }
    if (body == ".inf" || body == ".Inf" || body == ".INF") {
        return true;
    }
    if (body.empty()) {
        return false;
    }
    if (!signed_value && body.starts_with("0o")) {
        const auto digits = body.substr(2U);
        return !digits.empty() &&
               std::ranges::all_of(digits, [](const char character) {
                   return character >= '0' && character <= '7';
               });
    }
    if (!signed_value && body.starts_with("0x")) {
        const auto digits = body.substr(2U);
        return !digits.empty() &&
               std::ranges::all_of(digits, [](const char character) {
                   return (character >= '0' && character <= '9') ||
                          (character >= 'a' && character <= 'f') ||
                          (character >= 'A' && character <= 'F');
               });
    }

    std::size_t cursor = 0U;
    if (body.front() == '.') {
        ++cursor;
        const auto fraction_begin = cursor;
        while (cursor < body.size() && body[cursor] >= '0' &&
               body[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == fraction_begin) {
            return false;
        }
    } else {
        while (cursor < body.size() && body[cursor] >= '0' &&
               body[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == 0U) {
            return false;
        }
        if (cursor < body.size() && body[cursor] == '.') {
            ++cursor;
            while (cursor < body.size() && body[cursor] >= '0' &&
                   body[cursor] <= '9') {
                ++cursor;
            }
        }
    }
    if (cursor < body.size() &&
        (body[cursor] == 'e' || body[cursor] == 'E')) {
        ++cursor;
        if (cursor < body.size() &&
            (body[cursor] == '+' || body[cursor] == '-')) {
            ++cursor;
        }
        const auto exponent_begin = cursor;
        while (cursor < body.size() && body[cursor] >= '0' &&
               body[cursor] <= '9') {
            ++cursor;
        }
        if (cursor == exponent_begin) {
            return false;
        }
    }
    return cursor == body.size();
}

[[nodiscard]] bool yaml_core_plain_non_string(
    const std::string_view value) noexcept {
    constexpr std::array null_and_boolean_values{
        "~"sv,     "null"sv,  "Null"sv,  "NULL"sv,  "true"sv,
        "True"sv,  "TRUE"sv,  "false"sv, "False"sv, "FALSE"sv,
    };
    return std::ranges::find(null_and_boolean_values, value) !=
               null_and_boolean_values.end() ||
           yaml_core_plain_number(value);
}

class ScanAbort final : public std::exception {
public:
    explicit ScanAbort(ManifestError value) : error_(std::move(value)) {}

    [[nodiscard]] const ManifestError& manifest_error() const noexcept {
        return error_;
    }

    [[nodiscard]] const char* what() const noexcept override {
        return "fleet manifest event scan rejected the document";
    }

private:
    ManifestError error_;
};

class StrictEventHandler final : public YAML::EventHandler {
public:
    StrictEventHandler(const std::string_view input,
                       const ManifestParseOptions& options)
        : input_(input), options_(options) {}

    void OnDocumentStart(const YAML::Mark& mark) override {
        check_interruption(mark);
        ++documents_;
        if (documents_ > 1U) {
            reject(ManifestErrorKind::MultipleDocuments,
                   mark,
                   "fleet manifest must contain exactly one YAML document");
        }
    }

    void OnDocumentEnd() override {
        if (const auto interrupted = interruption_error(options_, "$")) {
            throw ScanAbort(*interrupted);
        }
        if (depth_ != 0U) {
            throw ScanAbort(error(ManifestErrorKind::Syntax,
                                  "YAML document ended with an open collection",
                                  "$"));
        }
    }

    void OnNull(const YAML::Mark& mark, const YAML::anchor_t anchor) override {
        check_interruption(mark);
        reject_anchor(mark, anchor);
        count_node(mark);
    }

    void OnAlias(const YAML::Mark& mark, const YAML::anchor_t) override {
        check_interruption(mark);
        reject(ManifestErrorKind::AliasNotAllowed,
               mark,
               "YAML aliases and shared nodes are not allowed");
    }

    void OnScalar(const YAML::Mark& mark,
                  const std::string& tag,
                  const YAML::anchor_t anchor,
                  const std::string& value) override {
        check_interruption(mark);
        reject_tag(mark, tag);
        reject_anchor(mark, anchor);
        count_node(mark);
        if (value.find('\0') != std::string::npos) {
            reject(ManifestErrorKind::InvalidValue,
                   mark,
                   "YAML scalar contains an embedded NUL");
        }
        if (value.size() > kMaximumManifestScalarBytes) {
            reject(ManifestErrorKind::LimitExceeded,
                   mark,
                   "YAML scalar exceeds the manifest byte limit");
        }
    }

    void OnSequenceStart(const YAML::Mark& mark,
                         const std::string& tag,
                         const YAML::anchor_t anchor,
                         const YAML::EmitterStyle::value) override {
        enter_collection(mark, tag, anchor);
    }

    void OnSequenceEnd() override { leave_collection(); }

    void OnMapStart(const YAML::Mark& mark,
                    const std::string& tag,
                    const YAML::anchor_t anchor,
                    const YAML::EmitterStyle::value) override {
        enter_collection(mark, tag, anchor);
    }

    void OnMapEnd() override { leave_collection(); }

    void OnAnchor(const YAML::Mark& mark, const std::string&) override {
        check_interruption(mark);
        reject(ManifestErrorKind::AliasNotAllowed,
               mark,
               "YAML anchors and shared nodes are not allowed");
    }

    [[nodiscard]] std::size_t documents() const noexcept { return documents_; }

private:
    [[noreturn]] void reject(const ManifestErrorKind kind,
                             const YAML::Mark& mark,
                             const std::string_view message) const {
        throw ScanAbort(error(kind,
                              std::string(message),
                              "$",
                              location_from_mark(mark)));
    }

    void check_interruption(const YAML::Mark& mark) const {
        if (const auto interrupted = interruption_error(options_, "$")) {
            auto result = *interrupted;
            result.location = location_from_mark(mark);
            throw ScanAbort(std::move(result));
        }
    }

    void reject_tag(const YAML::Mark& mark, const std::string_view tag) const {
        // yaml-cpp reports both a normally quoted scalar and an explicit bare
        // non-specific `!` tag as tag "!". Its event mark points at the quote
        // for the former and at the tag token for the latter, so consult the
        // immutable input to keep quoted JSON strings valid while closing the
        // explicit-tag grammar.
        const bool explicit_bare_tag =
            tag == "!" && mark.pos >= 0 &&
            static_cast<std::size_t>(mark.pos) < input_.size() &&
            input_[static_cast<std::size_t>(mark.pos)] == '!';
        if (!implicit_tag(tag) || explicit_bare_tag) {
            reject(ManifestErrorKind::UnsupportedTag,
                   mark,
                   "explicit or custom YAML tags are not allowed");
        }
    }

    void reject_anchor(const YAML::Mark& mark,
                       const YAML::anchor_t anchor) const {
        if (anchor != 0U) {
            reject(ManifestErrorKind::AliasNotAllowed,
                   mark,
                   "YAML anchors and shared nodes are not allowed");
        }
    }

    void count_node(const YAML::Mark& mark) {
        ++nodes_;
        if (nodes_ > kMaximumManifestNodes) {
            reject(ManifestErrorKind::LimitExceeded,
                   mark,
                   "YAML node count exceeds the manifest limit");
        }
    }

    void enter_collection(const YAML::Mark& mark,
                          const std::string_view tag,
                          const YAML::anchor_t anchor) {
        check_interruption(mark);
        reject_tag(mark, tag);
        reject_anchor(mark, anchor);
        count_node(mark);
        ++depth_;
        if (depth_ > kMaximumManifestDepth) {
            reject(ManifestErrorKind::LimitExceeded,
                   mark,
                   "YAML nesting depth exceeds the manifest limit");
        }
    }

    void leave_collection() {
        if (const auto interrupted = interruption_error(options_, "$")) {
            throw ScanAbort(*interrupted);
        }
        if (depth_ == 0U) {
            throw ScanAbort(error(ManifestErrorKind::Syntax,
                                  "YAML collection end has no matching start",
                                  "$"));
        }
        --depth_;
    }

    std::string_view input_;
    const ManifestParseOptions& options_;
    std::size_t documents_{};
    std::size_t nodes_{};
    std::size_t depth_{};
};

[[nodiscard]] std::expected<void, ManifestError> scan_events(
    const std::string& input,
    const ManifestParseOptions& options) {
    try {
        invoke_fault(options, ManifestFaultPoint::EventScan);
        std::istringstream stream(input);
        YAML::Parser parser(stream);
        StrictEventHandler handler(input, options);
        while (parser.HandleNextDocument(handler)) {
            if (const auto interrupted = interruption_error(options, "$")) {
                return std::unexpected(*interrupted);
            }
        }
        if (handler.documents() == 0U) {
            return std::unexpected(error(
                ManifestErrorKind::Syntax,
                "fleet manifest contains no YAML document",
                "$"));
        }
        return {};
    } catch (const ScanAbort& rejected) {
        return std::unexpected(rejected.manifest_error());
    } catch (const YAML::Exception& exception) {
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        return std::unexpected(error(ManifestErrorKind::Syntax,
                                     "invalid YAML syntax",
                                     "$",
                                     location_from_mark(exception.mark)));
    }
}

struct NodeTraversal final {
    std::vector<YAML::Node> active;
    std::size_t nodes{};
};

[[nodiscard]] std::expected<void, ManifestError> validate_node_graph(
    const YAML::Node& node,
    NodeTraversal& traversal,
    const ManifestParseOptions& options,
    const std::size_t depth = 1U) {
    if (const auto interrupted = interruption_error(options, "$")) {
        return std::unexpected(*interrupted);
    }
    if (depth > kMaximumManifestDepth) {
        return std::unexpected(error(
            ManifestErrorKind::LimitExceeded,
            "YAML nesting depth exceeds the manifest limit",
            "$",
            location_from_mark(node.Mark())));
    }
    ++traversal.nodes;
    if (traversal.nodes > kMaximumManifestNodes) {
        return std::unexpected(error(
            ManifestErrorKind::LimitExceeded,
            "YAML node count exceeds the manifest limit",
            "$",
            location_from_mark(node.Mark())));
    }
    if (!implicit_tag(node.Tag())) {
        return std::unexpected(error(
            ManifestErrorKind::UnsupportedTag,
            "explicit or custom YAML tags are not allowed",
            "$",
            location_from_mark(node.Mark())));
    }
    for (const auto& ancestor : traversal.active) {
        if (node.is(ancestor)) {
            return std::unexpected(error(
                ManifestErrorKind::AliasNotAllowed,
                "cyclic YAML nodes are not allowed",
                "$",
                location_from_mark(node.Mark())));
        }
    }
    // The event scan rejects every anchor and alias before YAML::Load builds
    // this graph, so a shared node cannot originate from accepted input. Keep
    // only the bounded active-chain identity check here as defense in depth;
    // comparing every node with every previous node would make a maximum-size
    // valid manifest quadratic.
    traversal.active.push_back(node);
    if (node.IsMap()) {
        for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
            if (auto result = validate_node_graph(
                    iterator->first, traversal, options, depth + 1U);
                !result) {
                return result;
            }
            if (auto result = validate_node_graph(
                    iterator->second, traversal, options, depth + 1U);
                !result) {
                return result;
            }
        }
    } else if (node.IsSequence()) {
        for (const auto& child : node) {
            if (auto result = validate_node_graph(
                    child, traversal, options, depth + 1U);
                !result) {
                return result;
            }
        }
    }
    traversal.active.pop_back();
    return {};
}

struct MapField final {
    std::string key;
    YAML::Node value;
    ManifestSourceLocation key_location;
};

class StrictMap final {
public:
    std::vector<MapField> fields;

    [[nodiscard]] const MapField* find(const std::string_view key) const noexcept {
        const auto found = std::ranges::find(fields, key, &MapField::key);
        return found == fields.end() ? nullptr : &*found;
    }
};

[[nodiscard]] bool allowed_key(const std::span<const std::string_view> allowed,
                               const std::string_view key) noexcept {
    return std::ranges::find(allowed, key) != allowed.end();
}

[[nodiscard]] std::expected<StrictMap, ManifestError> strict_map(
    const YAML::Node& node,
    const std::span<const std::string_view> allowed,
    const std::span<const std::string_view> required,
    const std::string& path,
    const ManifestParseOptions& options) {
    if (const auto interrupted = interruption_error(options, path)) {
        return std::unexpected(*interrupted);
    }
    if (!node.IsMap()) {
        return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                     "manifest value must be a map",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    StrictMap result;
    result.fields.reserve(node.size());
    for (auto iterator = node.begin(); iterator != node.end(); ++iterator) {
        if (const auto interrupted = interruption_error(options, path)) {
            return std::unexpected(*interrupted);
        }
        // yaml-cpp map iterators return a proxy value. Keep an owning Node
        // copy instead of a reference into that end-of-expression temporary.
        const auto key_node = iterator->first;
        if (!key_node.IsScalar()) {
            return std::unexpected(error(
                ManifestErrorKind::NonScalarKey,
                "manifest map keys must be scalar strings",
                path,
                location_from_mark(key_node.Mark())));
        }
        const auto& key = key_node.Scalar();
        if (!allowed_key(allowed, key)) {
            return std::unexpected(error(
                ManifestErrorKind::UnknownField,
                "manifest map contains an unknown field",
                path + "." + key,
                location_from_mark(key_node.Mark())));
        }
        if (result.find(key) != nullptr) {
            return std::unexpected(error(
                ManifestErrorKind::DuplicateKey,
                "manifest map contains a duplicate key",
                path + "." + key,
                location_from_mark(key_node.Mark())));
        }
        result.fields.push_back(MapField{
            .key = key,
            .value = iterator->second,
            .key_location = required_location(key_node),
        });
    }
    for (const auto required_key : required) {
        if (result.find(required_key) == nullptr) {
            return std::unexpected(error(
                ManifestErrorKind::MissingField,
                "manifest map is missing a required field",
                path + "." + std::string(required_key),
                location_from_mark(node.Mark())));
        }
    }
    return result;
}

[[nodiscard]] std::expected<LocatedManifestString, ManifestError> text_value(
    const YAML::Node& node,
    const std::size_t maximum_bytes,
    const std::string& path,
    const bool allow_empty = false) {
    if (!node.IsScalar()) {
        return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                     "manifest value must be a scalar string",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    const auto& value = node.Scalar();
    // The published JSON Schema describes these fields as strings. Under the
    // YAML 1.2 Core Schema, plain null/boolean/number spellings are not strings;
    // callers must quote those spellings when they are intended as text.
    if (node.Tag() == "?" && yaml_core_plain_non_string(value)) {
        return std::unexpected(error(
            ManifestErrorKind::TypeMismatch,
            "manifest value resolves to a YAML null, boolean, or number; "
            "quote it to use it as a string",
            path,
            location_from_mark(node.Mark())));
    }
    if ((!allow_empty && value.empty()) || value.size() > maximum_bytes ||
        !valid_utf8(value)) {
        return std::unexpected(error(
            ManifestErrorKind::InvalidValue,
            "manifest string is empty, invalid UTF-8, or exceeds its byte limit",
            path,
            location_from_mark(node.Mark())));
    }
    return LocatedManifestString{
        .value = value,
        .location = required_location(node),
    };
}

[[nodiscard]] std::expected<std::uint64_t, ManifestError> unsigned_integer(
    const YAML::Node& node,
    const std::uint64_t minimum,
    const std::uint64_t maximum,
    const std::string& path) {
    if (!node.IsScalar() || node.Tag() == "!") {
        return std::unexpected(error(
            ManifestErrorKind::TypeMismatch,
            "manifest value must be an unquoted decimal integer",
            path,
            location_from_mark(node.Mark())));
    }
    const auto& scalar = node.Scalar();
    if (scalar.empty() ||
        (scalar.size() > 1U && scalar.front() == '0') ||
        !std::ranges::all_of(scalar, [](const char value) {
            return value >= '0' && value <= '9';
        })) {
        return std::unexpected(error(
            ManifestErrorKind::InvalidValue,
            "manifest integer must use canonical unsigned decimal syntax",
            path,
            location_from_mark(node.Mark())));
    }
    std::uint64_t value{};
    const auto [end, conversion_error] = std::from_chars(
        scalar.data(), scalar.data() + scalar.size(), value);
    if (conversion_error != std::errc{} || end != scalar.data() + scalar.size() ||
        value < minimum || value > maximum) {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "manifest integer is outside its allowed range",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    return value;
}

[[nodiscard]] bool safe_relative_artifact_path(
    const std::string_view value) noexcept {
    if (value.empty() || value.front() == '/' || value.back() == '/' ||
        value.find('\\') != std::string_view::npos ||
        value.find(':') != std::string_view::npos) {
        return false;
    }
    std::size_t component_begin = 0U;
    while (component_begin < value.size()) {
        const auto separator = value.find('/', component_begin);
        const auto component_end =
            separator == std::string_view::npos ? value.size() : separator;
        const auto component =
            value.substr(component_begin, component_end - component_begin);
        if (component.empty() || component == "." || component == "..") {
            return false;
        }
        for (const auto character : component) {
            const auto byte = static_cast<unsigned char>(character);
            if (byte < 0x20U || byte == 0x7FU) {
                return false;
            }
        }
        if (separator == std::string_view::npos) {
            break;
        }
        component_begin = separator + 1U;
    }
    return true;
}

[[nodiscard]] std::expected<std::string, ManifestError> normalized_sha256(
    const YAML::Node& node,
    const std::string& path) {
    auto value = text_value(node, 64U, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (value->value.size() != 64U ||
        !std::ranges::all_of(value->value, [](const char character) {
            return (character >= '0' && character <= '9') ||
                (character >= 'a' && character <= 'f') ||
                (character >= 'A' && character <= 'F');
        })) {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "artifact sha256 must contain 64 hexadecimal digits",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    std::ranges::transform(value->value, value->value.begin(), [](const char value) {
        return value >= 'A' && value <= 'F'
            ? static_cast<char>(value - 'A' + 'a')
            : value;
    });
    return std::move(value->value);
}

[[nodiscard]] std::expected<std::vector<LocatedManifestString>, ManifestError>
selector_values(const YAML::Node& node,
                const std::string& path,
                const ManifestParseOptions& options) {
    if (!node.IsSequence()) {
        return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                     "selector value must be a sequence",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    if (node.size() == 0U || node.size() > kMaximumManifestSelectorValues) {
        return std::unexpected(error(
            ManifestErrorKind::LimitExceeded,
            "selector sequence count is outside its allowed range",
            path,
            location_from_mark(node.Mark())));
    }
    std::vector<LocatedManifestString> values;
    values.reserve(node.size());
    std::unordered_set<std::string> unique;
    for (std::size_t index = 0U; index < node.size(); ++index) {
        if (const auto interrupted = interruption_error(options, path)) {
            return std::unexpected(*interrupted);
        }
        const auto item_path = path + "[" + std::to_string(index) + "]";
        auto value = text_value(node[index], kMaximumManifestScalarBytes, item_path);
        if (!value) {
            return std::unexpected(value.error());
        }
        if (!unique.insert(value->value).second) {
            return std::unexpected(error(ManifestErrorKind::DuplicateValue,
                                         "selector sequence contains a duplicate value",
                                         item_path,
                                         value->location));
        }
        values.push_back(std::move(*value));
    }
    return values;
}

[[nodiscard]] std::expected<ManifestFlashSlot, ManifestError> flash_slot(
    const YAML::Node& node,
    const std::string& path) {
    auto value = text_value(node, kIdentifierBytes, path);
    if (!value) {
        return std::unexpected(value.error());
    }
    if (value->value == "current") {
        return ManifestFlashSlot::Current;
    }
    if (value->value == "other") {
        return ManifestFlashSlot::Other;
    }
    if (value->value == "all") {
        return ManifestFlashSlot::All;
    }
    if (value->value == "a") {
        return ManifestFlashSlot::A;
    }
    if (value->value == "b") {
        return ManifestFlashSlot::B;
    }
    return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                 "flash slot is not a supported value",
                                 path,
                                 value->location));
}

[[nodiscard]] std::expected<ManifestStep, ManifestError> parse_step(
    const YAML::Node& node,
    const std::string& path,
    const std::unordered_set<std::string>& artifact_ids,
    const ManifestParseOptions& options) {
    constexpr std::array step_keys{"flash"sv,
                                   "erase"sv,
                                   "setActive"sv,
                                   "reboot"sv,
                                   "oem"sv};
    auto outer = strict_map(node, step_keys, {}, path, options);
    if (!outer) {
        return std::unexpected(outer.error());
    }
    if (outer->fields.size() != 1U) {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "manifest step must contain exactly one action",
                                     path,
                                     location_from_mark(node.Mark())));
    }
    const auto& action = outer->fields.front();
    const auto action_path = path + "." + action.key;
    if (action.key == "flash") {
        constexpr std::array allowed{"partition"sv, "artifact"sv, "slot"sv};
        constexpr std::array required{"partition"sv, "artifact"sv};
        auto fields = strict_map(action.value, allowed, required, action_path, options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto partition = text_value(fields->find("partition")->value,
                                    kMaximumManifestScalarBytes,
                                    action_path + ".partition");
        auto artifact = text_value(fields->find("artifact")->value,
                                   kMaximumManifestScalarBytes,
                                   action_path + ".artifact");
        if (!partition) {
            return std::unexpected(partition.error());
        }
        if (!artifact) {
            return std::unexpected(artifact.error());
        }
        if (!artifact_ids.contains(artifact->value)) {
            return std::unexpected(error(ManifestErrorKind::UnknownArtifact,
                                         "flash step references an unknown artifact",
                                         action_path + ".artifact",
                                         artifact->location));
        }
        ManifestFlashStep result{
            .partition = std::move(*partition),
            .artifact = std::move(*artifact),
        };
        if (const auto* slot = fields->find("slot")) {
            auto parsed = flash_slot(slot->value, action_path + ".slot");
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            result.slot = *parsed;
            result.slot_location = required_location(slot->value);
        }
        return ManifestStep{
            .location = required_location(node),
            .payload = std::move(result),
        };
    }
    if (action.key == "erase") {
        constexpr std::array allowed{"partition"sv};
        auto fields = strict_map(action.value, allowed, allowed, action_path, options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto partition = text_value(fields->find("partition")->value,
                                    kMaximumManifestScalarBytes,
                                    action_path + ".partition");
        if (!partition) {
            return std::unexpected(partition.error());
        }
        return ManifestStep{
            .location = required_location(node),
            .payload = ManifestEraseStep{.partition = std::move(*partition)},
        };
    }
    if (action.key == "setActive") {
        constexpr std::array allowed{"slot"sv};
        auto fields = strict_map(action.value, allowed, allowed, action_path, options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        const auto& slot_node = fields->find("slot")->value;
        auto value = text_value(slot_node, kIdentifierBytes, action_path + ".slot");
        if (!value) {
            return std::unexpected(value.error());
        }
        ManifestActiveSlot slot{};
        if (value->value == "a") {
            slot = ManifestActiveSlot::A;
        } else if (value->value == "b") {
            slot = ManifestActiveSlot::B;
        } else if (value->value == "other") {
            slot = ManifestActiveSlot::Other;
        } else {
            return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                         "setActive slot is not a supported value",
                                         action_path + ".slot",
                                         value->location));
        }
        return ManifestStep{
            .location = required_location(node),
            .payload = ManifestSetActiveStep{
                .slot = slot,
                .slot_location = value->location,
            },
        };
    }
    if (action.key == "reboot") {
        constexpr std::array allowed{"target"sv};
        auto fields = strict_map(action.value, allowed, {}, action_path, options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        ManifestRebootStep result;
        if (const auto* target = fields->find("target")) {
            auto value = text_value(target->value,
                                    kIdentifierBytes,
                                    action_path + ".target");
            if (!value) {
                return std::unexpected(value.error());
            }
            if (value->value == "system") {
                result.target = ManifestRebootTarget::System;
            } else if (value->value == "bootloader") {
                result.target = ManifestRebootTarget::Bootloader;
            } else if (value->value == "recovery") {
                result.target = ManifestRebootTarget::Recovery;
            } else if (value->value == "fastboot") {
                result.target = ManifestRebootTarget::Fastboot;
            } else {
                return std::unexpected(error(
                    ManifestErrorKind::InvalidValue,
                    "reboot target is not a supported value",
                    action_path + ".target",
                    value->location));
            }
            result.target_location = value->location;
        }
        return ManifestStep{
            .location = required_location(node),
            .payload = result,
        };
    }

    constexpr std::array allowed{"command"sv};
    auto fields = strict_map(action.value, allowed, allowed, action_path, options);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    auto command = text_value(fields->find("command")->value,
                              kMaximumManifestScalarBytes,
                              action_path + ".command");
    if (!command) {
        return std::unexpected(command.error());
    }
    return ManifestStep{
        .location = required_location(node),
        .payload = ManifestOemStep{.command = std::move(*command)},
    };
}

[[nodiscard]] std::expected<ManifestPolicy, ManifestError> parse_policy(
    const YAML::Node& node,
    const ManifestParseOptions& options) {
    constexpr std::array allowed{"onDeviceFailure"sv,
                                 "maxParallelDevices"sv,
                                 "memoryBudget"sv};
    auto fields = strict_map(node, allowed, {}, "$.policy", options);
    if (!fields) {
        return std::unexpected(fields.error());
    }
    ManifestPolicy result;
    result.location = required_location(node);
    if (const auto* failure = fields->find("onDeviceFailure")) {
        auto value = text_value(failure->value,
                                kIdentifierBytes,
                                "$.policy.onDeviceFailure");
        if (!value) {
            return std::unexpected(value.error());
        }
        if (value->value == "continue") {
            result.on_device_failure = ManifestDeviceFailurePolicy::Continue;
        } else if (value->value == "stop") {
            result.on_device_failure = ManifestDeviceFailurePolicy::Stop;
        } else {
            return std::unexpected(error(
                ManifestErrorKind::InvalidValue,
                "onDeviceFailure is not a supported value",
                "$.policy.onDeviceFailure",
                value->location));
        }
        result.on_device_failure_location = value->location;
    }
    if (const auto* parallel = fields->find("maxParallelDevices")) {
        auto value = unsigned_integer(parallel->value,
                                      kMinimumParallelDevices,
                                      kMaximumParallelDevices,
                                      "$.policy.maxParallelDevices");
        if (!value) {
            return std::unexpected(value.error());
        }
        result.max_parallel_devices = static_cast<std::uint32_t>(*value);
        result.max_parallel_devices_location = required_location(parallel->value);
    }
    if (const auto* memory = fields->find("memoryBudget")) {
        if (memory->value.IsScalar() && memory->value.Scalar() == "auto") {
            result.memory_budget.automatic = true;
            result.memory_budget.bytes = 0U;
            result.memory_budget.location = required_location(memory->value);
        } else {
            auto value = unsigned_integer(memory->value,
                                          kMinimumMemoryBudget,
                                          kMaximumMemoryBudget,
                                          "$.policy.memoryBudget");
            if (!value) {
                return std::unexpected(value.error());
            }
            result.memory_budget.automatic = false;
            result.memory_budget.bytes = *value;
            result.memory_budget.location = required_location(memory->value);
        }
    }
    return result;
}

[[nodiscard]] std::expected<FlashJobManifest, ManifestError> parse_ast(
    const YAML::Node& root,
    const ManifestParseOptions& options) {
    invoke_fault(options, ManifestFaultPoint::AstConstruction);
    if (const auto interrupted = interruption_error(options, "$")) {
        return std::unexpected(*interrupted);
    }
    constexpr std::array allowed{"apiVersion"sv,
                                 "kind"sv,
                                 "artifacts"sv,
                                 "targets"sv,
                                 "policy"sv};
    constexpr std::array required{"apiVersion"sv,
                                  "kind"sv,
                                  "artifacts"sv,
                                  "targets"sv};
    auto root_fields = strict_map(root, allowed, required, "$", options);
    if (!root_fields) {
        return std::unexpected(root_fields.error());
    }
    auto api_version = text_value(root_fields->find("apiVersion")->value,
                                  kMaximumManifestScalarBytes,
                                  "$.apiVersion");
    auto kind = text_value(root_fields->find("kind")->value,
                           kMaximumManifestScalarBytes,
                           "$.kind");
    if (!api_version) {
        return std::unexpected(api_version.error());
    }
    if (!kind) {
        return std::unexpected(kind.error());
    }
    if (api_version->value != "kairosboot.io/v1") {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "unsupported fleet manifest apiVersion",
                                     "$.apiVersion",
                                     api_version->location));
    }
    if (kind->value != "FlashJob") {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "fleet manifest kind must be FlashJob",
                                     "$.kind",
                                     kind->location));
    }

    FlashJobManifest manifest{
        .location = required_location(root),
        .api_version = std::move(*api_version),
        .kind = std::move(*kind),
    };
    const auto& artifacts_node = root_fields->find("artifacts")->value;
    if (!artifacts_node.IsSequence()) {
        return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                     "artifacts must be a sequence",
                                     "$.artifacts",
                                     location_from_mark(artifacts_node.Mark())));
    }
    if (artifacts_node.size() == 0U ||
        artifacts_node.size() > kMaximumManifestArtifacts) {
        return std::unexpected(error(ManifestErrorKind::LimitExceeded,
                                     "artifact count is outside its allowed range",
                                     "$.artifacts",
                                     location_from_mark(artifacts_node.Mark())));
    }
    manifest.artifacts.reserve(artifacts_node.size());
    std::unordered_set<std::string> artifact_ids;
    std::unordered_set<std::string> artifact_paths;
    constexpr std::array artifact_fields{"id"sv, "path"sv, "sha256"sv};
    for (std::size_t index = 0U; index < artifacts_node.size(); ++index) {
        const auto item_path = "$.artifacts[" + std::to_string(index) + "]";
        auto fields = strict_map(artifacts_node[index],
                                 artifact_fields,
                                 artifact_fields,
                                 item_path,
                                 options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto id = text_value(fields->find("id")->value,
                             kIdentifierBytes,
                             item_path + ".id");
        auto path = text_value(fields->find("path")->value,
                               kMaximumManifestScalarBytes,
                               item_path + ".path");
        auto sha256 = normalized_sha256(fields->find("sha256")->value,
                                        item_path + ".sha256");
        if (!id) {
            return std::unexpected(id.error());
        }
        if (!path) {
            return std::unexpected(path.error());
        }
        if (!sha256) {
            return std::unexpected(sha256.error());
        }
        if (!safe_relative_artifact_path(path->value)) {
            return std::unexpected(error(
                ManifestErrorKind::UnsafePath,
                "artifact path is not a strict forward-slash relative path",
                item_path + ".path",
                path->location));
        }
        if (!artifact_ids.insert(id->value).second) {
            return std::unexpected(error(ManifestErrorKind::DuplicateValue,
                                         "artifact id is not unique",
                                         item_path + ".id",
                                         id->location));
        }
        if (!artifact_paths.insert(path->value).second) {
            return std::unexpected(error(ManifestErrorKind::DuplicateValue,
                                         "artifact path is not unique",
                                         item_path + ".path",
                                         path->location));
        }
        manifest.artifacts.push_back(ManifestArtifact{
            .location = required_location(artifacts_node[index]),
            .id = std::move(*id),
            .path = std::move(*path),
            .sha256 = LocatedManifestString{
                .value = std::move(*sha256),
                .location = required_location(fields->find("sha256")->value),
            },
        });
    }

    const auto& targets_node = root_fields->find("targets")->value;
    if (!targets_node.IsSequence()) {
        return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                     "targets must be a sequence",
                                     "$.targets",
                                     location_from_mark(targets_node.Mark())));
    }
    if (targets_node.size() == 0U ||
        targets_node.size() > kMaximumManifestTargets) {
        return std::unexpected(error(ManifestErrorKind::LimitExceeded,
                                     "target count is outside its allowed range",
                                     "$.targets",
                                     location_from_mark(targets_node.Mark())));
    }
    manifest.targets.reserve(targets_node.size());
    std::unordered_set<std::string> target_names;
    std::unordered_map<std::string, std::size_t> selector_owners;
    constexpr std::array target_fields{"name"sv,
                                       "selector"sv,
                                       "expectedProduct"sv,
                                       "steps"sv};
    constexpr std::array selector_fields{"serials"sv, "usbPaths"sv};
    for (std::size_t target_index = 0U;
         target_index < targets_node.size();
         ++target_index) {
        if (const auto interrupted = interruption_error(options, "$.targets")) {
            return std::unexpected(*interrupted);
        }
        const auto target_path =
            "$.targets[" + std::to_string(target_index) + "]";
        const auto& target_node = targets_node[target_index];
        auto fields = strict_map(target_node,
                                 target_fields,
                                 target_fields,
                                 target_path,
                                 options);
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto name = text_value(fields->find("name")->value,
                               kIdentifierBytes,
                               target_path + ".name");
        auto expected_product = text_value(
            fields->find("expectedProduct")->value,
            kMaximumManifestScalarBytes,
            target_path + ".expectedProduct");
        if (!name) {
            return std::unexpected(name.error());
        }
        if (!expected_product) {
            return std::unexpected(expected_product.error());
        }
        if (!target_names.insert(name->value).second) {
            return std::unexpected(error(ManifestErrorKind::DuplicateValue,
                                         "target name is not unique",
                                         target_path + ".name",
                                         name->location));
        }
        const auto& selector_node = fields->find("selector")->value;
        auto selector_map = strict_map(selector_node,
                                       selector_fields,
                                       {},
                                       target_path + ".selector",
                                       options);
        if (!selector_map) {
            return std::unexpected(selector_map.error());
        }
        if (selector_map->fields.empty()) {
            return std::unexpected(error(
                ManifestErrorKind::MissingField,
                "selector must contain serials or usbPaths",
                target_path + ".selector",
                location_from_mark(selector_node.Mark())));
        }
        ManifestSelector selector{
            .location = required_location(selector_node),
        };
        if (const auto* serials = selector_map->find("serials")) {
            auto values = selector_values(serials->value,
                                          target_path + ".selector.serials",
                                          options);
            if (!values) {
                return std::unexpected(values.error());
            }
            selector.serials = std::move(*values);
        }
        if (const auto* usb_paths = selector_map->find("usbPaths")) {
            auto values = selector_values(usb_paths->value,
                                          target_path + ".selector.usbPaths",
                                          options);
            if (!values) {
                return std::unexpected(values.error());
            }
            selector.usb_paths = std::move(*values);
        }
        const auto claim_selector_value = [&](const LocatedManifestString& value)
            -> std::optional<ManifestError> {
            const auto [owner, inserted] =
                selector_owners.emplace(value.value, target_index);
            if (!inserted && owner->second != target_index) {
                return error(
                    ManifestErrorKind::DuplicateValue,
                    "selector value appears in more than one target",
                    target_path + ".selector",
                    value.location);
            }
            return std::nullopt;
        };
        for (const auto& value : selector.serials) {
            if (const auto duplicate = claim_selector_value(value)) {
                return std::unexpected(*duplicate);
            }
        }
        for (const auto& value : selector.usb_paths) {
            if (const auto duplicate = claim_selector_value(value)) {
                return std::unexpected(*duplicate);
            }
        }

        const auto& steps_node = fields->find("steps")->value;
        if (!steps_node.IsSequence()) {
            return std::unexpected(error(ManifestErrorKind::TypeMismatch,
                                         "target steps must be a sequence",
                                         target_path + ".steps",
                                         location_from_mark(steps_node.Mark())));
        }
        if (steps_node.size() == 0U ||
            steps_node.size() > kMaximumManifestSteps) {
            return std::unexpected(error(
                ManifestErrorKind::LimitExceeded,
                "target step count is outside its allowed range",
                target_path + ".steps",
                location_from_mark(steps_node.Mark())));
        }
        std::vector<ManifestStep> steps;
        steps.reserve(steps_node.size());
        for (std::size_t step_index = 0U;
             step_index < steps_node.size();
             ++step_index) {
            auto step = parse_step(
                steps_node[step_index],
                target_path + ".steps[" + std::to_string(step_index) + "]",
                artifact_ids,
                options);
            if (!step) {
                return std::unexpected(step.error());
            }
            steps.push_back(std::move(*step));
        }
        manifest.targets.push_back(ManifestTarget{
            .location = required_location(target_node),
            .name = std::move(*name),
            .selector = std::move(selector),
            .expected_product = std::move(*expected_product),
            .steps = std::move(steps),
        });
    }

    if (const auto* policy = root_fields->find("policy")) {
        auto parsed = parse_policy(policy->value, options);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        manifest.policy = std::move(*parsed);
    }
    if (const auto interrupted = interruption_error(options, "$")) {
        return std::unexpected(*interrupted);
    }
    return manifest;
}

[[nodiscard]] std::expected<FlashJobManifest, ManifestError>
parse_manifest_text(const std::string& input,
                    const ManifestParseOptions& options) {
    if (const auto interrupted = interruption_error(options, "$")) {
        return std::unexpected(*interrupted);
    }
    if (input.find('\0') != std::string::npos) {
        return std::unexpected(error(ManifestErrorKind::InvalidValue,
                                     "fleet manifest contains an embedded NUL",
                                     "$"));
    }
    if (!valid_utf8(input)) {
        return std::unexpected(error(ManifestErrorKind::InvalidUtf8,
                                     "fleet manifest is not valid UTF-8",
                                     "$"));
    }
    if (auto scanned = scan_events(input, options); !scanned) {
        return std::unexpected(scanned.error());
    }
    if (const auto interrupted = interruption_error(options, "$")) {
        return std::unexpected(*interrupted);
    }
    try {
        auto root = YAML::Load(input);
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        NodeTraversal traversal;
        if (auto graph = validate_node_graph(root, traversal, options); !graph) {
            return std::unexpected(graph.error());
        }
        return parse_ast(root, options);
    } catch (const YAML::Exception& exception) {
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        return std::unexpected(error(ManifestErrorKind::Syntax,
                                     "invalid YAML syntax",
                                     "$",
                                     location_from_mark(exception.mark)));
    }
}

[[nodiscard]] ManifestError from_file_error(
    const image::FileSourceError& source_error) {
    auto kind = ManifestErrorKind::Io;
    if (source_error.kind == image::FileSourceErrorKind::InvalidArgument) {
        kind = ManifestErrorKind::InvalidArgument;
    } else if (source_error.kind == image::FileSourceErrorKind::NotFound) {
        kind = ManifestErrorKind::NotFound;
    } else if (source_error.kind == image::FileSourceErrorKind::UnsafePath ||
               source_error.kind == image::FileSourceErrorKind::NotRegularFile) {
        kind = ManifestErrorKind::UnsafePath;
    }
    return error(kind,
                 "unable to open fleet manifest snapshot: " + source_error.message,
                 {},
                 std::nullopt,
                 source_error.native_code);
}

[[nodiscard]] ManifestError out_of_memory_error() {
    return error(ManifestErrorKind::ResourceExhausted,
                 "memory allocation failed while parsing fleet manifest",
                 "$");
}

[[nodiscard]] ManifestError unexpected_error() {
    return error(ManifestErrorKind::UnexpectedFailure,
                 "unexpected exception while parsing fleet manifest",
                 "$");
}

}  // namespace

std::expected<FlashJobManifest, ManifestError>
load_fleet_manifest_file(const std::filesystem::path& path,
                         const ManifestParseOptions& options) {
    try {
        if (const auto interrupted = interruption_error(options)) {
            return std::unexpected(*interrupted);
        }
        auto source = image::FileImageSource::open(path);
        if (!source) {
            return std::unexpected(from_file_error(source.error()));
        }
        if (const auto interrupted = interruption_error(options)) {
            return std::unexpected(*interrupted);
        }
        return parse_fleet_manifest_source(**source, options);
    } catch (const std::bad_alloc&) {
        return std::unexpected(out_of_memory_error());
    } catch (...) {
        return std::unexpected(unexpected_error());
    }
}

std::expected<FlashJobManifest, ManifestError>
parse_fleet_manifest_source(const image::FileImageSource& source,
                            const ManifestParseOptions& options) {
    try {
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        if (source.size() > kMaximumManifestBytes) {
            return std::unexpected(error(
                ManifestErrorKind::TooLarge,
                "fleet manifest exceeds the one MiB input limit",
                "$"));
        }
        auto initial_identity = source.snapshot_identity();
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        if (!initial_identity) {
            return std::unexpected(error(
                ManifestErrorKind::Io,
                "unable to bind the fleet manifest snapshot identity: " +
                    initial_identity.error().message,
                "$",
                std::nullopt,
                initial_identity.error().native_code));
        }
        if (initial_identity->directory ||
            initial_identity->size != source.size()) {
            return std::unexpected(error(
                ManifestErrorKind::Io,
                "fleet manifest source identity does not match its snapshotted size",
                "$"));
        }
        invoke_fault(options, ManifestFaultPoint::InputBuffer);
        std::string input(static_cast<std::size_t>(source.size()), '\0');
        std::size_t completed = 0U;
        while (completed < input.size()) {
            if (const auto interrupted = interruption_error(options, "$")) {
                return std::unexpected(*interrupted);
            }
            const auto amount = std::min(kReadChunkBytes, input.size() - completed);
            auto bytes = std::span<std::byte>{
                reinterpret_cast<std::byte*>(input.data() + completed), amount};
            auto read = source.read_at(completed, bytes);
            if (const auto interrupted = interruption_error(options, "$")) {
                return std::unexpected(*interrupted);
            }
            if (!read) {
                return std::unexpected(error(
                    ManifestErrorKind::Io,
                    "unable to read fleet manifest snapshot: " +
                        read.error().message,
                    "$"));
            }
            if (*read == 0U || *read > amount) {
                return std::unexpected(error(
                    ManifestErrorKind::Io,
                    "fleet manifest snapshot changed or returned an invalid short read",
                    "$"));
            }
            completed += *read;
        }
        auto final_identity = source.snapshot_identity();
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        if (!final_identity) {
            return std::unexpected(error(
                ManifestErrorKind::Io,
                "unable to revalidate the fleet manifest snapshot: " +
                    final_identity.error().message,
                "$",
                std::nullopt,
                final_identity.error().native_code));
        }
        if (*final_identity != *initial_identity) {
            return std::unexpected(error(
                ManifestErrorKind::Io,
                "fleet manifest snapshot identity changed while it was read",
                "$"));
        }
        return parse_manifest_text(input, options);
    } catch (const std::bad_alloc&) {
        return std::unexpected(out_of_memory_error());
    } catch (const YAML::Exception& exception) {
        if (const auto interrupted = interruption_error(options, "$")) {
            return std::unexpected(*interrupted);
        }
        return std::unexpected(error(ManifestErrorKind::Syntax,
                                     "invalid YAML syntax",
                                     "$",
                                     location_from_mark(exception.mark)));
    } catch (...) {
        return std::unexpected(unexpected_error());
    }
}

}  // namespace kairosboot::fleet
