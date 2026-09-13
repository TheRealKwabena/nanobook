// nanobook — minimal test framework.
//
// Deliberately not gtest or Catch2. The whole library is header-only with no
// dependencies, and `make tests` on a fresh clone should work with nothing but a
// compiler. A test registry, four assertion macros and a runner is all this
// project needs, and it keeps the dependency count at zero.
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace nbtest {

using TestFn = void (*)();

struct TestCase {
    const char* suite;
    const char* name;
    TestFn fn;
};

// Function-local static: avoids the static-initialisation-order problem that a
// namespace-scope vector would have, since registration happens during dynamic
// initialisation across several translation units.
inline std::vector<TestCase>& registry() {
    static std::vector<TestCase> r;
    return r;
}

struct Registrar {
    Registrar(const char* suite, const char* name, TestFn fn) {
        registry().push_back(TestCase{suite, name, fn});
    }
};

struct Counters {
    std::uint64_t checks = 0;
    std::uint64_t failures = 0;
    std::string current;
};

inline Counters& counters() {
    static Counters c;
    return c;
}

inline void report_failure(const char* file, int line, const std::string& msg) {
    Counters& c = counters();
    ++c.failures;
    if (c.failures <= 40) {
        std::fprintf(stderr, "  FAIL %s:%d\n        %s\n", file, line, msg.c_str());
    }
}

template <class T>
std::string to_str(const T& v) {
    if constexpr (std::is_same_v<T, bool>) return v ? "true" : "false";
    else if constexpr (std::is_same_v<T, char>) return std::string(1, v);
    // string_view is only *explicitly* convertible to std::string, so it needs its
    // own branch or it falls through to std::to_string and fails to compile.
    else if constexpr (std::is_convertible_v<T, std::string_view>)
        return std::string(std::string_view(v));
    else if constexpr (std::is_convertible_v<T, std::string>) return std::string(v);
    else return std::to_string(v);
}

}  // namespace nbtest

#define NB_TEST(suite, name)                                                    \
    static void nbtest_##suite##_##name();                                      \
    static ::nbtest::Registrar nbreg_##suite##_##name(#suite, #name,            \
                                                      nbtest_##suite##_##name); \
    static void nbtest_##suite##_##name()

#define CHECK(cond)                                                          \
    do {                                                                     \
        ++::nbtest::counters().checks;                                       \
        if (!(cond)) ::nbtest::report_failure(__FILE__, __LINE__, #cond);    \
    } while (0)

#define CHECK_MSG(cond, msg)                                                            \
    do {                                                                                \
        ++::nbtest::counters().checks;                                                   \
        if (!(cond))                                                                     \
            ::nbtest::report_failure(__FILE__, __LINE__, std::string(#cond) + " — " + (msg)); \
    } while (0)

#define CHECK_EQ(a, b)                                                                    \
    do {                                                                                  \
        ++::nbtest::counters().checks;                                                     \
        const auto nb_a_ = (a);                                                            \
        const auto nb_b_ = (b);                                                            \
        if (!(nb_a_ == nb_b_))                                                             \
            ::nbtest::report_failure(__FILE__, __LINE__,                                   \
                std::string(#a) + " == " + #b + "  (got " + ::nbtest::to_str(nb_a_) +      \
                ", want " + ::nbtest::to_str(nb_b_) + ")");                                \
    } while (0)
