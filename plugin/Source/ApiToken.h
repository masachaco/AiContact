#pragma once

#include <string>

// The control API's token, kept in the application settings (shared by every AI Contact
// instance of this user: plugins in any DAW and the Standalone app). Clients get it from the
// editor's "API Token" button; nothing reads it from a well-known file.
namespace ApiToken {

std::string get();     // creates one on first use; follows changes made by other processes
std::string rotate();  // replaces it; clients holding the old token get 401 from then on

}  // namespace ApiToken
