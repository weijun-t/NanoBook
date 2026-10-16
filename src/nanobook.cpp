// Compiles every public header with the engine's flags (-fno-exceptions, -Werror, ...), so a
// header that throws, warns or is missing an #include fails the build here, not later in a test.
#include "nanobook/event_buffer.hpp"
#include "nanobook/messages.hpp"
#include "nanobook/reference_book.hpp"
#include "nanobook/types.hpp"
