// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <stdexcept>
#define ASSERT(c) do { if (!(c)) throw std::runtime_error("Invalid video state: " #c); } while (0)
#define ASSERT_MSG(c, ...) ASSERT(c)
#define UNREACHABLE() throw std::runtime_error("Unsupported video operation")
#define UNREACHABLE_MSG(...) UNREACHABLE()
