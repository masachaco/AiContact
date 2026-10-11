#include "aicontact/Auth.h"

#include <cctype>
#include <cstdio>
#include <fstream>
#include <stdexcept>

#ifdef _WIN32
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <bcrypt.h>
#  pragma comment(lib, "bcrypt.lib")
#endif

namespace aicontact {

std::string generateToken() {
    unsigned char bytes[32];
#ifdef _WIN32
    if (BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0)
        throw std::runtime_error("BCryptGenRandom failed");
#else
    std::ifstream urandom("/dev/urandom", std::ios::binary);
    if (!urandom.read(reinterpret_cast<char*>(bytes), sizeof(bytes)))
        throw std::runtime_error("cannot read /dev/urandom");
#endif
    char hex[65];
    for (int i = 0; i < 32; ++i) std::snprintf(hex + 2 * i, 3, "%02x", bytes[i]);
    return std::string(hex, 64);
}

bool isValidToken(const std::string& t) {
    if (t.size() != 64) return false;
    for (char c : t)
        if (!std::isxdigit((unsigned char)c)) return false;
    return true;
}

std::string maskToken(const std::string& token) {
    if (token.size() < 8) return "????";
    return token.substr(0, 4) + "\xE2\x80\xA6" + token.substr(token.size() - 4);
}

bool constantTimeEquals(const std::string& a, const std::string& b) {
    unsigned char diff = (unsigned char)(a.size() != b.size());
    const size_t n = a.size() < b.size() ? a.size() : b.size();
    for (size_t i = 0; i < n; ++i) diff |= (unsigned char)(a[i] ^ b[i]);
    return diff == 0;
}

}  // namespace aicontact
