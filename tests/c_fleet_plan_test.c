/* SPDX-License-Identifier: MIT */
#include <kairosboot/kairosboot.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      fprintf(stderr, "check failed at line %d: %s\n", __LINE__, #condition);   \
      return __LINE__;                                                          \
    }                                                                           \
  } while (0)

/* Test-only SHA-256 oracle (FIPS 180-4). Consumers must not copy this; the
 * library's digest authority lives in the internal image layer. */
typedef struct {
  uint32_t state[8];
  uint64_t bit_count;
  unsigned char block[64];
  size_t block_size;
} sha256_ctx;

static const uint32_t kSha256RoundConstants[64] = {
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

static uint32_t sha256_rotate_right(uint32_t value, unsigned bits) {
  return (value >> bits) | (value << (32U - bits));
}

static void sha256_transform(sha256_ctx *ctx, const unsigned char block[64]) {
  uint32_t w[64];
  for (unsigned i = 0U; i < 16U; ++i) {
    const unsigned offset = i * 4U;
    w[i] = ((uint32_t)block[offset] << 24) |
           ((uint32_t)block[offset + 1U] << 16) |
           ((uint32_t)block[offset + 2U] << 8) |
           (uint32_t)block[offset + 3U];
  }
  for (unsigned i = 16U; i < 64U; ++i) {
    const uint32_t s0 = sha256_rotate_right(w[i - 15U], 7) ^
                        sha256_rotate_right(w[i - 15U], 18) ^ (w[i - 15U] >> 3);
    const uint32_t s1 = sha256_rotate_right(w[i - 2U], 17) ^
                        sha256_rotate_right(w[i - 2U], 19) ^ (w[i - 2U] >> 10);
    w[i] = w[i - 16U] + s0 + w[i - 7U] + s1;
  }
  uint32_t a = ctx->state[0];
  uint32_t b = ctx->state[1];
  uint32_t c = ctx->state[2];
  uint32_t d = ctx->state[3];
  uint32_t e = ctx->state[4];
  uint32_t f = ctx->state[5];
  uint32_t g = ctx->state[6];
  uint32_t h = ctx->state[7];
  for (unsigned i = 0U; i < 64U; ++i) {
    const uint32_t sum1 = sha256_rotate_right(e, 6) ^
                          sha256_rotate_right(e, 11) ^
                          sha256_rotate_right(e, 25);
    const uint32_t choose = (e & f) ^ (~e & g);
    const uint32_t t1 = h + sum1 + choose + kSha256RoundConstants[i] + w[i];
    const uint32_t sum0 = sha256_rotate_right(a, 2) ^
                          sha256_rotate_right(a, 13) ^
                          sha256_rotate_right(a, 22);
    const uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const uint32_t t2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + t1;
    d = c;
    c = b;
    b = a;
    a = t1 + t2;
  }
  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
  ctx->state[5] += f;
  ctx->state[6] += g;
  ctx->state[7] += h;
}

static void sha256_init(sha256_ctx *ctx) {
  ctx->state[0] = 0x6a09e667U;
  ctx->state[1] = 0xbb67ae85U;
  ctx->state[2] = 0x3c6ef372U;
  ctx->state[3] = 0xa54ff53aU;
  ctx->state[4] = 0x510e527fU;
  ctx->state[5] = 0x9b05688cU;
  ctx->state[6] = 0x1f83d9abU;
  ctx->state[7] = 0x5be0cd19U;
  ctx->bit_count = UINT64_C(0);
  ctx->block_size = 0U;
  memset(ctx->block, 0, sizeof(ctx->block));
}

static void sha256_byte(sha256_ctx *ctx, unsigned char value) {
  ctx->block[ctx->block_size] = value;
  ++ctx->block_size;
  if (ctx->block_size == 64U) {
    sha256_transform(ctx, ctx->block);
    ctx->block_size = 0U;
  }
}

static void sha256_update(sha256_ctx *ctx, const unsigned char *data,
                          size_t size) {
  ctx->bit_count += (uint64_t)size * UINT64_C(8);
  while (size > 0U) {
    size_t take = 64U - ctx->block_size;
    if (take > size) {
      take = size;
    }
    memcpy(ctx->block + ctx->block_size, data, take);
    ctx->block_size += take;
    data += take;
    size -= take;
    if (ctx->block_size == 64U) {
      sha256_transform(ctx, ctx->block);
      ctx->block_size = 0U;
    }
  }
}

static void sha256_final(sha256_ctx *ctx, unsigned char digest[32]) {
  const uint64_t bits = ctx->bit_count;
  sha256_byte(ctx, 0x80U);
  while (ctx->block_size != 56U) {
    sha256_byte(ctx, 0x00U);
  }
  for (unsigned i = 0U; i < 8U; ++i) {
    sha256_byte(ctx, (unsigned char)((bits >> (56U - 8U * i)) & 0xFFU));
  }
  for (unsigned i = 0U; i < 8U; ++i) {
    digest[i * 4U] = (unsigned char)((ctx->state[i] >> 24) & 0xFFU);
    digest[i * 4U + 1U] = (unsigned char)((ctx->state[i] >> 16) & 0xFFU);
    digest[i * 4U + 2U] = (unsigned char)((ctx->state[i] >> 8) & 0xFFU);
    digest[i * 4U + 3U] = (unsigned char)(ctx->state[i] & 0xFFU);
  }
}

static void sha256_hex_of(const unsigned char *data, size_t size,
                          char output[65]) {
  static const char digits[] = "0123456789abcdef";
  sha256_ctx ctx;
  unsigned char digest[32];
  sha256_init(&ctx);
  sha256_update(&ctx, data, size);
  sha256_final(&ctx, digest);
  for (unsigned i = 0U; i < 32U; ++i) {
    output[i * 2U] = digits[digest[i] >> 4];
    output[i * 2U + 1U] = digits[digest[i] & 0x0FU];
  }
  output[64] = '\0';
}

static int write_text_file(const char *path, const char *text) {
  FILE *file = fopen(path, "wb");
  if (file == NULL) {
    return 0;
  }
  const size_t length = strlen(text);
  const size_t written = fwrite(text, 1U, length, file);
  if (fclose(file) != 0 || written != length) {
    return 0;
  }
  return 1;
}

static unsigned char *read_file(const char *path, size_t *size) {
  *size = 0U;
  FILE *file = fopen(path, "rb");
  if (file == NULL) {
    return NULL;
  }
  if (fseek(file, 0L, SEEK_END) != 0) {
    fclose(file);
    return NULL;
  }
  const long length = ftell(file);
  if (length < 0 || fseek(file, 0L, SEEK_SET) != 0) {
    fclose(file);
    return NULL;
  }
  unsigned char *buffer = (unsigned char *)malloc((size_t)length);
  if (buffer == NULL) {
    fclose(file);
    return NULL;
  }
  if (length > 0 &&
      fread(buffer, 1U, (size_t)length, file) != (size_t)length) {
    free(buffer);
    fclose(file);
    return NULL;
  }
  fclose(file);
  *size = (size_t)length;
  return buffer;
}

/* Returns 1 when text contains a ":<line>:<column>:" diagnostic fragment. */
static int contains_line_column(const char *text) {
  const size_t length = strlen(text);
  for (size_t i = 0U; i + 1U < length; ++i) {
    if (text[i] != ':') {
      continue;
    }
    size_t line_digits = 0U;
    while (i + 1U + line_digits < length &&
           text[i + 1U + line_digits] >= '0' &&
           text[i + 1U + line_digits] <= '9') {
      ++line_digits;
    }
    if (line_digits == 0U) {
      continue;
    }
    const size_t column_start = i + 1U + line_digits;
    if (column_start >= length || text[column_start] != ':') {
      continue;
    }
    size_t column_digits = 0U;
    while (column_start + 1U + column_digits < length &&
           text[column_start + 1U + column_digits] >= '0' &&
           text[column_start + 1U + column_digits] <= '9') {
      ++column_digits;
    }
    if (column_digits == 0U) {
      continue;
    }
    const size_t after = column_start + 1U + column_digits;
    if (after < length && text[after] == ':') {
      return 1;
    }
  }
  return 0;
}

static int is_lowercase_hex(const char *text) {
  for (const char *cursor = text; *cursor != '\0'; ++cursor) {
    if ((*cursor < '0' || *cursor > '9') && (*cursor < 'a' || *cursor > 'f')) {
      return 0;
    }
  }
  return 1;
}

static const char kValidManifest[] =
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

static const char kSyntaxFailureManifest[] =
    "apiVersion: kairosboot.io/v1\n"
    "kind: [unclosed\n";

static const char kSemanticFailureManifest[] =
    "apiVersion: kairosboot.io/v1\n"
    "kind: FlashJob\n"
    "bogusField: true\n";

int main(void) {
  kb_error_t *error = NULL;
  kb_job_plan_t *plan = NULL;

  /* NULL-handle safety first: release tolerates NULL and getters degrade. */
  kb_job_plan_release(NULL);
  {
    size_t size = 1234U;
    CHECK(kb_job_plan_canonical_json(NULL, &size) == NULL);
    CHECK(size == 0U);
    CHECK(kb_job_plan_canonical_json(NULL, NULL) == NULL);
    CHECK(strcmp(kb_job_plan_sha256_hex(NULL), "") == 0);
  }

  /* NULL argument handling follows the existing entry-point conventions. */
  CHECK(kb_validate_job_file(NULL, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(kb_error_status(error) == KB_E_INVALID_ARGUMENT);
  CHECK(strlen(kb_error_message(error)) > 0U);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_validate_job_file(NULL, NULL) == KB_E_INVALID_ARGUMENT);
  CHECK(kb_plan_job_file(NULL, NULL, &error) == KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(kb_error_status(error) == KB_E_INVALID_ARGUMENT);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_plan_job_file(NULL, &plan, NULL) == KB_E_INVALID_ARGUMENT);

  CHECK(write_text_file("kb_fleet_valid.yaml", kValidManifest));
  CHECK(write_text_file("kb_fleet_syntax.yaml", kSyntaxFailureManifest));
  CHECK(write_text_file("kb_fleet_semantic.yaml", kSemanticFailureManifest));

  /* Valid manifest: validate is context-free and must not report an error. */
  CHECK(kb_validate_job_file("kb_fleet_valid.yaml", &error) == KB_OK);
  CHECK(error == NULL);
  CHECK(kb_validate_job_file("kb_fleet_valid.yaml", NULL) == KB_OK);

  /* Planning resets the output handle before doing any work. */
  plan = (kb_job_plan_t *)(uintptr_t)1;
  CHECK(kb_plan_job_file("kb_fleet_valid.yaml", &plan, &error) == KB_OK);
  CHECK(error == NULL);
  CHECK(plan != NULL);
  {
    size_t json_size = 0U;
    const char *json = kb_job_plan_canonical_json(plan, &json_size);
    CHECK(json != NULL);
    CHECK(json_size > 1U);
    /* Borrowed buffer is NUL-terminated and excludes the terminator. */
    CHECK(json[json_size] == '\0');
    /* Canonical SDK form never ends with a trailing LF. */
    CHECK(json[json_size - 1U] != '\n');
    CHECK(json[0] == '{');
    CHECK(json[json_size - 1U] == '}');
    CHECK(strstr(json, "\"kind\":\"FlashJob\"") != NULL);
    CHECK(strstr(json, "\"schemaVersion\":1") != NULL);

    const char *hex = kb_job_plan_sha256_hex(plan);
    CHECK(hex != NULL);
    CHECK(strlen(hex) == 64U);
    CHECK(is_lowercase_hex(hex));
    /* Digest equals SHA-256 over the canonical JSON bytes, matching the
     * internal implementation's definition of the plan digest. */
    char computed[65];
    sha256_hex_of((const unsigned char *)json, json_size, computed);
    CHECK(strcmp(hex, computed) == 0);

    /* Getter results are stable, immutable borrows for the handle's life. */
    size_t again_size = 0U;
    CHECK(kb_job_plan_canonical_json(plan, &again_size) == json);
    CHECK(again_size == json_size);
    CHECK(kb_job_plan_canonical_json(plan, NULL) == json);
  }

  /* Two plans from one file own independent snapshots. */
  {
    kb_job_plan_t *second = NULL;
    CHECK(kb_plan_job_file("kb_fleet_valid.yaml", &second, &error) == KB_OK);
    CHECK(error == NULL);
    CHECK(second != NULL);
    CHECK(second != plan);
    size_t first_size = 0U;
    size_t second_size = 0U;
    const char *first_json = kb_job_plan_canonical_json(plan, &first_size);
    const char *second_json =
        kb_job_plan_canonical_json(second, &second_size);
    CHECK(first_size == second_size);
    CHECK(strcmp(first_json, second_json) == 0);
    CHECK(strcmp(kb_job_plan_sha256_hex(plan),
                 kb_job_plan_sha256_hex(second)) == 0);
    kb_job_plan_release(plan);
    plan = NULL;
    const char *surviving =
        kb_job_plan_canonical_json(second, &second_size);
    CHECK(surviving == second_json);
    CHECK(strcmp(kb_job_plan_sha256_hex(second),
                 kb_job_plan_sha256_hex(second)) == 0);
    kb_job_plan_release(second);
  }

  /* Missing file: stable I/O status with the native code preserved. */
  CHECK(kb_validate_job_file("kb_fleet_missing.yaml", &error) == KB_E_IO);
  CHECK(error != NULL);
  CHECK(kb_error_status(error) == KB_E_IO);
  CHECK(kb_error_native_code(error) == 2);
  CHECK(strstr(kb_error_message(error), "kb_fleet_missing.yaml") != NULL);
  kb_error_release(error);
  error = NULL;
  plan = (kb_job_plan_t *)(uintptr_t)1;
  CHECK(kb_plan_job_file("kb_fleet_missing.yaml", &plan, &error) == KB_E_IO);
  CHECK(error != NULL);
  CHECK(plan == NULL);
  kb_error_release(error);
  error = NULL;

  /* YAML syntax failure: stable UTF-8 message with path, line, column. */
  CHECK(kb_validate_job_file("kb_fleet_syntax.yaml", &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(kb_error_native_code(error) == 0);
  CHECK(strstr(kb_error_message(error), "kb_fleet_syntax.yaml") != NULL);
  CHECK(contains_line_column(kb_error_message(error)) == 1);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_validate_job_file("kb_fleet_syntax.yaml", NULL) ==
        KB_E_INVALID_ARGUMENT);
  plan = (kb_job_plan_t *)(uintptr_t)1;
  CHECK(kb_plan_job_file("kb_fleet_syntax.yaml", &plan, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(plan == NULL);
  kb_error_release(error);
  error = NULL;

  /* Semantic failure: schema violation at a document path and location. */
  CHECK(kb_validate_job_file("kb_fleet_semantic.yaml", &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(strstr(kb_error_message(error), "kb_fleet_semantic.yaml") != NULL);
  CHECK(strstr(kb_error_message(error), ":3:1:") != NULL);
  CHECK(strstr(kb_error_message(error), "$.bogusField") != NULL);
  kb_error_release(error);
  error = NULL;
  CHECK(kb_plan_job_file("kb_fleet_semantic.yaml", &plan, &error) ==
        KB_E_INVALID_ARGUMENT);
  CHECK(error != NULL);
  CHECK(plan == NULL);
  CHECK(strstr(kb_error_message(error), ":3:1:") != NULL);
  kb_error_release(error);
  error = NULL;

  /* Frozen repository contract: canonical JSON is the plan golden without
   * its single trailing LF and the digest is that byte string's SHA-256. */
  {
    char fixture_path[1024];
    char golden_path[1024];
    const int fixture_written = snprintf(
        fixture_path, sizeof(fixture_path),
        "%s/tests/contracts/fleet-job-v1.fixture.yaml",
        KAIROSBOOT_TEST_SOURCE_DIR);
    const int golden_written = snprintf(
        golden_path, sizeof(golden_path),
        "%s/tests/contracts/job-plan-v1.golden.json",
        KAIROSBOOT_TEST_SOURCE_DIR);
    CHECK(fixture_written > 0 &&
          (size_t)fixture_written < sizeof(fixture_path));
    CHECK(golden_written > 0 && (size_t)golden_written < sizeof(golden_path));

    plan = NULL;
    CHECK(kb_plan_job_file(fixture_path, &plan, &error) == KB_OK);
    CHECK(error == NULL);
    CHECK(plan != NULL);
    size_t golden_size = 0U;
    unsigned char *golden = read_file(golden_path, &golden_size);
    CHECK(golden != NULL);
    CHECK(golden_size > 1U);
    CHECK(golden[golden_size - 1U] == '\n');
    CHECK(golden[golden_size - 2U] != '\n');
    size_t json_size = 0U;
    const char *json = kb_job_plan_canonical_json(plan, &json_size);
    CHECK(json != NULL);
    CHECK(json_size == golden_size - 1U);
    CHECK(memcmp(json, golden, json_size) == 0);
    /* Fixture provenance digest travels inside the canonical JSON. */
    CHECK(strstr(json,
                 "58539b1d8a0ba3108ffd0f0ea835d25efca9a6ce85b06cd15f0f1307d4b"
                 "1c9ef") != NULL);
    char computed[65];
    sha256_hex_of(golden, golden_size - 1U, computed);
    CHECK(strcmp(kb_job_plan_sha256_hex(plan), computed) == 0);
    CHECK(strcmp(computed,
                 "992daa21b5ea246910fc5d9ffffafed3e36e883d6a407b70abe3b04def382"
                 "3f4") == 0);
    kb_job_plan_release(plan);
    plan = NULL;
    free(golden);
  }

  return 0;
}
