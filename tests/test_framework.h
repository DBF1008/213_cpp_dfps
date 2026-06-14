/*
 * Lightweight test framework for dfps pure-logic tests.
 *
 * Include this header in every test file.  The main() runner lives in test_main.cpp.
 */

#pragma once

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <vector>

// ---- Test registry ----

struct TestEntry {
    const char *name;
    std::function<void()> fn;
};

// Defined in test_main.cpp
std::vector<TestEntry> &GetTests(void);

struct TestRegistrar {
    TestRegistrar(const char *name, std::function<void()> fn) { GetTests().push_back({name, fn}); }
};

// ---- Exception-based assertion failure ----

struct TestFailure {
    const char *file;
    int line;
    const char *expr;
    const char *msg;
};

// ---- Assertion macros ----

#define TEST(name)                                                                                                     \
    static void test_##name(void);                                                                                     \
    static TestRegistrar reg_##name(#name, test_##name);                                                               \
    static void test_##name(void)

#define ASSERT_TRUE(expr)                                                                                              \
    do {                                                                                                               \
        if (!(expr)) {                                                                                                 \
            throw TestFailure{__FILE__, __LINE__, #expr, "expected true"};                                             \
        }                                                                                                              \
    } while (0)

#define ASSERT_FALSE(expr)                                                                                             \
    do {                                                                                                               \
        if ((expr)) {                                                                                                  \
            throw TestFailure{__FILE__, __LINE__, #expr, "expected false"};                                            \
        }                                                                                                              \
    } while (0)

#define ASSERT_EQ(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a != _b) {                                                                                                \
            fprintf(stderr, "\n  ASSERT_EQ failed: %s != %s\n", #a, #b);                                              \
            throw TestFailure{__FILE__, __LINE__, #a " == " #b, "not equal"};                                          \
        }                                                                                                              \
    } while (0)

#define ASSERT_NE(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (_a == _b) {                                                                                                \
            fprintf(stderr, "\n  ASSERT_NE failed: %s == %s\n", #a, #b);                                              \
            throw TestFailure{__FILE__, __LINE__, #a " != " #b, "unexpectedly equal"};                                 \
        }                                                                                                              \
    } while (0)

#define ASSERT_GT(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (!(_a > _b)) {                                                                                              \
            fprintf(stderr, "\n  ASSERT_GT failed: %s <= %s\n", #a, #b);                                              \
            throw TestFailure{__FILE__, __LINE__, #a " > " #b, "not greater"};                                         \
        }                                                                                                              \
    } while (0)

#define ASSERT_GE(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (!(_a >= _b)) {                                                                                             \
            fprintf(stderr, "\n  ASSERT_GE failed: %s < %s\n", #a, #b);                                               \
            throw TestFailure{__FILE__, __LINE__, #a " >= " #b, "not greater-or-equal"};                               \
        }                                                                                                              \
    } while (0)

#define ASSERT_LT(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (!(_a < _b)) {                                                                                              \
            fprintf(stderr, "\n  ASSERT_LT failed: %s >= %s\n", #a, #b);                                              \
            throw TestFailure{__FILE__, __LINE__, #a " < " #b, "not less"};                                            \
        }                                                                                                              \
    } while (0)

#define ASSERT_LE(a, b)                                                                                                \
    do {                                                                                                               \
        auto _a = (a);                                                                                                 \
        auto _b = (b);                                                                                                 \
        if (!(_a <= _b)) {                                                                                             \
            fprintf(stderr, "\n  ASSERT_LE failed: %s > %s\n", #a, #b);                                               \
            throw TestFailure{__FILE__, __LINE__, #a " <= " #b, "not less-or-equal"};                                  \
        }                                                                                                              \
    } while (0)
