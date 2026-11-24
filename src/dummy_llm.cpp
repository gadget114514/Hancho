#ifdef HANCHO_DEMO_BUILD

#include "demo.h"
#include "common.h"

#include <cstdio>
#include <string>

int main(int argc, char** argv) {
    std::string scenario = "mixed";
    int seed = 0;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--scenario" && i + 1 < argc) {
            scenario = argv[++i];
        } else if (arg == "--seed" && i + 1 < argc) {
            try {
                seed = std::stoi(argv[++i]);
            } catch (...) {
                seed = 0;
            }
        }
    }
    std::string output = pc::demo::BuildDummyLlmOutput(scenario, seed);
    std::fwrite(output.data(), 1, output.size(), stdout);
    std::fflush(stdout);
    return 0;
}

#else

int main() { return 0; }

#endif
