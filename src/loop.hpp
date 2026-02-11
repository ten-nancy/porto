#pragma once

#include "util/error.hpp"
#include "util/path.hpp"

class TFile;
class TPath;

TError SetupLoopDev(const TFile &file, const TPath &path, int &devnr);
TError PutLoopDev(const int loopNr);
