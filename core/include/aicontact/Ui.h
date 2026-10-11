#pragma once

#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace aicontact {

struct WasmInfo;
struct InstrumentMeta;

// Custom GUI assets shipped inside an ACI binary as wasm custom sections named
// "aci.ui/<path>" (spec §10). Runtimes ignore custom sections, so modules with
// a UI stay loadable everywhere; hosts without UI support show the default panel.
//   aci.ui/index.html  -> "html": HTML/CSS/JS page shown in a web view
//   aci.ui/ui.json     -> "skin": declarative layout drawn natively (images, knobs...)
// A module may carry both; hosts prefer "html" and fall back to "skin".
struct UiFile {
    std::string path;
    size_t offset = 0, size = 0;  // payload range inside the module binary
};

struct UiBundle {
    static constexpr const char* kSectionPrefix = "aci.ui/";
    static constexpr size_t kMaxBytes = 8 * 1024 * 1024;

    std::vector<UiFile> files;
    size_t totalBytes = 0;
    std::vector<std::string> problems;  // malformed paths, duplicates (files are skipped)

    static UiBundle fromWasm(const WasmInfo& info);

    const UiFile* find(const std::string& path) const;
    bool hasHtml() const { return find("index.html") != nullptr; }
    bool hasSkin() const { return find("ui.json") != nullptr; }
    bool empty() const { return files.empty(); }
    std::string kind() const;  // "html", "skin" or "none"
};

// "knob.png" -> "image/png"; unknown extensions -> "application/octet-stream".
std::string uiMimeType(const std::string& path);

// Index of the param a UI element refers to: an integer id or a name
// (case-insensitive). -1 if not found.
int uiParamIndex(const InstrumentMeta& meta, const nlohmann::json& ref);

// Pixel size of a PNG / JPEG / GIF image; false for other formats (e.g. SVG).
bool uiImageSize(const uint8_t* data, size_t size, int& width, int& height);

// Size the UI is designed for, in pixels: ui.json "size" for skins,
// <meta name="aci-size" content="720x360"> for HTML pages (default 720x360).
// False if the module has no UI.
bool uiPreferredSize(const std::vector<uint8_t>& wasm, const UiBundle& ui, int& width, int& height);

// Checks the UI of a module against its metadata: {"kind", "files", "bytes",
// "errors", "warnings"}. Errors mean the host will fall back to a default UI.
nlohmann::json checkUi(const std::vector<uint8_t>& wasm, const UiBundle& ui, const InstrumentMeta& meta);

}  // namespace aicontact
