#pragma once

// The list of book implementations every typed book test runs against.

#include "itchbt/book/book.hpp"

#include <gtest/gtest.h>

namespace itchbt::test {

using BookTypes = ::testing::Types<book::BaselineBooks>;

} // namespace itchbt::test
