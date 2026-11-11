#pragma once

// The book implementation the engine uses, chosen at compile time.
//
// Configure with -DITCHBT_BASELINE_BOOK=ON to build the engine on the baseline containers, so
// end-to-end comparisons between the two stay reproducible.

#include "itchbt/book/book.hpp"

namespace itchbt::book {

#ifdef ITCHBT_BASELINE_BOOK
using DefaultBooks = BaselineBooks;
inline constexpr const char* kDefaultBookName = "baseline";
#else
using DefaultBooks = OptimisedBooks;
inline constexpr const char* kDefaultBookName = "optimised";
#endif

} // namespace itchbt::book
