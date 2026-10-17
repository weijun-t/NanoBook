/*
 * nanobook.cpp: a build check for the engine's headers.
 *
 * What it does: Includes every public header and compiles Exchange<ReferenceBook>, all with the
 *   engine's strict flags (-fno-exceptions, -Werror, ...).
 *
 * Why it exists: Headers and templates are only compiled where something uses them. This file makes
 *   sure a header that throws, warns or is missing an #include fails here, in the library, instead
 *   of later in some test.
 */

#include "nanobook/core/event_buffer.hpp"
#include "nanobook/exchange.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/names.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/reference/reference_book.hpp"
#include "nanobook/core/types.hpp"

// Exchange is a template, so its code is only compiled where it's used. This "explicit
// instantiation" compiles every member for the reference book here, under the engine's flags.
template class nanobook::Exchange<nanobook::ReferenceBook>;
