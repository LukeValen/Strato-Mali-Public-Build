// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <vector>
#include <stdexcept>
namespace Common {
template<class T> class ScratchBuffer : public std::vector<T> {
public:
    using std::vector<T>::vector;
    void resize(size_t n) {
        if (n > (64 * 1024 * 1024) / sizeof(T)) throw std::runtime_error("Video scratch buffer too large");
        std::vector<T>::resize(n);
    }
    void resize_destructive(size_t n) { resize(n); }
};
}
