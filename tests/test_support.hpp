#pragma once

#include <cassert>

#include <gtest/gtest.h>

#undef assert
#define assert(expr) ASSERT_TRUE((expr)) << "assert(" #expr ")"

#define ASSERT_HAS_VALUE_OR_SKIP_NOT_AVAILABLE(result)                                      \
    do {                                                                                    \
        if (!(result).has_value() && (result).error().code == "NotAvailable") {          \
            GTEST_SKIP() << (result).error().message;                                       \
        }                                                                                   \
        ASSERT_TRUE((result).has_value())                                                   \
            << (result).error().code << ": " << (result).error().message;                 \
    } while (0)
