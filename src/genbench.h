// Generator for the benchmark suite shipped in benchmark/.
//
// Every instance is satisfiable by construction: a planted assignment is drawn
// first and the formula is built around it. The circuit and SHA-256 families are
// Tseitin encodings whose input bits occupy the lowest variable indices, which
// is what the solver's input auto-detection expects.
#pragma once

#include <string>

namespace tcs {

bool generateBenchmarkSuite(const std::string& directory);

}  // namespace tcs
