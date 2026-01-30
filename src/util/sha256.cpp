#include <openssl/sha.h>

#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "util/error.hpp"

TError calculateSHA256(const std::string &filename, std::string &sha256Digest) {
    std::ifstream file(filename, std::ios::binary);
    if (!file) {
        return TError("Cannot open file: {} ", filename);
    }

    SHA256_CTX sha256;
    SHA256_Init(&sha256);

    char buffer[8192];  // 8KB buffer for efficiency
    while (true) {
        file.read(buffer, sizeof(buffer));
        if (file.bad())
            return TError("Failed to read file: {}, {}", filename, errno);
        // non-ANSI C standart behaviour of return value,
        // 0 - means error
        // 1 - means success
        if (!SHA256_Update(&sha256, buffer, file.gcount()))
            return TError("Failed to update sha256");

        if (!file.good() || file.fail() || file.eof())
            break;
    }

    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256_Final(hash, &sha256);

    // Convert to hex string
    std::stringstream ss;
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++) {
        ss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    sha256Digest = ss.str();
    return OK;
}
