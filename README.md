How to use it (copy/paste commands)
Build
cc -O2 -Wall -Wextra -std=c11 api_tool.c -o api_tool

Generate api.def + index
./api_tool gen --root . --out framework/api.def --index framework/api_index.json

Search for stuff
./api_tool search --root . --kind fn_proto --name fw_add
./api_tool search --root . --kind struct --pattern Player

Auto-generate imports for an entry file (public view)
./api_tool needs --root . --entry game.c --auto_out framework/auto_import.h --vis public

Preprocessor mode (recommended when includes matter)
./api_tool needs --root . --auto_out framework/auto_import.h --vis public \
  --preprocess "cc -E -P -I. game.c"

Use the generated imports in your code

In game.c (or your TU), do:

#include "framework/auto_import.h"


Now only the needed declarations from framework/api.h will be emitted.

Public / Private usage
Public consumer:
#include "framework/public.h"

Internal consumer:
#include "framework/private.h"

With selective imports:
#include "framework/auto_import.h" // sets API_SELECTIVE + IMPORT_* + includes api.h

Notes (practical “deadline” caveats)

The extractor is heuristic for function prototypes/definitions (most repos are fine).

needs mode is robust because it’s token-based: it’ll catch usage of symbols even if prototypes are multi-line.

If you have names that collide with local variables/macros, you may see a few extra IMPORT_* lines. That’s usually harmless.
