#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace aicontact {

// Minimal structural view of a WebAssembly binary — just enough to check an
// ACI module's imports and export signatures before handing it to a runtime.
struct WasmInfo {
    struct FuncType {
        std::vector<uint8_t> params, results;  // value type codes (0x7f i32, 0x7d f32, ...)
        std::string signature() const;          // e.g. "(i32,f32)->()"
    };
    struct Import { std::string module, name; uint8_t kind; uint32_t typeIndex; };
    struct Export { std::string name; uint8_t kind; uint32_t index; };
    struct Custom { std::string name; size_t offset, size; };  // payload range in the binary

    std::vector<FuncType> types;
    std::vector<Import> imports;
    std::vector<uint32_t> functionTypes;  // includes imported functions first
    std::vector<Export> exports;
    std::vector<Custom> customs;          // custom sections (id 0), in file order

    // Throws std::runtime_error with a readable message on malformed input.
    static WasmInfo parse(const std::vector<uint8_t>& bytes);

    const Export* findExport(const std::string& name) const;
    // Signature of an exported function, or empty string if not a function export.
    std::string exportSignature(const std::string& name) const;
};

// Checks the ACI v1 structural rules (spec §8 items 1-2). Returns a list of
// problems; empty means OK.
std::vector<std::string> checkAciStructure(const WasmInfo& info);

}  // namespace aicontact
