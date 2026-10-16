// Compiles every public header with the engine's flags (-fno-exceptions, -Werror, ...), so a
// header that throws, warns or is missing an #include fails the build here, not later in a test.
#include "nanobook/core/event_buffer.hpp"
#include "nanobook/exchange.hpp"
#include "nanobook/core/messages.hpp"
#include "nanobook/core/order_book.hpp"
#include "nanobook/reference/reference_book.hpp"
#include "nanobook/core/types.hpp"

// Exchange is a template, so its code is only compiled where it's used. This "explicit
// instantiation" compiles every member for the reference book here, under the engine's flags.
template class nanobook::Exchange<nanobook::ReferenceBook>;
