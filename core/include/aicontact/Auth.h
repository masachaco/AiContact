#pragma once

#include <string>

namespace aicontact {

// The localhost API is protected by a secret token that clients send as
// "Authorization: Bearer <token>". The application owns it (the plugin keeps it
// in its settings, the headless host takes it from the command line or makes
// one up) and hands it to ControlServer; nothing else reads it from disk.

// 256 random bits from the OS CSPRNG, hex encoded. Throws on failure.
std::string generateToken();

// 64 hex characters.
bool isValidToken(const std::string& token);

// "3f9a…c21e": enough to recognise a token without revealing it.
std::string maskToken(const std::string& token);

// Compares without an early exit, so timing does not reveal a prefix match.
bool constantTimeEquals(const std::string& a, const std::string& b);

}  // namespace aicontact
