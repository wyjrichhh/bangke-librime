// SPDX-License-Identifier: BSD-3-Clause
#ifndef RIME_COMPAT_FILESYSTEM_H_
#define RIME_COMPAT_FILESYSTEM_H_
#if defined(__GNUC__) && !defined(__clang__) && __GNUC__ < 8
#include <experimental/filesystem>
#define RIME_LEGACY_FILESYSTEM 1
namespace std { namespace filesystem = experimental::filesystem; }
namespace rime { namespace fs = std::experimental::filesystem; }
#else
#include <filesystem>
namespace rime { namespace fs = std::filesystem; }
#endif
#endif
