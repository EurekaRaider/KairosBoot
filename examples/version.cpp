#include <kairosboot/kairosboot.hpp>

#include <iostream>

int main() {
  std::cout << "KairosBoot " << kairosboot::version().string << '\n';
  return 0;
}
