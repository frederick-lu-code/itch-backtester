#pragma once

// The book implementation the engine uses, chosen at compile time.
//
// Configure with -DITCHBT_BASELINE_BOOK=ON to build the engine on the baseline containers, so
// end-to-end comparisons between the two stay reproducible.

#include "itchbt/book/book.hpp"

namespace itchbt::book {

using DefaultBooks = BaselineBooks;

} // namespace itchbt::book
