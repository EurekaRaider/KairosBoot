#include <kairosboot/kairosboot.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::cerr << "check failed at line " << __LINE__ << ": " #condition    \
                << '\n';                                                       \
      return __LINE__;                                                         \
    }                                                                          \
  } while (false)

static_assert(__cplusplus >= 202100L);
static_assert(!std::is_copy_constructible_v<kairosboot::JobPlan>);
static_assert(!std::is_copy_assignable_v<kairosboot::JobPlan>);
static_assert(std::is_nothrow_move_constructible_v<kairosboot::JobPlan>);
static_assert(std::is_nothrow_move_assignable_v<kairosboot::JobPlan>);
static_assert(requires(const kairosboot::JobPlan &plan,
                       const std::filesystem::path &path) {
  { plan.canonical_json() } noexcept -> std::same_as<std::string_view>;
  { plan.sha256_hex() } noexcept -> std::same_as<std::string_view>;
  { kairosboot::validate_job_file(path) } ->
      std::same_as<std::expected<void, kairosboot::Error>>;
  { kairosboot::plan_job_file(path) } ->
      std::same_as<std::expected<kairosboot::JobPlan, kairosboot::Error>>;
});

namespace {

constexpr std::string_view kValidManifest =
    "apiVersion: kairosboot.io/v1\n"
    "kind: FlashJob\n"
    "artifacts:\n"
    "  - id: system\n"
    "    path: images/system.img\n"
    "    sha256: \"11111111111111111111111111111111"
    "11111111111111111111111111111111\"\n"
    "targets:\n"
    "  - name: product-a\n"
    "    selector:\n"
    "      serials: [SERIAL-01]\n"
    "    expectedProduct: product_a\n"
    "    steps:\n"
    "      - flash:\n"
    "          partition: system\n"
    "          artifact: system\n"
    "policy:\n"
    "  onDeviceFailure: continue\n"
    "  maxParallelDevices: 32\n"
    "  memoryBudget: auto\n";

constexpr std::string_view kSyntaxFailureManifest =
    "apiVersion: kairosboot.io/v1\n"
    "kind: [unclosed\n";

constexpr std::string_view kSemanticFailureManifest =
    "apiVersion: kairosboot.io/v1\n"
    "kind: FlashJob\n"
    "bogusField: true\n";

/* Test-only SHA-256 oracle (FIPS 180-4). Consumers must not copy this; the
 * library's digest authority lives in the internal image layer. */
class Sha256Oracle final {
public:
  Sha256Oracle() noexcept { reset(); }

  void reset() noexcept {
    state_[0] = 0x6a09e667U;
    state_[1] = 0xbb67ae85U;
    state_[2] = 0x3c6ef372U;
    state_[3] = 0xa54ff53aU;
    state_[4] = 0x510e527fU;
    state_[5] = 0x9b05688cU;
    state_[6] = 0x1f83d9abU;
    state_[7] = 0x5be0cd19U;
    bit_count_ = 0U;
    block_size_ = 0U;
  }

  void update(const std::string_view data) {
    bit_count_ += static_cast<std::uint64_t>(data.size()) * UINT64_C(8);
    for (const char value : data) {
      block_[block_size_++] = static_cast<std::uint8_t>(value);
      if (block_size_ == 64U) {
        transform();
        block_size_ = 0U;
      }
    }
  }

  std::string hex_digest() {
    const std::uint64_t bits = bit_count_;
    push_byte(0x80U);
    while (block_size_ != 56U) {
      push_byte(0x00U);
    }
    for (unsigned i = 0U; i < 8U; ++i) {
      push_byte(static_cast<std::uint8_t>((bits >> (56U - 8U * i)) & 0xFFU));
    }
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string hex(64U, '0');
    for (unsigned i = 0U; i < 8U; ++i) {
      const std::uint32_t word = state_[i];
      hex[i * 8U + 0U] = kDigits[(word >> 28) & 0xFU];
      hex[i * 8U + 1U] = kDigits[(word >> 24) & 0xFU];
      hex[i * 8U + 2U] = kDigits[(word >> 20) & 0xFU];
      hex[i * 8U + 3U] = kDigits[(word >> 16) & 0xFU];
      hex[i * 8U + 4U] = kDigits[(word >> 12) & 0xFU];
      hex[i * 8U + 5U] = kDigits[(word >> 8) & 0xFU];
      hex[i * 8U + 6U] = kDigits[(word >> 4) & 0xFU];
      hex[i * 8U + 7U] = kDigits[word & 0xFU];
    }
    return hex;
  }

private:
  static constexpr std::uint32_t kRoundConstants[64] = {
      0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU,
      0x59f111f1U, 0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U,
      0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U,
      0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU,
      0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U,
      0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
      0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
      0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U,
      0xa2bfe8a1U, 0xa81a664bU, 0xc24b8b70U, 0xc76c51a3U, 0xd192e819U,
      0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U, 0x1e376c08U,
      0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU,
      0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
      0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U};

