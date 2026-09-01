# borealis patches

Each file here is a whole-file replacement for a borealis source, copied over
the submodule by `configure_file(... COPYONLY)` during CMake configure (see the
`*_PATCH` / `*_TARGET` pairs in the top-level `CMakeLists.txt`). Whole-file
copies, not diffs — so when upstream changes a patched file, the change is
silently discarded until someone rebases.

That has bitten us once already: the GLFW input patch predated upstream's
joystick-count fixes, so desktop builds quietly ran without the `controllersCount++`
on connect and without the underflow guard on disconnect.

## Which upstream revision each patch was rebased onto

Keep this table current when you touch a patch — without it, finding the base
means diffing the patch against dozens of historical revisions of the file to
see which one it was derived from.

| Patch | Target in submodule | Rebased onto |
| --- | --- | --- |
| `brls_application.cpp` | `library/lib/core/application.cpp` | `b298947c6d2f` |
| `brls_application.hpp` | `library/include/borealis/core/application.hpp` | `e5136ab978a7` |
| `fontstash.h` | `library/include/borealis/extern/nanovg/fontstash.h` | `fac8e7ff747b` |
| `nanovg.c` | `library/lib/extern/nanovg/nanovg.c` | `67d78a113eb8` |
| `glfw_input.cpp` | `library/lib/platforms/glfw/glfw_input.cpp` | `0303fe7ff01c` |
| `glfw_platform.cpp` | `library/lib/platforms/glfw/glfw_platform.cpp` | `bdbc2506823d` |
| `psv_platform.cpp` | `library/lib/platforms/psv/psv_platform.cpp` | `bdbc2506823d` |

A base older than the submodule pointer is not automatically stale: it only
matters if upstream has since touched that file. Six of the seven above sit on
older revisions simply because upstream has not changed those files.

## Rebasing after a submodule bump

The submodule is cloned shallow, so fetch enough history to reach the base
first (`git -C lib/borealis fetch --depth 200 origin switchfin`), then per file:

    git -C lib/borealis show <base>:<target>      > /tmp/base
    git -C lib/borealis show <new-rev>:<target>   > /tmp/theirs
    git merge-file -p patches/<file> /tmp/base /tmp/theirs > /tmp/merged

Resolve any conflict, install `/tmp/merged` as the patch, and update the row
above. Then check the residual `diff` against the new upstream file contains
only intentional changes — anything upstream added that is missing from it is
a regression the copy would reintroduce.
