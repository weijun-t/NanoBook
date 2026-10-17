/*
 * fast_book.cpp: compiles the fast book under the engine's flags.
 *
 * What it does: Explicit instantiations of BasicFastBook for both level structures (SortedLevels
 *   and TickLevels): every member function of the template is compiled here, inside the engine
 *   library, with -fno-exceptions and -Werror.
 *
 * Why it exists: A template is normally only compiled where something uses it, which for the fast
 *   book would be the tests, built with exceptions on. Compiling it here proves the engine code
 *   itself builds under the engine's rules.
 */

#include "nanobook/fast/fast_book.hpp"

template class nanobook::fast::BasicFastBook<nanobook::fast::SortedLevels>;
template class nanobook::fast::BasicFastBook<nanobook::fast::TickLevels>;
