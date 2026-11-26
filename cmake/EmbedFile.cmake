# cmake -DIN=<file> -DOUT=<header> -DNAME=<identifier> -P EmbedFile.cmake
# Build-time fallback for #embed (compilers without C++26 #embed): writes the file as a constexpr byte array.
if(NOT DEFINED IN OR NOT DEFINED OUT OR NOT DEFINED NAME)
    message(FATAL_ERROR "EmbedFile.cmake needs IN, OUT and NAME")
endif()
file(SIZE "${IN}" _size)
file(READ "${IN}" _hex HEX)
string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," _bytes "${_hex}")
string(REGEX REPLACE ",$" "" _bytes "${_bytes}")
if(_size EQUAL 0)
    set(_size 1)
    set(_bytes "0x00")
endif()
file(WRITE "${OUT}" "// Generated from ${IN} by cmake/EmbedFile.cmake. Do not edit.
#pragma once
#include <array>
#include <cstdint>
inline constexpr std::array<std::uint8_t, ${_size}> ${NAME} = { ${_bytes} };
")
