// c++14 compatibility with some c++17 features and higher.
// This helps minimize changes from the default branch.

#ifndef COMPAT_H
#define COMPAT_H

#ifdef __cplusplus

#include <cmath>
#include <cstddef>
#include <cstring>
#include <algorithm>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace Scintilla {
namespace Internal {
namespace Compat {

// std::clamp
template <typename T>
inline constexpr T clamp(T val, T minVal, T maxVal) {
    return (val > maxVal) ? maxVal : ((val < minVal) ? minVal : val);
}

// std::make_unique
template<class T> struct _Unique_if {
    typedef std::unique_ptr<T> _Single_object;
};
template<class T> struct _Unique_if<T[]> {
    typedef std::unique_ptr<T[]> _Unknown_bound;
};
template<class T, size_t N> struct _Unique_if<T[N]> {
    typedef void _Known_bound;
};
template<class T, class... Args>
typename _Unique_if<T>::_Single_object
    make_unique(Args&&... args) {
    return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}
template<class T>
typename _Unique_if<T>::_Unknown_bound
    make_unique(size_t n) {
    typedef typename std::remove_extent<T>::type U;
    return std::unique_ptr<T>(new U[n]());
}
template<class T, class... Args>
typename _Unique_if<T>::_Known_bound
    make_unique(Args&&...) = delete;

// std::size
template <typename T, size_t N>
constexpr size_t size(const T(&)[N]) noexcept {
    return N;
}

// std::string_view
template<typename T>
class basic_string_view // Note: does not implement all the features of std::string_view.
{
    const T* p;
    std::size_t n;
public:
    typedef const T* iterator;
    typedef const T* const_iterator;

    static constexpr std::size_t npos = std::size_t(-1);
    constexpr basic_string_view() : p(nullptr), n(0) {}
    constexpr basic_string_view(const basic_string_view& other) = default;
    constexpr basic_string_view(const T* p, std::size_t n) : p(p), n(n) {}
    constexpr basic_string_view(const T* s) : p(s), n(0) { while (*s++) n++; }
    constexpr basic_string_view(const std::string& s) : p(s.c_str()), n(s.size()) {}
    constexpr const T* begin() const noexcept { return p; }
    constexpr const T* cbegin() const noexcept { return p; }
    constexpr const T* end() const noexcept { return p + n; }
    constexpr const T* cend() const noexcept { return p + n; }
    constexpr const T& operator[](std::size_t i) const noexcept { return p[i]; }
    constexpr const T& front() const { return p[0]; }
    constexpr const T& back() const { return p[n - 1]; }
    constexpr const T* data() const noexcept { return p; }
    constexpr std::size_t size() const noexcept { return n; }
    constexpr std::size_t length() const noexcept { return n; }
    constexpr bool empty() const noexcept { return n == 0; }
    constexpr void remove_prefix(std::size_t k) { p += k; n -= k; }
    constexpr void remove_suffix(std::size_t k) { n -= k; }
    void swap(basic_string_view& other) noexcept { std::swap(p, other.p); std::swap(n, other.n); }
    std::size_t copy(T* dest, std::size_t k, std::size_t pos = 0) const { basic_string_view v(substr(pos, k)); for (std::size_t i = 0; i < v.n; i++) dest[i] = v.p[i]; return v.n; }
    constexpr basic_string_view substr(std::size_t pos = 0, std::size_t k = npos) const { if (pos > n) throw std::out_of_range(""); return basic_string_view(p + pos, std::min(k, n - pos)); }
    constexpr std::size_t find_first_of(basic_string_view v, std::size_t pos = 0) const noexcept { const char* it = std::find_first_of(p + pos, end(), v.begin(), v.end()); return it == end() ? npos : it - p; }
    constexpr std::size_t find_first_of(T c, std::size_t pos = 0) const noexcept { for (std::size_t i = 0; i < n; i++) if (p[i] == c) return i; return npos; }
    constexpr std::size_t find_first_of(const T* s, std::size_t pos, std::size_t k) const { return find_first_of(basic_string_view(s, k), pos); }
    constexpr std::size_t find_first_of(const T* s, std::size_t pos = 0) const { return find_first_of(basic_string_view(s), pos); }

    // custom
    constexpr explicit operator std::basic_string<T>() const { return std::basic_string<T>(p, n); }
    constexpr std::string as_string() const { return std::basic_string<T>(p, n); }
};
typedef basic_string_view<char> string_view;
typedef basic_string_view<wchar_t> wstring_view;

// std::optional

template <class, template <class...> class> struct IsSpecialization : std::false_type {};
template <template <class...> class Template, class... Ts> struct IsSpecialization<Template<Ts...>, Template> : std::true_type {};

//struct in_place_t { explicit in_place_t() = default; };
//inline constexpr in_place_t in_place{};

struct nullopt_t { constexpr explicit nullopt_t(int) {} };
constexpr nullopt_t nullopt{-1};

class bad_optional_access : std::exception { };

template<typename T>
class optional // Note: does not implement all the features of std::optional; especially not all the correct SFINAE rules.
{
    alignas(T) unsigned char buf[sizeof(T)];
    bool present;
    constexpr T& obj() const { return *(T*)buf; }

