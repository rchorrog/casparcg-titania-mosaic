// Force-included into libvmx on non-MSVC compilers (see CMakeLists.txt).
//
// libvmx declares its lookup tables `__declspec(align(16))`, and the SSE paths load them with
// aligned loads. GCC has no __declspec on Linux; libvmx's own Linux build uses clang's
// -fdeclspec instead. This maps the one form it uses onto the GCC attribute, so the tables
// keep their alignment rather than losing it to an ignored attribute.
#pragma once

#define __declspec(x) __declspec_##x
#define __declspec_align(n) __attribute__((aligned(n)))
