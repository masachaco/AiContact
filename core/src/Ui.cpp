#include "aicontact/Ui.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <set>

#include "aicontact/Instrument.h"
#include "aicontact/WasmInfo.h"

namespace aicontact {

using json = nlohmann::json;

namespace {

bool validPath(const std::string& p) {
    if (p.empty() || p.size() > 200 || p.front() == '/' || p.back() == '/') return false;
    for (char c : p)
        if (!(std::isalnum((unsigned char)c) || c == '.' || c == '_' || c == '-' || c == '/')) return false;
    size_t start = 0;
    while (start <= p.size()) {
        size_t end = p.find('/', start);
        if (end == std::string::npos) end = p.size();
        const std::string part = p.substr(start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

std::string extension(const std::string& path) {
    const size_t dot = path.rfind('.');
    if (dot == std::string::npos || path.find('/', dot) != std::string::npos) return {};
    std::string e = path.substr(dot + 1);
    for (auto& c : e) c = (char)std::tolower((unsigned char)c);
    return e;
}

bool startsWith(const uint8_t* d, size_t n, const char* sig, size_t len) {
    return n >= len && std::memcmp(d, sig, len) == 0;
}

// What an image reference may point to (formats both the native skin renderer and web views decode).
bool isImage(const uint8_t* d, size_t n, bool& svg) {
    svg = false;
    if (startsWith(d, n, "\x89PNG\r\n\x1a\n", 8) || startsWith(d, n, "\xff\xd8\xff", 3) ||
        startsWith(d, n, "GIF8", 4))
        return true;
    const std::string head((const char*)d, std::min<size_t>(n, 512));
    svg = head.find("<svg") != std::string::npos || head.find("<?xml") != std::string::npos;
    return svg;
}

bool isFont(const uint8_t* d, size_t n) {
    return startsWith(d, n, "\x00\x01\x00\x00", 4) || startsWith(d, n, "OTTO", 4) || startsWith(d, n, "true", 4);
}

bool validColour(const std::string& s) {
    if (s.empty() || s[0] != '#') return false;
    if (s.size() != 4 && s.size() != 7 && s.size() != 9) return false;
    return std::all_of(s.begin() + 1, s.end(), [](char c) { return std::isxdigit((unsigned char)c) != 0; });
}

struct SkinChecker {
    const std::vector<uint8_t>& wasm;
    const UiBundle& ui;
    const InstrumentMeta& meta;
    json& errors;
    json& warnings;
    std::set<std::string> referenced;

    void error(const std::string& where, const std::string& what) { errors.push_back("ui.json " + where + ": " + what); }
    void warn(const std::string& where, const std::string& what) { warnings.push_back("ui.json " + where + ": " + what); }

    const UiFile* file(const std::string& where, const json& v, const char* key) {
        if (!v.is_string() || v.get<std::string>().empty()) {
            error(where, std::string("'") + key + "' must be a file name");
            return nullptr;
        }
        const std::string path = v.get<std::string>();
        referenced.insert(path);
        const UiFile* f = ui.find(path);
        if (!f) error(where, "file '" + path + "' is not in the UI bundle");
        return f;
    }

    // Returns the image size (0x0 for SVG) or false if the reference is unusable.
    bool image(const std::string& where, const json& v, const char* key, int& w, int& h) {
        w = h = 0;
        const UiFile* f = file(where, v, key);
        if (!f) return false;
        bool svg = false;
        const uint8_t* d = wasm.data() + f->offset;
        if (!isImage(d, f->size, svg)) {
            error(where, "'" + f->path + "' is not a PNG, JPEG, GIF or SVG image");
            return false;
        }
        if (!svg) uiImageSize(d, f->size, w, h);
        return true;
    }

    void colour(const std::string& where, const json& el, const char* key) {
        if (el.contains(key) && !(el[key].is_string() && validColour(el[key].get<std::string>())))
            error(where, std::string("'") + key + "' must be a colour like \"#ffaa00\" or \"#ffaa0080\"");
    }

    int param(const std::string& where, const json& el) {
        if (!el.contains("param")) {
            error(where, "'param' (name or id) is required");
            return -1;
        }
        const int idx = uiParamIndex(meta, el["param"]);
        if (idx < 0) {
            std::string names;
            for (const auto& p : meta.params) names += (names.empty() ? "" : ", ") + p.name;
            error(where, "no param " + el["param"].dump() + " (params: " + (names.empty() ? "none" : names) + ")");
        }
        return idx;
    }

    void run(const json& skin) {
        if (!skin.is_object()) {
            error("", "must be a JSON object");
            return;
        }
        int width = 0, height = 0;
        const json size = skin.value("size", json());
        if (size.is_array() && size.size() == 2 && size[0].is_number_integer() && size[1].is_number_integer()) {
            width = size[0].get<int>();
            height = size[1].get<int>();
        }
        if (width < 64 || height < 32 || width > 4096 || height > 4096) {
            error("'size'", "must be [width, height] in pixels (64..4096 x 32..4096)");
            return;
        }
        if (skin.contains("background")) {
            const json& bg = skin["background"];
            if (bg.is_string() && !bg.get<std::string>().empty() && bg.get<std::string>()[0] == '#') {
                colour("'background'", skin, "background");
            } else {
                int w, h;
                if (image("'background'", bg, "background", w, h) && w && (w * height != h * width))
                    warn("'background'", "image is " + std::to_string(w) + "x" + std::to_string(h) +
                                             ", not the aspect ratio of 'size'; it will be stretched");
            }
        }
        if (skin.contains("font")) {
            if (const UiFile* f = file("'font'", skin["font"], "font"))
                if (!isFont(wasm.data() + f->offset, f->size)) error("'font'", "'" + f->path + "' is not a TTF/OTF font");
        }
        colour("", skin, "color");
        colour("", skin, "accent");

        if (!skin.contains("elements") || !skin["elements"].is_array()) {
            error("", "'elements' must be an array");
            return;
        }
        std::set<int> placed;
        int i = 0;
        for (const auto& el : skin["elements"]) {
            const std::string where = "elements[" + std::to_string(i++) + "]";
            if (!el.is_object() || !el.contains("type") || !el["type"].is_string()) {
                error(where, "must be an object with a 'type'");
                continue;
            }
            const std::string type = el["type"].get<std::string>();
            const std::string at = where + " (" + type + ")";
            bool rectOk = true;
            for (const char* k : {"x", "y", "w", "h"})
                if (!el.contains(k) || !el[k].is_number()) rectOk = false;
            if (!rectOk) {
                error(at, "needs numeric 'x', 'y', 'w', 'h'");
                continue;
            }
            const double x = el["x"], y = el["y"], w = el["w"], h = el["h"];
            if (w <= 0 || h <= 0) error(at, "'w' and 'h' must be positive");
            if (x < 0 || y < 0 || x + w > width || y + h > height)
                warn(at, "extends outside 'size' and will be clipped");
            colour(at, el, "color");

            if (type == "image") {
                int iw, ih;
                image(at, el.value("src", json()), "src", iw, ih);
            } else if (type == "label") {
                if (!el.contains("text") || !el["text"].is_string()) error(at, "'text' is required");
            } else if (type == "value") {
                param(at, el);
            } else if (type == "knob" || type == "slider" || type == "toggle") {
                const int idx = param(at, el);
                if (idx >= 0) placed.insert(idx);
                if (el.contains("src")) {
                    int iw = 0, ih = 0;
                    if (image(at, el["src"], "src", iw, ih)) {
                        const int frames = el.value("frames", type == "toggle" ? 2 : 1);
                        if (frames < 1 || frames > 1024) error(at, "'frames' must be 1..1024");
                        else if (iw && ih && frames > 1) {
                            const int along = ih >= iw ? ih : iw;  // filmstrips run along their long side
                            if (along % frames)
                                warn(at, "image is " + std::to_string(iw) + "x" + std::to_string(ih) +
                                             ", which does not divide into " + std::to_string(frames) + " frames");
                        }
                        if (type == "toggle" && frames != 2)
                            error(at, "a toggle image is a 2-frame filmstrip (off, on); set \"frames\": 2");
                    }
                }
                if (type == "slider" && el.contains("track")) {
                    int iw, ih;
                    image(at, el["track"], "track", iw, ih);
                }
                if (el.contains("angle")) {
                    const json& a = el["angle"];
                    if (!(a.is_array() && a.size() == 2 && a[0].is_number() && a[1].is_number()))
                        error(at, "'angle' must be [start, end] in degrees (0 = up, clockwise)");
                }
            } else if (type == "menu" || type == "buttons") {
                const int idx = param(at, el);
                if (idx >= 0) {
                    placed.insert(idx);
                    const auto& p = meta.params[(size_t)idx];
                    if (p.steps < 2) error(at, "param '" + p.name + "' has no 'steps'; use a knob or slider");
                    else if (p.steps > 64) error(at, "param '" + p.name + "' has too many steps for a " + type);
                    else if (p.labels.size() != (size_t)p.steps)
                        warn(at, "param '" + p.name + "' has no 'labels'; numbers will be shown");
                }
            } else {
                error(at, "unknown type (expected image, label, value, knob, slider, toggle, menu, buttons)");
            }
        }

        std::string missing;
        for (size_t k = 0; k < meta.params.size(); ++k)
            if (!placed.count((int)k)) missing += (missing.empty() ? "" : ", ") + meta.params[k].name;
        if (!missing.empty())
            warnings.push_back("ui.json: params without a control (reachable only via the Default view, the API "
                               "and automation): " + missing);
    }
};

// External references cannot load: the web view only serves the bundle (no network).
void checkWebFile(const std::vector<uint8_t>& wasm, const UiFile& f, json& warnings) {
    const std::string ext = extension(f.path);
    if (ext != "html" && ext != "htm" && ext != "css" && ext != "js" && ext != "mjs") return;
    const std::string text((const char*)wasm.data() + f.offset, f.size);
    for (const char* scheme : {"http://", "https://", "//cdn", "//fonts."}) {
        const size_t pos = text.find(scheme);
        if (pos == std::string::npos) continue;
        // Namespace URIs are not loads.
        const size_t from = pos >= 8 ? pos - 8 : 0;
        const std::string around = text.substr(from, 60);
        if (around.find("www.w3.org") != std::string::npos) continue;
        size_t end = text.find_first_of("\"' )>\n", pos);
        warnings.push_back(f.path + ": external URL '" + text.substr(pos, std::min<size_t>(end - pos, 80)) +
                           "' will not load (the UI has no network access); put the asset in the UI folder");
        break;
    }
}

}  // namespace

UiBundle UiBundle::fromWasm(const WasmInfo& info) {
    UiBundle b;
    const size_t prefix = std::strlen(kSectionPrefix);
    for (const auto& c : info.customs) {
        if (c.name.compare(0, prefix, kSectionPrefix) != 0) continue;
        const std::string path = c.name.substr(prefix);
        if (!validPath(path)) {
            b.problems.push_back("invalid UI file name '" + path + "' (letters, digits, '.', '_', '-', '/' only)");
            continue;
        }
        if (b.find(path)) {
            b.problems.push_back("duplicate UI file '" + path + "'");
            continue;
        }
        b.files.push_back({path, c.offset, c.size});
        b.totalBytes += c.size;
    }
    return b;
}

const UiFile* UiBundle::find(const std::string& path) const {
    for (const auto& f : files)
        if (f.path == path) return &f;
    return nullptr;
}

std::string UiBundle::kind() const {
    if (hasHtml()) return "html";
    if (hasSkin()) return "skin";
    return "none";
}

std::string uiMimeType(const std::string& path) {
    static const std::pair<const char*, const char*> types[] = {
        {"html", "text/html"}, {"htm", "text/html"}, {"js", "text/javascript"}, {"mjs", "text/javascript"},
        {"css", "text/css"}, {"json", "application/json"}, {"txt", "text/plain"}, {"svg", "image/svg+xml"},
        {"png", "image/png"}, {"jpg", "image/jpeg"}, {"jpeg", "image/jpeg"}, {"gif", "image/gif"},
        {"webp", "image/webp"}, {"ico", "image/x-icon"}, {"woff", "font/woff"}, {"woff2", "font/woff2"},
        {"ttf", "font/ttf"}, {"otf", "font/otf"}, {"wav", "audio/wav"}, {"mp3", "audio/mpeg"},
        {"wasm", "application/wasm"},
    };
    const std::string ext = extension(path);
    for (const auto& t : types)
        if (ext == t.first) return t.second;
    return "application/octet-stream";
}

int uiParamIndex(const InstrumentMeta& meta, const json& ref) {
    if (ref.is_number_integer()) return meta.paramIndexById(ref.get<int>());
    if (ref.is_string()) return meta.paramIndexByName(ref.get<std::string>());
    return -1;
}

bool uiImageSize(const uint8_t* d, size_t n, int& w, int& h) {
    auto be16 = [&](size_t i) { return (int)d[i] << 8 | d[i + 1]; };
    auto be32 = [&](size_t i) { return (int)((uint32_t)d[i] << 24 | (uint32_t)d[i + 1] << 16 | (uint32_t)d[i + 2] << 8 | d[i + 3]); };
    if (n >= 24 && startsWith(d, n, "\x89PNG\r\n\x1a\n", 8)) {
        w = be32(16);
        h = be32(20);
        return true;
    }
    if (n >= 10 && startsWith(d, n, "GIF8", 4)) {
        w = d[6] | d[7] << 8;
        h = d[8] | d[9] << 8;
        return true;
    }
    if (n >= 4 && d[0] == 0xff && d[1] == 0xd8) {
        size_t i = 2;
        while (i + 9 < n) {
            if (d[i] != 0xff) return false;
            const uint8_t marker = d[i + 1];
            if (marker == 0xd8 || (marker >= 0xd0 && marker <= 0xd7) || marker == 0x01) { i += 2; continue; }
            const size_t len = (size_t)be16(i + 2);
            // SOFn markers (except DHT/JPG/DAC) carry the frame size.
            if (marker >= 0xc0 && marker <= 0xcf && marker != 0xc4 && marker != 0xc8 && marker != 0xcc) {
                h = be16(i + 5);
                w = be16(i + 7);
                return true;
            }
            i += 2 + len;
        }
    }
    return false;
}

bool uiPreferredSize(const std::vector<uint8_t>& wasm, const UiBundle& ui, int& w, int& h) {
    if (const UiFile* f = ui.find("index.html")) {
        w = 720;
        h = 360;
        std::string text((const char*)wasm.data() + f->offset, std::min<size_t>(f->size, 16384));
        for (auto& c : text) c = (char)std::tolower((unsigned char)c);
        const size_t tag = text.find("name=\"aci-size\"");
        if (tag != std::string::npos) {
            const size_t open = text.rfind('<', tag), close = text.find('>', tag);
            const size_t content = text.find("content=\"", open);
            if (open != std::string::npos && content != std::string::npos && content < close) {
                int cw = 0, ch = 0;
                if (std::sscanf(text.c_str() + content + 9, "%dx%d", &cw, &ch) == 2 && cw >= 64 && ch >= 32 &&
                    cw <= 4096 && ch <= 4096) {
                    w = cw;
                    h = ch;
                }
            }
        }
        return true;
    }
    if (const UiFile* f = ui.find("ui.json")) {
        try {
            const json skin = json::parse(wasm.begin() + (std::ptrdiff_t)f->offset,
                                          wasm.begin() + (std::ptrdiff_t)(f->offset + f->size));
            w = skin.at("size").at(0).get<int>();
            h = skin.at("size").at(1).get<int>();
            return w > 0 && h > 0;
        } catch (const std::exception&) {
            return false;
        }
    }
    return false;
}

json checkUi(const std::vector<uint8_t>& wasm, const UiBundle& ui, const InstrumentMeta& meta) {
    json report = {{"kind", ui.kind()}};
    json errors = json::array(), warnings = json::array();
    for (const auto& p : ui.problems) errors.push_back(p);
    if (ui.empty() && errors.empty()) return report;

    json files = json::array();
    for (const auto& f : ui.files) files.push_back({{"path", f.path}, {"bytes", f.size}});
    report["files"] = files;
    report["bytes"] = ui.totalBytes;
    int w = 0, h = 0;
    if (uiPreferredSize(wasm, ui, w, h)) report["size"] = {w, h};
    if (ui.totalBytes > UiBundle::kMaxBytes)
        errors.push_back("UI bundle is " + std::to_string(ui.totalBytes / 1024) + " KiB; the limit is " +
                         std::to_string(UiBundle::kMaxBytes / 1024) + " KiB");
    else if (ui.totalBytes > 1024 * 1024)
        warnings.push_back("UI bundle is " + std::to_string(ui.totalBytes / 1024) +
                           " KiB; it is saved in every DAW project using it, so keep images small (WebP/JPEG, "
                           "2x the displayed size at most)");

    std::set<std::string> referenced;
    if (const UiFile* f = ui.find("ui.json")) {
        json skin;
        try {
            skin = json::parse(wasm.begin() + (std::ptrdiff_t)f->offset, wasm.begin() + (std::ptrdiff_t)(f->offset + f->size));
        } catch (const std::exception& e) {
            errors.push_back(std::string("ui.json is not valid JSON: ") + e.what());
        }
        if (!skin.is_null()) {
            SkinChecker c{wasm, ui, meta, errors, warnings, {}};
            c.run(skin);
            referenced = c.referenced;
        }
    }
    if (ui.hasHtml()) {
        for (const auto& f : ui.files) checkWebFile(wasm, f, warnings);
    } else {
        // Without a page nothing else can use the files, so unreferenced ones are dead weight.
        for (const auto& f : ui.files)
            if (f.path != "ui.json" && !referenced.count(f.path))
                warnings.push_back("UI file '" + f.path + "' is not used by ui.json");
        if (!ui.hasSkin() && errors.empty())
            errors.push_back("UI files present but neither index.html nor ui.json; the host will show the default UI");
    }
    report["errors"] = errors;
    report["warnings"] = warnings;
    return report;
}

}  // namespace aicontact