    template <typename U>
    using CanCtorConvert =
        std::bool_constant<
            std::conjunction<
                std::negation<std::is_same<typename std::remove_cv<typename std::remove_reference<U>::type>::type, optional>>,
                //std::negation<std::is_same<typename std::remove_cv<typename std::remove_reference<U>::type>::type, in_place_t>>,
                std::negation<std::conjunction<std::is_same<typename std::remove_cv<T>::type, bool>, IsSpecialization<typename std::remove_cv<typename std::remove_reference<U>::type>::type, optional>>>,
                std::is_constructible<T, U>
            >::value
        >;
public:
    constexpr optional() : present(false) { }
    constexpr optional(nullopt_t) : present(false) { }
    constexpr optional(const optional& other) : present(other.present) { if (present) new(buf) T(other.obj()); }
    constexpr optional(optional&& other) noexcept(std::is_nothrow_move_constructible<T>::value) : present(other.present) { if (present) new(buf) T(std::move(other.obj())); }
    template <typename U = T, typename std::enable_if<std::conjunction<CanCtorConvert<U>, std::is_convertible<U, T>>::value, int>::type = 0> constexpr optional(U&& a) : present(true) { new(buf) T(std::forward<U>(a)); }
    template <typename U = T, typename std::enable_if<std::conjunction<CanCtorConvert<U>, std::negation<std::is_convertible<U, T>>>::value, int>::type = 0> constexpr explicit optional(U&& a) : present(true) { new(buf) T(std::forward<U>(a)); }
    ~optional() { if (present) obj().~T(); }
    constexpr optional& operator=(nullopt_t) { reset(); return *this; }
    constexpr optional& operator=(const optional& other) { if (this == &other) return *this; if (!other.present) reset(); else if (present) obj() = other.obj(); else { new(buf) T(other.obj()); present = true; } return *this; }
    constexpr optional& operator=(optional&& other) noexcept(std::is_nothrow_move_assignable<T>::value && std::is_nothrow_move_constructible<T>::value) { if (!other.present) reset(); else if (present) obj() = std::move(other.obj()); else { new(buf) T(std::move(other.obj())); present = true; } return *this; }

    template<
        typename U = T,
        typename std::enable_if<std::conjunction<
            std::negation<std::is_same<optional, typename std::remove_cv<typename std::remove_reference<U>::type>::type>>,
            std::negation<std::conjunction<std::is_scalar<T>, std::is_same<T, typename std::decay<U>::type>>>,
            std::is_constructible<T, U>,
            std::is_assignable<T&, U>>::value,
        int>::type = 0>
    optional& operator=(U&& a) noexcept(std::is_nothrow_assignable_v<T&, U> && std::is_nothrow_constructible_v<T, U>) {
        reset();
        new(buf) T(std::forward<U>(a));
        present = true;
        return *this;
    }

    constexpr const T* operator->() const noexcept { return &obj(); }
    constexpr T* operator->() noexcept { return &obj(); }
    constexpr const T& operator*() const & noexcept { return obj(); }
    constexpr T& operator*() & noexcept { return obj(); }
    constexpr const T&& operator*() const && noexcept { return obj(); }
    constexpr T&& operator*() && noexcept { return obj(); }
    constexpr explicit operator bool() const noexcept { return present; }
    constexpr bool has_value() const noexcept { return present; }
    constexpr T& value() & { if (!present) throw bad_optional_access(); return obj(); }
    constexpr const T& value() const & { if (!present) throw bad_optional_access(); return obj(); }
    constexpr T&& value() && { if (!present) throw bad_optional_access(); return obj(); }
    constexpr const T&& value() const && { if (!present) throw bad_optional_access(); return obj(); }
    template<typename U> constexpr T value_or(U&& def) const & { if (present) return obj(); return static_cast<T>(std::forward<U>(def)); }
    template<typename U> constexpr T value_or(U&& def) && { if (present) return obj(); return static_cast<T>(std::forward<U>(def)); }
    constexpr void reset() noexcept { if (present) obj().~T(); present = false; }
};

template<typename T> constexpr bool operator==(basic_string_view<T> l, basic_string_view<T> r) noexcept { if (l.size() != r.size()) return false; for (std::size_t i = 0; i < l.size(); i++) if (l[i] != r[i]) return false; return true; }
template<typename T> constexpr bool operator==(basic_string_view<T> l, const T* r) noexcept { return l == basic_string_view<T>(r); }
template<typename T> constexpr bool operator==(const T* l, basic_string_view<T> r) noexcept { return basic_string_view<T>(l) == r; }
template<typename T> constexpr bool operator!=(basic_string_view<T> l, basic_string_view<T> r) noexcept { return !(l == r); }
template<typename T> constexpr bool operator!=(basic_string_view<T> l, const T* r) noexcept { return !(l == r); }
template<typename T> constexpr bool operator!=(const T* l, basic_string_view<T> r) noexcept { return !(l == r); }


}
}
}

namespace std {

template<typename T>
struct hash<Scintilla::Internal::Compat::basic_string_view<T>>
{
    std::size_t operator()(Scintilla::Internal::Compat::basic_string_view<T> s)
    {
        // FNV-1a
        std::size_t h = 14695981039346656037ULL;
        for (std::size_t i = 0; i < s.size(); i++) {
            h ^= s[i];
            h *= 1099511628211ULL;
        }
        return h;
    }
};

}

#endif

#endif
