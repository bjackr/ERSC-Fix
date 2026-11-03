#include "common/sha256.h"

#include <windows.h>
#include <bcrypt.h>

#include <format>
#include <fstream>
#include <vector>

namespace ersc_fix::common {

std::string sha256_file(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return {};

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return {};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        return {};
    }

    std::vector<char> chunk(1 << 20);
    while (file) {
        file.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        if (auto got = file.gcount(); got > 0) {
            BCryptHashData(hash, reinterpret_cast<PUCHAR>(chunk.data()), static_cast<ULONG>(got), 0);
        }
    }

    UCHAR digest[32];
    BCryptFinishHash(hash, digest, sizeof(digest), 0);
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);

    std::string hex;
    for (UCHAR byte : digest) hex += std::format("{:02x}", byte);
    return hex;
}

}
