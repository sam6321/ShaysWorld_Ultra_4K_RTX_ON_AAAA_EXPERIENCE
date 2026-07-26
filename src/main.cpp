#include "app.h"

#include <cstdio>
#include <exception>

int main() {
  App app;
  try {
    if (!app.init()) {
      std::fprintf(stderr, "Failed to initialize Shays World VK\n");
      return 1;
    }
    app.run();
    app.shutdown();
  } catch (const std::exception& e) {
    std::fprintf(stderr, "Fatal: %s\n", e.what());
    app.shutdown();
    return 1;
  }
  return 0;
}
