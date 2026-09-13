#include "game/steam/steam_save_identity.h"

#include <windows.h>
#include <bcrypt.h>

#include <limits>
#include <vector>

namespace novafix::game::steam_cloud::save_identity {
namespace {

bool Succeeded(NTSTATUS status) {
    return status >= 0;
}

} // namespace

bool Compute(std::span<const unsigned char> bytes,
             ContentIdentity& identity) {
    identity = {};
    if (bytes.size() > std::numeric_limits<ULONG>::max()) return false;

    BCRYPT_ALG_HANDLE algorithm{};
    if (!Succeeded(BCryptOpenAlgorithmProvider(
            &algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0))) {
        return false;
    }

    DWORD objectBytes{};
    DWORD copied{};
    bool success = Succeeded(BCryptGetProperty(
        algorithm, BCRYPT_OBJECT_LENGTH,
        reinterpret_cast<PUCHAR>(&objectBytes), sizeof(objectBytes),
        &copied, 0));
    success = success && copied == sizeof(objectBytes) && objectBytes != 0;

    std::vector<unsigned char> object;
    BCRYPT_HASH_HANDLE hash{};
    if (success) {
        object.resize(objectBytes);
        success = Succeeded(BCryptCreateHash(
            algorithm, &hash, object.data(), objectBytes,
            nullptr, 0, 0));
    }
    if (success && !bytes.empty()) {
        success = Succeeded(BCryptHashData(
            hash, const_cast<PUCHAR>(bytes.data()),
            static_cast<ULONG>(bytes.size()), 0));
    }
    if (success) {
        success = Succeeded(BCryptFinishHash(
            hash, identity.sha256.data(),
            static_cast<ULONG>(identity.sha256.size()), 0));
    }

    if (hash) BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (!success) {
        identity = {};
        return false;
    }
    identity.size = bytes.size();
    return true;
}

} // namespace novafix::game::steam_cloud::save_identity
