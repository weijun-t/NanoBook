/*
 * fast_book.cpp: compiles the fast book under the engine's flags.
 *
 * What it does: An explicit instantiation of BasicFastBook<MapLevels>: every member function of
 *   the template is compiled here, inside the engine library, with -fno-exceptions and -Werror.
 *
 * Why it exists: A template is normally only compiled where something uses it, which for the fast
 *   book would be the tests, built with exceptions on. Compiling it here proves the engine code
 *   itself builds under the engine's rules. Phase 2 step 6 adds a line per price-level structure.
 */

#include "nanobook/fast/fast_book.hpp"

template class nanobook::fast::BasicFastBook<nanobook::fast::MapLevels>;
