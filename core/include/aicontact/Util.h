#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aicontact {

constexpr const char* kVersion = "0.1.0";
constexpr int kAciAbiVersion = 1;

std::string base64Encode(const std::vector<uint8_t>& data);
// Throws std::runtime_error on malformed input. Whitespace is ignored.
std::vector<uint8_t> base64Decode(const std::string& text);

// SHA-256, hex encoded. Identifies module binaries; DAW projects store it so a file that was
// changed behind the project's back is refused instead of run.
std::string sha256Hex(const uint8_t* data, size_t size);
std::string sha256Hex(const std::vector<uint8_t>& data);

std::vector<uint8_t> readFile(const std::string& path);   // throws

// "Super Saw!" -> "super-saw"
std::string slugify(const std::string& text);

double nowSeconds();  // steady clock

}  // namespace aicontact
