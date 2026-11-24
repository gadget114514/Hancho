#ifdef HANCHO_DEMO_BUILD

#include "demo.h"
#include "common.h"
#include "nlohmann/json.hpp"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    // A translate-llm batch (TranslationStore) names its input file in PC_TRANSLATE_INPUT_FILE.
    // Reply with the same array, each item marked as a dummy translation, so the batch path can be
    // exercised without a real engine.
    if (const char* inputPath = std::getenv("PC_TRANSLATE_INPUT_FILE")) {
        std::ifstream in(inputPath, std::ios::binary);
        std::stringstream text;
        text << in.rdbuf();
        nlohmann::json items = nlohmann::json::parse(text.str(), nullptr, false);
        nlohmann::json reply = nlohmann::json::array();
        if (items.is_array()) {
            for (const auto& item : items) {
                reply.push_back("[dummy] " + (item.is_string() ? item.get<std::string>() : std::string()));
            }
        }
        const std::string output = reply.dump();
        std::fwrite(output.data(), 1, output.size(), stdout);
        std::fflush(stdout);
        return 0;
    }

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
