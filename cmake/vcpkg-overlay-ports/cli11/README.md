# CLI11 C++20 build

This overlay retains the pinned CLI11 2.4.0 source and builds its precompiled
library in C++20 mode, matching OpenPoseidon. With clang-cl 22 and MSVC 14.51,
the default older language mode crashed during `CLI::App` construction: the
precompiled code attempted runtime initialization of `valid_alias_name_string`'s
inline static string while C++20 consumers placed it in read-only storage.

The port files are adapted from Microsoft's vcpkg tree
`396b497f39aff89e8cbfec62a6e2ed8bd5df73a0` (CLI11 2.4.0).
Their MIT license is included in `LICENSE-vcpkg.txt`. CLI11 itself remains
BSD-3-Clause licensed, with its license installed by the port.
