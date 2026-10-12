// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
namespace Common {
template<class T, class A> constexpr T AlignUp(T value, A alignment) { return (value + alignment - 1) / alignment * alignment; }
template<class T, class A> constexpr T AlignDown(T value, A alignment) { return value / alignment * alignment; }
}
