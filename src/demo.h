#pragma once

#include "common.h"

#include <string>
#include <utility>
#include <vector>

namespace pc {
namespace demo {

struct DemoModel {
    std::string engine;
    std::string model;
    int weight = 1;
};

struct DemoSpec {
    std::string now = "2026-11-21 15:00:00";
    int days = 30;
    int pointsPerHour = 4;
    uint64_t tokensPerDay = 180000;
    double costPerDay = 5.0;
    std::string queue = "demo";
    std::vector<DemoModel> models;
};

bool LoadSpec(DemoSpec& out, std::string& err);
bool GenerateData(const DemoSpec& spec, int64_t nowSec, std::string& err);
bool Prepare(std::string& err);
std::string DummyJobCommand(const std::string& scenario, int seed);
std::string BuildDummyLlmOutput(const std::string& scenario, int seed);

} // namespace demo
} // namespace pc
