#pragma once
#include <cstdio>
#include <cstdlib>
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "%s:%d CHECK failed: %s\n", __FILE__, __LINE__, #c); std::exit(1); } } while (0)
