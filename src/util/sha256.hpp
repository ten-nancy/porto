#pragma once

#include <util/error.hpp>

TError calculateSHA256(const std::string &filename, std::string &sha256Digest);
