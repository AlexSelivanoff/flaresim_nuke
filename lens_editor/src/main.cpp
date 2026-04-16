// ============================================================================
// main.cpp — FlareSim Lens Editor entry point
// ============================================================================

#include "app.h"

#include <cstdio>

int main(int argc, char* argv[])
{
    App app;

    if (!app.init()) {
        fprintf(stderr, "Initialisation failed.\n");
        return 1;
    }

    // Optional: load a .lens file passed as first argument
    if (argc > 1)
        app.load_lens(argv[1]);

    app.run();
    app.shutdown();
    return 0;
}