  static std::uint32_t rotate_right(const std::uint32_t value,
                                    const unsigned bits) noexcept {
    return (value >> bits) | (value << (32U - bits));
  }

  void push_byte(const std::uint8_t value) {
    block_[block_size_++] = value;
    if (block_size_ == 64U) {
      transform();
      block_size_ = 0U;
    }
  }

  void transform() noexcept {
    std::uint32_t w[64];
    for (unsigned i = 0U; i < 16U; ++i) {
      const unsigned offset = i * 4U;
      w[i] = (static_cast<std::uint32_t>(block_[offset]) << 24) |
             (static_cast<std::uint32_t>(block_[offset + 1U]) << 16) |
             (static_cast<std::uint32_t>(block_[offset + 2U]) << 8) |
             static_cast<std::uint32_t>(block_[offset + 3U]);
    }
    for (unsigned i = 16U; i < 64U; ++i) {
      const std::uint32_t s0 = rotate_right(w[i - 15U], 7) ^
                               rotate_right(w[i - 15U], 18) ^ (w[i - 15U] >> 3);
      const std::uint32_t s1 = rotate_right(w[i - 2U], 17) ^
                               rotate_right(w[i - 2U], 19) ^ (w[i - 2U] >> 10);
      w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
    }
    std::uint32_t a = state_[0];
    std::uint32_t b = state_[1];
    std::uint32_t c = state_[2];
    std::uint32_t d = state_[3];
    std::uint32_t e = state_[4];
    std::uint32_t f = state_[5];
    std::uint32_t g = state_[6];
    std::uint32_t h = state_[7];
    for (unsigned i = 0U; i < 64U; ++i) {
      const std::uint32_t sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^
                                 rotate_right(e, 25);
      const std::uint32_t choose = (e & f) ^ (~e & g);
      const std::uint32_t t1 = h + sum1 + choose + kRoundConstants[i] + w[i];
      const std::uint32_t sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^
                                 rotate_right(a, 22);
      const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
      const std::uint32_t t2 = sum0 + majority;
      h = g;
      g = f;
      f = e;
      e = d + t1;
      d = c;
      c = b;
      b = a;
      a = t1 + t2;
    }
    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
  }

  std::uint32_t state_[8];
  std::uint64_t bit_count_{0U};
  std::uint8_t block_[64]{};
  std::size_t block_size_{0U};
};

std::string sha256_hex_of(const std::string_view data) {
  Sha256Oracle oracle;
  oracle.update(data);
  return oracle.hex_digest();
}

bool write_text_file(const std::filesystem::path &path,
                     const std::string_view text) {
  std::ofstream file{path, std::ios::binary | std::ios::trunc};
  if (!file.is_open()) {
    return false;
  }
  file.write(text.data(), static_cast<std::streamsize>(text.size()));
  return file.good();
}

std::optional<std::string> read_text_file(const std::filesystem::path &path) {
  std::ifstream file{path, std::ios::binary};
  if (!file.is_open()) {
    return std::nullopt;
  }
  return std::string{std::istreambuf_iterator<char>{file},
                     std::istreambuf_iterator<char>{}};
}

/* Returns true when text contains a ":<line>:<column>:" diagnostic fragment. */
bool contains_line_column(const std::string_view text) {
  for (std::size_t i = 0U; i + 1U < text.size(); ++i) {
    if (text[i] != ':') {
      continue;
    }
    std::size_t line_digits = 0U;
    while (i + 1U + line_digits < text.size() &&
           text[i + 1U + line_digits] >= '0' &&
           text[i + 1U + line_digits] <= '9') {
      ++line_digits;
    }
    if (line_digits == 0U) {
      continue;
    }
    const std::size_t column_start = i + 1U + line_digits;
    if (column_start >= text.size() || text[column_start] != ':') {
      continue;
    }
    std::size_t column_digits = 0U;
    while (column_start + 1U + column_digits < text.size() &&
           text[column_start + 1U + column_digits] >= '0' &&
           text[column_start + 1U + column_digits] <= '9') {
      ++column_digits;
    }
    if (column_digits == 0U) {
      continue;
    }
    const std::size_t after = column_start + 1U + column_digits;
    if (after < text.size() && text[after] == ':') {
      return true;
    }
  }
  return false;
}

bool is_lowercase_hex(const std::string_view text) {
  for (const char value : text) {
    if ((value < '0' || value > '9') && (value < 'a' || value > 'f')) {
      return false;
    }
  }
  return true;
}

} // namespace

