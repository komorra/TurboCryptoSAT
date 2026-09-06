// Parameter tuning against known solutions.
//
// `--siglen`, `--initk` and `--mink` have no defaults that are right for every
// encoding - the band in which the statistical layer says anything useful is
// narrow, and it moves from one instance family to the next. Tuning finds that
// band by measurement instead of by guesswork.
//
// What makes it affordable is the reference solution. The solver assigns in
// place and never backtracks, so a setting that commits a wrong literal does not
// announce itself: the run simply wanders off and burns the whole budget before
// reporting a timeout. Given the solution, the very first literal committed
// against it ends the trial instead - usually in a fraction of a second - so a
// bad setting costs almost nothing to rule out and the search can afford to look
// at many of them.
//
// The solution is only ever a check. Nothing in the solver reads it to decide
// anything, so the parameters that come out mean the same thing on an instance
// whose solution nobody has - which is the entire point of tuning on a solved
// member of a family and using the result on the rest.
#pragma once

#include <string>

#include "options.h"

namespace tcs {

// Runs the search described by `opt.tune*` and prints the winning settings.
// Returns a process exit code.
int runTune(const Options& opt);

}  // namespace tcs
