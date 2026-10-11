#include "aicontact/WasmInfo.h"

#include <stdexcept>

namespace aicontact {

namespace {

struct Reader {
    const uint8_t* p;
    const uint8_t* end;

    uint8_t byte() {
        if (p >= end) throw std::runtime_error("unexpected end of wasm binary");
        return *p++;
    }
    uint32_t u32() {
        uint32_t result = 0;
        for (int shift = 0; shift < 35; shift += 7) {
            uint8_t b = byte();
            result |= uint32_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return result;
        }
        throw std::runtime_error("malformed LEB128");
    }
    std::string name() {
        uint32_t n = u32();
        if ((size_t)(end - p) < n) throw std::runtime_error("unexpected end of wasm binary");
        std::string s((const char*)p, n);
        p += n;
        return s;
    }
    void skip(size_t n) {
        if ((size_t)(end - p) < n) throw std::runtime_error("unexpected end of wasm binary");
        p += n;
    }
    void limits() {
        uint8_t flags = byte();
        u32();
        if (flags & 1) u32();
    }
};

const char* typeName(uint8_t t) {
    switch (t) {
        case 0x7f: return "i32";
        case 0x7e: return "i64";
        case 0x7d: return "f32";
        case 0x7c: return "f64";
        case 0x7b: return "v128";
        default: return "ref";
    }
}

}  // namespace

std::string WasmInfo::FuncType::signature() const {
    std::string s = "(";
    for (size_t i = 0; i < params.size(); ++i) { if (i) s += ","; s += typeName(params[i]); }
    s += ")->(";
    for (size_t i = 0; i < results.size(); ++i) { if (i) s += ","; s += typeName(results[i]); }
    return s + ")";
}

WasmInfo WasmInfo::parse(const std::vector<uint8_t>& bytes) {
    if (bytes.size() < 8 || bytes[0] != 0 || bytes[1] != 'a' || bytes[2] != 's' || bytes[3] != 'm')
        throw std::runtime_error("not a WebAssembly binary (missing \\0asm header)");
    if (bytes[4] != 1 || bytes[5] || bytes[6] || bytes[7])
        throw std::runtime_error("unsupported WebAssembly binary version");

    WasmInfo info;
    Reader r{bytes.data() + 8, bytes.data() + bytes.size()};
    while (r.p < r.end) {
        uint8_t id = r.byte();
        uint32_t size = r.u32();
        if ((size_t)(r.end - r.p) < size) throw std::runtime_error("section exceeds file size");
        Reader s{r.p, r.p + size};
        r.p += size;

        switch (id) {
            case 1: {  // type
                uint32_t n = s.u32();
                for (uint32_t i = 0; i < n; ++i) {
                    if (s.byte() != 0x60) throw std::runtime_error("unsupported type section entry");
                    FuncType ft;
                    uint32_t np = s.u32();
                    for (uint32_t k = 0; k < np; ++k) ft.params.push_back(s.byte());
                    uint32_t nr = s.u32();
                    for (uint32_t k = 0; k < nr; ++k) ft.results.push_back(s.byte());
                    info.types.push_back(std::move(ft));
                }
                break;
            }
            case 2: {  // import
                uint32_t n = s.u32();
                for (uint32_t i = 0; i < n; ++i) {
                    Import im;
                    im.module = s.name();
                    im.name = s.name();
                    im.kind = s.byte();
                    im.typeIndex = 0;
                    switch (im.kind) {
                        case 0: im.typeIndex = s.u32(); info.functionTypes.push_back(im.typeIndex); break;
                        case 1: s.byte(); s.limits(); break;      // table
                        case 2: s.limits(); break;                // memory
                        case 3: s.byte(); s.byte(); break;        // global
                        default: s.byte(); s.u32(); break;        // tag
                    }
                    info.imports.push_back(std::move(im));
                }
                break;
            }
            case 3: {  // function
                uint32_t n = s.u32();
                for (uint32_t i = 0; i < n; ++i) info.functionTypes.push_back(s.u32());
                break;
            }
            case 7: {  // export
                uint32_t n = s.u32();
                for (uint32_t i = 0; i < n; ++i) {
                    Export ex;
                    ex.name = s.name();
                    ex.kind = s.byte();
                    ex.index = s.u32();
                    info.exports.push_back(std::move(ex));
                }
                break;
            }
            case 0: {  // custom
                std::string name = s.name();
                info.customs.push_back({std::move(name), (size_t)(s.p - bytes.data()), (size_t)(s.end - s.p)});
                break;
            }
            default:
                break;
        }
    }
    return info;
}

const WasmInfo::Export* WasmInfo::findExport(const std::string& name) const {
    for (const auto& e : exports)
        if (e.name == name) return &e;
    return nullptr;
}

std::string WasmInfo::exportSignature(const std::string& name) const {
    const Export* e = findExport(name);
    if (!e || e->kind != 0 || e->index >= functionTypes.size()) return {};
    uint32_t t = functionTypes[e->index];
    if (t >= types.size()) return {};
    return types[t].signature();
}

std::vector<std::string> checkAciStructure(const WasmInfo& info) {
    std::vector<std::string> problems;

    for (const auto& im : info.imports) {
        bool ok = im.module == "env" && im.name == "aci_log" && im.kind == 0 &&
                  im.typeIndex < info.types.size() &&
                  info.types[im.typeIndex].signature() == "(i32,i32)->()";
        if (!ok)
            problems.push_back("forbidden import '" + im.module + "." + im.name +
                               "' (ACI modules may only import env.aci_log(i32,i32); "
                               "use sdk/c/aci.h for math/memory functions)");
    }

    const WasmInfo::Export* mem = info.findExport("memory");
    if (!mem || mem->kind != 2) problems.push_back("missing exported memory 'memory'");

    struct Req { const char* name; const char* sig; bool required; };
    static const Req reqs[] = {
        {"aci_abi_version", "()->(i32)", true},
        {"aci_meta", "()->(i32)", true},
        {"aci_init", "(f32,i32)->(i32)", true},
        {"aci_output", "(i32)->(i32)", true},
        {"aci_process", "(i32)->()", true},
        {"aci_note_on", "(i32,f32)->()", true},
        {"aci_note_off", "(i32,f32)->()", true},
        {"aci_set_param", "(i32,f32)->()", false},
        {"aci_pitch_bend", "(f32)->()", false},
        {"aci_control_change", "(i32,f32)->()", false},
        {"aci_channel_pressure", "(f32)->()", false},
        {"aci_all_notes_off", "()->()", false},
        {"aci_reset", "()->()", false},
    };
    for (const auto& r : reqs) {
        std::string sig = info.exportSignature(r.name);
        if (sig.empty()) {
            if (r.required) problems.push_back(std::string("missing required export '") + r.name + "' " + r.sig);
        } else if (sig != r.sig) {
            problems.push_back(std::string("export '") + r.name + "' has signature " + sig +
                               ", expected " + r.sig);
        }
    }
    return problems;
}

}  // namespace aicontact