int main() {
  /* RAII safety first: a null-handle wrapper is inert and its destructor is
   * harmless because the C release call tolerates NULL. */
  {
    const kairosboot::JobPlan empty{nullptr};
    CHECK(empty.canonical_json().empty());
    CHECK(empty.sha256_hex().empty());
  }

  CHECK(write_text_file("kb_cxx_fleet_valid.yaml", kValidManifest));
  CHECK(write_text_file("kb_cxx_fleet_syntax.yaml", kSyntaxFailureManifest));
  CHECK(write_text_file("kb_cxx_fleet_semantic.yaml", kSemanticFailureManifest));

  const std::filesystem::path valid_path{"kb_cxx_fleet_valid.yaml"};

  /* Valid manifest: validate is context-free and reports no error. */
  const auto validated = kairosboot::validate_job_file(valid_path);
  CHECK(validated.has_value());

  /* Planning hands back a RAII-owned immutable snapshot. */
  auto planned = kairosboot::plan_job_file(valid_path);
  CHECK(planned.has_value());

  /* Borrowed canonical JSON: object form, no trailing LF, plan facts. */
  const std::string_view json = planned->canonical_json();
  CHECK(json.size() > 1U);
  CHECK(json.front() == '{');
  CHECK(json.back() == '}');
  CHECK(json.find("\"kind\":\"FlashJob\"") != std::string_view::npos);
  CHECK(json.find("\"schemaVersion\":1") != std::string_view::npos);

  /* Digest: 64 lowercase hex chars equal to SHA-256 over the borrowed JSON. */
  const std::string_view hex = planned->sha256_hex();
  CHECK(hex.size() == 64U);
  CHECK(is_lowercase_hex(hex));
  CHECK(sha256_hex_of(json) == hex);

  /* Getter borrows are stable for the handle's life: same memory returned. */
  CHECK(planned->canonical_json().data() == json.data());
  CHECK(planned->canonical_json().size() == json.size());
  CHECK(planned->sha256_hex().data() == hex.data());

  /* Independent snapshots: scope exit of one plan leaves the other intact. */
  {
    auto second = kairosboot::plan_job_file(valid_path);
    CHECK(second.has_value());
    CHECK(second->canonical_json() == json);
    CHECK(second->sha256_hex() == hex);
  }
  CHECK(planned->canonical_json() == json);
  CHECK(planned->sha256_hex() == hex);

  /* Move construction transfers ownership; the moved-from plan degrades to
   * empty borrows instead of touching released memory. */
  kairosboot::JobPlan moved{std::move(*planned)};
  CHECK(moved.canonical_json() == json);
  CHECK(moved.sha256_hex() == hex);
  CHECK(planned->canonical_json().empty());
  CHECK(planned->sha256_hex().empty());

  /* Move assignment releases the target's previous plan before stealing:
   * the old borrows die with the released handle, so compare against a
   * copy taken while the first plan was still alive. */
  {
    const std::string expected_hex{hex};
    auto replacement = kairosboot::plan_job_file(valid_path);
    CHECK(replacement.has_value());
    const std::string_view replacement_json = replacement->canonical_json();
    CHECK(replacement_json == json);
    moved = std::move(*replacement);
    CHECK(moved.canonical_json() == replacement_json);
    CHECK(moved.sha256_hex() == expected_hex);
    CHECK(replacement->canonical_json().empty());
  }

  /* Missing file: stable I/O status with the platform native code kept. */
  const std::filesystem::path missing_path{"kb_cxx_fleet_missing.yaml"};
  const auto missing = kairosboot::validate_job_file(missing_path);
  CHECK(!missing.has_value());
  CHECK(missing.error().status() == KB_E_IO);
  CHECK(missing.error().native_code() == 2);
  CHECK(missing.error().message().find("kb_cxx_fleet_missing.yaml") !=
        std::string::npos);
  const auto missing_plan = kairosboot::plan_job_file(missing_path);
  CHECK(!missing_plan.has_value());
  CHECK(missing_plan.error().status() == KB_E_IO);
  CHECK(missing_plan.error().native_code() == 2);

  /* Empty path: std::filesystem::path cannot express NULL, and the loader
   * rejects the empty path as an invalid argument before touching the
   * filesystem. The C ABI keeps its own NULL-path rejection. */
  const auto empty_path =
      kairosboot::validate_job_file(std::filesystem::path{});
  CHECK(!empty_path.has_value());
  CHECK(empty_path.error().status() == KB_E_INVALID_ARGUMENT);
  const auto empty_path_plan =
      kairosboot::plan_job_file(std::filesystem::path{});
  CHECK(!empty_path_plan.has_value());
  CHECK(empty_path_plan.error().status() == KB_E_INVALID_ARGUMENT);

  /* YAML syntax failure: message carries the path plus :line:column:. */
  const std::filesystem::path syntax_path{"kb_cxx_fleet_syntax.yaml"};
  const auto syntax = kairosboot::validate_job_file(syntax_path);
  CHECK(!syntax.has_value());
  CHECK(syntax.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(syntax.error().message().find("kb_cxx_fleet_syntax.yaml") !=
        std::string::npos);
  CHECK(contains_line_column(syntax.error().message()));
  const auto syntax_plan = kairosboot::plan_job_file(syntax_path);
  CHECK(!syntax_plan.has_value());
  CHECK(syntax_plan.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(contains_line_column(syntax_plan.error().message()));

  /* Semantic failure: schema violation naming the document path and spot. */
  const std::filesystem::path semantic_path{"kb_cxx_fleet_semantic.yaml"};
  const auto semantic = kairosboot::validate_job_file(semantic_path);
  CHECK(!semantic.has_value());
  CHECK(semantic.error().status() == KB_E_INVALID_ARGUMENT);
  CHECK(semantic.error().message().find("kb_cxx_fleet_semantic.yaml") !=
        std::string::npos);
  CHECK(semantic.error().message().find(":3:1:") != std::string::npos);
  CHECK(semantic.error().message().find("$.bogusField") != std::string::npos);
  const auto semantic_plan = kairosboot::plan_job_file(semantic_path);
  CHECK(!semantic_plan.has_value());
  CHECK(semantic_plan.error().message().find("$.bogusField") !=
        std::string::npos);

  /* Frozen repository contract: canonical JSON is the plan golden without
   * its single trailing LF and the digest is that byte string's SHA-256. */
  {
    const std::filesystem::path fixture_path{
        std::string{KAIROSBOOT_TEST_SOURCE_DIR} +
        "/tests/contracts/fleet-job-v1.fixture.yaml"};
    const std::filesystem::path golden_path{
        std::string{KAIROSBOOT_TEST_SOURCE_DIR} +
        "/tests/contracts/job-plan-v1.golden.json"};
    const auto golden = read_text_file(golden_path);
    CHECK(golden.has_value());
    CHECK(golden->size() > 1U);
    CHECK((*golden)[golden->size() - 1U] == '\n');
    CHECK((*golden)[golden->size() - 2U] != '\n');

    auto golden_plan = kairosboot::plan_job_file(fixture_path);
    CHECK(golden_plan.has_value());
    const std::string_view golden_json = golden_plan->canonical_json();
    CHECK(golden_json.size() == golden->size() - 1U);
    CHECK(golden_json ==
          std::string_view{*golden}.substr(0, golden->size() - 1U));
    /* Fixture provenance digest travels inside the canonical JSON. */
    CHECK(golden_json.find("58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd1"
                           "5f0f1307d4b1c9ef") != std::string_view::npos);
    CHECK(sha256_hex_of(golden_json) == golden_plan->sha256_hex());
    CHECK(golden_plan->sha256_hex() ==
          "992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def3823f4");
  }

  return 0;
}
