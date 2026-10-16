// Compiles every public header with the engine's flags (-fno-exceptions, -Werror, ...), so a
// header that throws, warns or is missing an #include fails the build here, not later in a test.
// The reference book's own source files join the library in the next step.
#include "nanobook/messages.hpp"
#include "nanobook/types.hpp"
