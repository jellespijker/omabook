#pragma once

#include <span>

#include "failure.h"

// The sentences Failure words its codes in (failure.cpp), and the tables kept
// apart from it.
namespace FailureWords {

// A code's sentence. %1, %2... are its arguments, in order.
struct Wording {
    Failure::Code code;
    const char *text;
};

// Of the snapshots and their destinations (#181 to #185, #292).
[[nodiscard]] std::span<const Wording> snapshots();

} // namespace FailureWords
