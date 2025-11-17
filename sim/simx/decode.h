#ifndef DECODE_H
#define DECODE_H

#include <stdint.h>
#include <type_traits>

// Bit field extraction and insertion helpers
#define get_field(reg, mask) \
  (((reg) & (std::remove_cv<decltype(reg)>::type)(mask)) / ((mask) & ~((mask) << 1)))

#define set_field(reg, mask, val) \
  (((reg) & ~(std::remove_cv<decltype(reg)>::type)(mask)) | (((std::remove_cv<decltype(reg)>::type)(val) * ((mask) & ~((mask) << 1))) & (std::remove_cv<decltype(reg)>::type)(mask)))

#endif

