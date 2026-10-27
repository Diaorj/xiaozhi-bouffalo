#pragma once
#include <utility>
#ifdef XIAOZHI_PLATFORM_BOUFFALO
#include <tl/expected.hpp>
namespace xiaozhi {
template<class T, class E> using Expected = tl::expected<T, E>;
template<class E> auto Unexpected(E&& error) { return tl::make_unexpected(std::forward<E>(error)); }
}
#else
#include <expected>
namespace xiaozhi {
template<class T, class E> using Expected = std::expected<T, E>;
template<class E> auto Unexpected(E&& error) { return std::unexpected(std::forward<E>(error)); }
}
#endif
