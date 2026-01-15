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

Backend Filtering Support

The C_SELECT tool now supports backend filtering to selectively include symbols based on their origin:

# Generate API with backend filtering
./api_tool gen --root . --out generated/api.def --index generated/api_index.json --backend raylib

# Available backends:
# - sdl: SDL-specific symbols
# - raylib: Raylib-specific symbols  
# - core: Core/utility symbols
# - (no filter): All symbols

# Search with backend filtering
./api_tool search --root . --kind struct --backend raylib
./api_tool search --root . --kind fn_def --backend sdl

# Generate selective imports with backend filter
./api_tool needs --root . --entry game.c --out generated/auto_import.h --backend raylib --vis public

X-Macro Pattern Integration

The tool supports X-macro patterns for clean API definitions:

# API definition file (api.def)
API_TYPE(PUBLIC, Vector2,
  float x;
  float y;
)

API_FN(void, draw_circle_filled, (float x, float y, float radius, Color color))

# Generated header (api.h)
#define API_TYPE(vis, name, body) typedef struct name { body } name;
#define API_FN(ret, name, sig) ret name sig;
#include "api.def"
#undef API_TYPE
#undef API_FN

Successful Game Integration

The C_SELECT system has been successfully integrated into a complex game project:

✅ **Refactored game API using C_SELECT** with backend filtering
✅ **Generated all API files into clean `generated/` directory**
✅ **Created OpenGL drawing functions** using X-macro pattern
✅ **Maintained Raylib compatibility** with custom implementations
✅ **Preserved game logic and AI behavior** while providing clean API layer

Example usage in game:
```c
#include "generated/auto_import.h"  // Auto-imports only needed symbols

// Uses custom OpenGL implementations
draw_circle_filled_opengl(x, y, radius, RED);
Vector2 mouse_pos = GetMousePosition();
```

Notes (practical "deadline" caveats)

The extractor is heuristic for function prototypes/definitions (most repos are fine).

needs mode is robust because it's token-based: it'll catch usage of symbols even if prototypes are multi-line.

If you have names that collide with local variables/macros, you may see a few extra IMPORT_* lines. That's usually harmless.

Backend filtering uses path heuristics and @backend annotations for accurate symbol categorization.
