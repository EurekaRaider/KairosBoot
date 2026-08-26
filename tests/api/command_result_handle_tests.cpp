// SPDX-License-Identifier: MIT
#include "src/api/command_result_handle.hpp"

#include <cstddef>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#define CHECK(condition)                                                        \
  do {                                                                          \
    if (!(condition)) {                                                         \
      throw std::runtime_error(std::string("check failed at line ") +          \
                               std::to_string(__LINE__) + ": " #condition);    \
    }                                                                           \
  } while (false)

int main() {
  try {
    auto payload =
        std::make_shared<kairosboot::api::CommandResultPayload>();
    payload->data.resize(8U * 1024U * 1024U, std::byte{0x5a});
    const auto *original_payload = payload.get();
    const auto *original_data = payload->data.data();
    std::weak_ptr<const kairosboot::api::CommandResultPayload> lifetime = payload;

    {
      kairosboot::api::CommandResultHandle handle(payload);
      CHECK(handle.get() == original_payload);
      CHECK(handle.get()->data.data() == original_data);

      payload.reset();
      CHECK(!lifetime.expired());
      CHECK(handle.get() == original_payload);
      CHECK(handle.get()->data.data() == original_data);
      CHECK(handle.get()->data.back() == std::byte{0x5a});
    }

    CHECK(lifetime.expired());
    std::cout << "PASS: command result extraction retains without copying\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "FAIL: " << error.what() << '\n';
    return 1;
  }
}
