/* Caching utilities for the PolyJIT JIT Compiler
 * 
 * Copyright © 2016 Andreas Simbürger <simbuerg@lairosiel.de>
 * 
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included
 * in all copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES
 * OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE
 * OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
*/ 
#ifndef POLLI_CACHING_H
#define POLLI_CACHING_H

#include <cstdlib>
#include <functional>

namespace polli {
struct CacheKey {
  const char *IR;
  size_t ValueHash;

  CacheKey(const char *IR, size_t ValueHash) : IR(IR), ValueHash(ValueHash) {}

  bool operator==(const CacheKey &O) const {
    return IR == O.IR && ValueHash == O.ValueHash;
  }

  bool operator<(const CacheKey &O) const {
    return IR < O.IR || (IR == O.IR && ValueHash < O.ValueHash);
  }
};
}

namespace std {
template <> struct hash<polli::CacheKey> {
  std::size_t operator()(const polli::CacheKey &K) const {
    size_t h = (size_t)K.IR ^ K.ValueHash;
    return h;
  }
};
}
#endif /* end of include guard: POLLI_CACHING_H */