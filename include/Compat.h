// c++14 compatibility with some c++17 features and higher.
// This helps minimize changes from the default branch.

#ifndef COMPAT_H
#define COMPAT_H

#ifdef __cplusplus

namespace Sci {

// std::clamp
template <typename T>
inline constexpr T clamp(T val, T minVal, T maxVal) {
	return (val > maxVal) ? maxVal : ((val < minVal) ? minVal : val);
}

// std::size
template <typename T, size_t N>
constexpr size_t size(const T (&)[N]) noexcept {
  return N;
}

}

#endif

#endif
