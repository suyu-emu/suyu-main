// SPDX-FileCopyrightText: Copyright 2026 suyu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/arm/recomp/recomp_gaps.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <system_error>
#include <utility>

namespace Core::RecompGaps {

namespace {

std::uint64_t SatAdd(std::uint64_t a, std::uint64_t b) {
    return a > std::numeric_limits<std::uint64_t>::max() - b
               ? std::numeric_limits<std::uint64_t>::max()
               : a + b;
}

std::string Hex(std::uint64_t value) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%llx", static_cast<unsigned long long>(value));
    return buf;
}

bool ParseHex(std::string_view text, std::uint64_t& out) {
    if (text.empty() || text.size() > 16) {
        return false;
    }
    std::uint64_t v = 0;
    for (const char c : text) {
        v <<= 4;
        if (c >= '0' && c <= '9') {
            v |= static_cast<std::uint64_t>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            v |= static_cast<std::uint64_t>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            v |= static_cast<std::uint64_t>(c - 'A' + 10);
        } else {
            return false;
        }
    }
    out = v;
    return true;
}

// ---- A small JSON reader, enough for this schema. ----

struct Value {
    enum class Type { Null, Bool, Number, String, Array, Object } type = Type::Null;
    bool boolean = false;
    std::string text; // string contents, or the number's literal
    std::vector<Value> array;
    // Keys and values side by side: a std::pair holding Value cannot be formed while
    // Value is still incomplete, which clang with libstdc++ rejects.
    std::vector<std::string> keys;
    std::vector<Value> values;

    const Value* Get(std::string_view key) const {
        for (std::size_t i = 0; i < keys.size(); ++i) {
            if (keys[i] == key) {
                return &values[i];
            }
        }
        return nullptr;
    }
};

class Reader {
public:
    explicit Reader(std::string_view in) : s{in} {}

    bool Document(Value& out, std::string& error) {
        if (!Parse(out, 0)) {
            error = err.empty() ? "invalid JSON" : err;
            return false;
        }
        Skip();
        if (pos != s.size()) {
            error = "trailing data after JSON";
            return false;
        }
        return true;
    }

private:
    std::string_view s;
    std::size_t pos = 0;
    std::string err;

    void Skip() {
        while (pos < s.size() &&
               (s[pos] == ' ' || s[pos] == '\t' || s[pos] == '\n' || s[pos] == '\r')) {
            ++pos;
        }
    }
    bool Fail(const char* what) {
        if (err.empty()) {
            err = std::string{what} + " at byte " + std::to_string(pos);
        }
        return false;
    }
    bool Literal(std::string_view word) {
        if (s.substr(pos, word.size()) != word) {
            return Fail("unexpected token");
        }
        pos += word.size();
        return true;
    }
    bool String(std::string& out) {
        ++pos; // opening quote
        while (pos < s.size()) {
            const char c = s[pos++];
            if (c == '"') {
                return true;
            }
            if (static_cast<unsigned char>(c) < 0x20) {
                return Fail("control character in string");
            }
            if (c != '\\') {
                out += c;
                continue;
            }
            if (pos >= s.size()) {
                break;
            }
            const char e = s[pos++];
            switch (e) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                std::uint64_t cp = 0;
                if (pos + 4 > s.size() || !ParseHex(s.substr(pos, 4), cp)) {
                    return Fail("bad \\u escape");
                }
                pos += 4;
                // Nothing in this schema needs more than ASCII; keep the rest
                // recognisable rather than decoding surrogates.
                out += cp < 0x80 ? static_cast<char>(cp) : '?';
                break;
            }
            default:
                return Fail("bad escape");
            }
        }
        return Fail("unterminated string");
    }
    bool Number(std::string& out) {
        const std::size_t start = pos;
        if (pos < s.size() && s[pos] == '-') {
            ++pos;
        }
        while (pos < s.size() && ((s[pos] >= '0' && s[pos] <= '9') || s[pos] == '.' ||
                                  s[pos] == 'e' || s[pos] == 'E' || s[pos] == '+' ||
                                  s[pos] == '-')) {
            ++pos;
        }
        if (pos == start) {
            return Fail("unexpected character");
        }
        out = std::string{s.substr(start, pos - start)};
        return true;
    }
    bool Parse(Value& v, int depth) {
        if (depth > 16) {
            return Fail("nesting too deep");
        }
        Skip();
        if (pos >= s.size()) {
            return Fail("unexpected end");
        }
        const char c = s[pos];
        if (c == '{') {
            v.type = Value::Type::Object;
            ++pos;
            Skip();
            if (pos < s.size() && s[pos] == '}') {
                ++pos;
                return true;
            }
            while (true) {
                Skip();
                if (pos >= s.size() || s[pos] != '"') {
                    return Fail("expected key");
                }
                std::string key;
                if (!String(key)) {
                    return false;
                }
                Skip();
                if (pos >= s.size() || s[pos] != ':') {
                    return Fail("expected ':'");
                }
                ++pos;
                Value item;
                if (!Parse(item, depth + 1)) {
                    return false;
                }
                v.keys.push_back(std::move(key));
                v.values.push_back(std::move(item));
                Skip();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == '}') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            v.type = Value::Type::Array;
            ++pos;
            Skip();
            if (pos < s.size() && s[pos] == ']') {
                ++pos;
                return true;
            }
            while (true) {
                Value item;
                if (!Parse(item, depth + 1)) {
                    return false;
                }
                v.array.push_back(std::move(item));
                Skip();
                if (pos < s.size() && s[pos] == ',') {
                    ++pos;
                    continue;
                }
                if (pos < s.size() && s[pos] == ']') {
                    ++pos;
                    return true;
                }
                return Fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            v.type = Value::Type::String;
            return String(v.text);
        }
        if (c == 't' || c == 'f') {
            v.type = Value::Type::Bool;
            v.boolean = c == 't';
            return Literal(v.boolean ? "true" : "false");
        }
        if (c == 'n') {
            v.type = Value::Type::Null;
            return Literal("null");
        }
        v.type = Value::Type::Number;
        return Number(v.text);
    }
};

/// A non-negative integer, saturating; nullopt for anything else.
std::optional<std::uint64_t> AsCount(const Value* v) {
    if (!v || v->type != Value::Type::Number || v->text.empty() || v->text[0] == '-') {
        return std::nullopt;
    }
    std::uint64_t out = 0;
    for (const char c : v->text) {
        if (c < '0' || c > '9') {
            return std::nullopt;
        }
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (out > (std::numeric_limits<std::uint64_t>::max() - digit) / 10) {
            out = std::numeric_limits<std::uint64_t>::max();
        } else {
            out = out * 10 + digit;
        }
    }
    return out;
}

std::uint64_t CountOr0(const Value* v) {
    return AsCount(v).value_or(0);
}

bool AddOffset(ModuleGaps& m, std::uint64_t offset, std::uint64_t hits, bool& truncated) {
    const auto it = m.offsets.find(offset);
    if (it != m.offsets.end()) {
        it->second = SatAdd(it->second, hits);
        return true;
    }
    if (m.offsets.size() >= kMaxOffsetsPerModule) {
        truncated = true;
        return false;
    }
    m.offsets.emplace(offset, hits);
    return true;
}

ModuleGaps* FindOrAddModule(std::map<std::string, ModuleGaps>& map, const std::string& build_id,
                            std::string_view name, bool& truncated) {
    auto it = map.find(build_id);
    if (it == map.end()) {
        if (map.size() >= kMaxModules) {
            truncated = true;
            return nullptr;
        }
        it = map.emplace(build_id, ModuleGaps{}).first;
        it->second.build_id = build_id;
    }
    if (it->second.name.empty()) {
        it->second.name = SanitizeName(name);
    }
    return &it->second;
}

void AddOpcode(GapData& d, std::uint32_t insn, std::uint64_t hits) {
    const auto it = d.unimplemented.find(insn);
    if (it != d.unimplemented.end()) {
        it->second = SatAdd(it->second, hits);
    } else if (d.unimplemented.size() >= kMaxOpcodes) {
        d.truncated = true;
    } else {
        d.unimplemented.emplace(insn, hits);
    }
}

void MergeModules(std::map<std::string, ModuleGaps>& into,
                  const std::map<std::string, ModuleGaps>& from, bool& truncated) {
    for (const auto& [id, m] : from) {
        ModuleGaps* target = FindOrAddModule(into, id, m.name, truncated);
        if (!target) {
            continue;
        }
        target->hits = SatAdd(target->hits, m.hits);
        for (const auto& [offset, hits] : m.offsets) {
            AddOffset(*target, offset, hits, truncated);
        }
    }
}

void AppendEscaped(std::string& o, std::string_view text) {
    o += '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') {
            o += '\\';
            o += c;
        } else if (static_cast<unsigned char>(c) < 0x20) {
            o += ' ';
        } else {
            o += c;
        }
    }
    o += '"';
}

void WriteModules(std::string& o, const char* key, const std::map<std::string, ModuleGaps>& map,
                  bool with_offsets) {
    o += "  \"";
    o += key;
    o += "\": [";
    bool first = true;
    for (const auto& [id, m] : map) {
        o += first ? "\n    {" : ",\n    {";
        first = false;
        o += "\"name\": ";
        AppendEscaped(o, SanitizeName(m.name));
        o += ", \"build_id\": \"" + id + "\", \"hits\": " + std::to_string(m.hits);
        if (with_offsets) {
            o += ", \"offsets\": [";
            bool first_offset = true;
            for (const auto& [offset, hits] : m.offsets) {
                o += first_offset ? "" : ", ";
                first_offset = false;
                o += "[\"" + Hex(offset) + "\", " + std::to_string(hits) + "]";
            }
            o += "]";
        }
        o += "}";
    }
    o += first ? "],\n" : "\n  ],\n";
}

bool ReadModules(const Value* list, std::map<std::string, ModuleGaps>& out, bool with_offsets,
                 bool& truncated, std::string& error) {
    if (!list) {
        return true;
    }
    if (list->type != Value::Type::Array) {
        error = "module list is not an array";
        return false;
    }
    for (const Value& item : list->array) {
        if (item.type != Value::Type::Object) {
            error = "module entry is not an object";
            return false;
        }
        const Value* id = item.Get("build_id");
        const std::string build_id =
            id && id->type == Value::Type::String ? NormalizeBuildId(id->text) : std::string{};
        if (build_id.empty()) {
            // Not an error: an entry nobody can match is simply of no use.
            continue;
        }
        const Value* name = item.Get("name");
        ModuleGaps* m = FindOrAddModule(
            out, build_id,
            name && name->type == Value::Type::String ? std::string_view{name->text}
                                                      : std::string_view{},
            truncated);
        if (!m) {
            continue;
        }
        m->hits = SatAdd(m->hits, CountOr0(item.Get("hits")));
        if (!with_offsets) {
            continue;
        }
        const Value* offsets = item.Get("offsets");
        if (!offsets) {
            continue;
        }
        if (offsets->type != Value::Type::Array) {
            error = "offsets is not an array";
            return false;
        }
        for (const Value& pair : offsets->array) {
            std::uint64_t offset = 0;
            if (pair.type != Value::Type::Array || pair.array.size() != 2 ||
                pair.array[0].type != Value::Type::String ||
                !ParseHex(pair.array[0].text, offset) || !AsCount(&pair.array[1])) {
                error = "bad offset entry";
                return false;
            }
            AddOffset(*m, offset, *AsCount(&pair.array[1]), truncated);
        }
    }
    return true;
}

} // namespace

std::uint64_t GapData::GapOffsets() const {
    std::uint64_t n = 0;
    for (const auto& [id, m] : modules) {
        n += m.offsets.size();
    }
    return n;
}

std::uint64_t GapData::Misses() const {
    std::uint64_t n = unattributed_misses;
    for (const auto& [id, m] : modules) {
        n = SatAdd(n, m.hits);
    }
    for (const auto& [id, m] : no_image) {
        n = SatAdd(n, m.hits);
    }
    return n;
}

std::uint64_t GapData::UnsupportedInstructionKinds() const {
    return std::max<std::uint64_t>(unimplemented.size(), imported_unsupported_kinds);
}

bool GapData::Clean() const {
    return Misses() == 0 && modules.empty() && no_image.empty() && unimplemented.empty() &&
           imported_unsupported_kinds == 0;
}

std::string TitleIdHex(std::uint64_t title_id) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llX", static_cast<unsigned long long>(title_id));
    return buf;
}

std::string BuildIdHex(const std::uint8_t* bytes, std::size_t size) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::size_t i = 0; i < 32; ++i) {
        const std::uint8_t b = i < size && bytes ? bytes[i] : 0;
        out += kDigits[b >> 4];
        out += kDigits[b & 15];
    }
    return out;
}

std::string NormalizeBuildId(std::string_view text) {
    if (text.empty() || text.size() > 64) {
        return {};
    }
    std::string out;
    out.reserve(64);
    bool nonzero = false;
    for (const char c : text) {
        char l = c;
        if (l >= 'A' && l <= 'F') {
            l = static_cast<char>(l - 'A' + 'a');
        }
        if (!((l >= '0' && l <= '9') || (l >= 'a' && l <= 'f'))) {
            return {};
        }
        nonzero |= l != '0';
        out += l;
    }
    if (!nonzero) {
        return {};
    }
    out.append(64 - out.size(), '0');
    return out;
}

bool BuildIdMatches(std::string_view a, std::string_view b) {
    const std::string na = NormalizeBuildId(a);
    return !na.empty() && na == NormalizeBuildId(b);
}

std::string SanitizeName(std::string_view name) {
    const std::size_t slash = name.find_last_of("/\\:");
    if (slash != std::string_view::npos) {
        name.remove_prefix(slash + 1);
    }
    std::string out;
    for (const char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
            c == '.' || c == '_' || c == '-') {
            out += c;
            if (out.size() == 64) {
                break;
            }
        }
    }
    return out;
}

std::optional<std::size_t> MatchImage(const std::vector<ImageIdentity>& images,
                                      std::size_t load_index, std::string_view module_name,
                                      std::string_view module_build_id) {
    const bool by_build_id = std::any_of(images.begin(), images.end(), [](const auto& image) {
        return !NormalizeBuildId(image.build_id).empty();
    });
    if (by_build_id) {
        for (std::size_t i = 0; i < images.size(); ++i) {
            if (BuildIdMatches(images[i].build_id, module_build_id)) {
                return i;
            }
        }
        return std::nullopt;
    }
    const auto by_name = [&images](std::string_view name) -> std::optional<std::size_t> {
        for (std::size_t i = 0; i < images.size(); ++i) {
            const std::string_view image = images[i].name;
            if (image.size() == name.size() &&
                std::equal(image.begin(), image.end(), name.begin(), [](char a, char b) {
                    const auto lower = [](char c) {
                        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
                    };
                    return lower(a) == lower(b);
                })) {
                return i;
            }
        }
        return std::nullopt;
    };
    if (auto i = by_name(module_name)) {
        return i;
    }
    if (module_name.size() > 2 && (module_name[0] == 'n' || module_name[0] == 'N') &&
        (module_name[1] == 'n' || module_name[1] == 'N')) {
        if (auto i = by_name(module_name.substr(2))) {
            return i;
        }
    }
    static constexpr std::string_view kSlots[] = {
        "rtld",    "main",    "subsdk0", "subsdk1", "subsdk2", "subsdk3", "subsdk4",
        "subsdk5", "subsdk6", "subsdk7", "subsdk8", "subsdk9", "sdk",
    };
    if (load_index < std::size(kSlots)) {
        return by_name(kSlots[load_index]);
    }
    return std::nullopt;
}

std::string ModuleNameFromRodata(const std::uint8_t* rodata, std::size_t size) {
    constexpr std::size_t kPathMax = 0x200;
    const auto word = [&](std::size_t at) -> std::optional<std::uint32_t> {
        if (at + 4 > size) {
            return std::nullopt;
        }
        return static_cast<std::uint32_t>(rodata[at]) | (std::uint32_t{rodata[at + 1]} << 8) |
               (std::uint32_t{rodata[at + 2]} << 16) | (std::uint32_t{rodata[at + 3]} << 24);
    };
    // {u32 0, s32 length, char path[length]} at `at`, ending by `limit`.
    const auto path_at = [&](std::size_t at, std::size_t limit) -> std::string {
        const auto zero = word(at);
        const auto length = word(at + 4);
        if (!zero || !length || *zero != 0 || *length == 0 || *length > 0x7fffffffu ||
            at + 8 + *length > limit) {
            return {};
        }
        const std::size_t start = at + 8;
        std::size_t end = start + (std::min<std::size_t>)({*length, kPathMax - 1, size - start});
        end = static_cast<std::size_t>(std::find(rodata + start, rodata + end, '\0') - rodata);
        std::size_t name = start;
        for (std::size_t i = start; i < end; ++i) {
            if (rodata[i] == '/' || rodata[i] == '\\') {
                name = i + 1;
            }
        }
        return std::string(reinterpret_cast<const char*>(rodata) + name, end - name);
    };
    const auto first = word(0);
    if (!first) {
        return {};
    }
    if (*first == 0) {
        return path_at(0, std::numeric_limits<std::size_t>::max());
    }
    // The newer header's second word is where the path struct after it ends.
    const auto path_end = word(4);
    if (*first == 1 && path_end && word(8)) {
        return path_at(12, *path_end);
    }
    return {};
}

namespace {

std::uint64_t ReadLe(const GuestRead8& read8, std::uint64_t va, int bytes) {
    std::uint64_t v = 0;
    for (int i = 0; i < bytes; ++i) {
        v |= std::uint64_t{read8(va + i)} << (8 * i);
    }
    return v;
}

} // namespace

std::uint64_t FindMod0(std::uint64_t module_base, const GuestRead8& read8) {
    constexpr std::uint64_t kMagic = 0x30444F4Du; // "MOD0"
    const std::uint64_t pointed = module_base + ReadLe(read8, module_base + 4, 4);
    if (ReadLe(read8, pointed, 4) == kMagic) {
        return pointed;
    }
    for (std::uint64_t off = 0; off < 0x4000; off += 4) {
        if (ReadLe(read8, module_base + off, 4) == kMagic) {
            return module_base + off;
        }
    }
    return 0;
}

DynSymbol ReadDynSymbol(std::uint64_t symtab_va, std::uint64_t strtab_va, std::uint32_t index,
                        const GuestRead8& read8) {
    // Elf64_Sym: st_name(4) st_info(1) st_other(1) st_shndx(2) st_value(8)
    // st_size(8) = 24 bytes.
    DynSymbol s;
    if (!symtab_va) {
        return s;
    }
    const std::uint64_t sym_va = symtab_va + std::uint64_t{index} * 24;
    const std::uint64_t name_off = ReadLe(read8, sym_va, 4);
    s.weak = (read8(sym_va + 4) >> 4) == 2; // binding is st_info's high nibble
    s.defined = ReadLe(read8, sym_va + 6, 2) != 0; // SHN_UNDEF == 0
    s.value = ReadLe(read8, sym_va + 8, 8);
    if (strtab_va) {
        for (std::uint64_t i = 0; i < 512; ++i) {
            const auto c = read8(strtab_va + name_off + i);
            if (!c) {
                break;
            }
            s.name.push_back(static_cast<char>(c));
        }
    }
    return s;
}

void IndexModuleExports(std::uint64_t module_base, std::uint64_t symtab_va,
                        std::uint64_t strtab_va, const GuestRead8& read8,
                        std::unordered_map<std::string, std::uint64_t>& out) {
    if (!symtab_va || !strtab_va) {
        return;
    }
    std::uint32_t max_index = 8192;
    if (strtab_va > symtab_va) {
        max_index = static_cast<std::uint32_t>(
            (std::min<std::uint64_t>)((strtab_va - symtab_va) / 24, 65536));
    }
    for (std::uint32_t i = 1; i < max_index; ++i) { // 0 is the null symbol
        const auto sym = ReadDynSymbol(symtab_va, strtab_va, i, read8);
        if (sym.defined && !sym.name.empty()) {
            out.emplace(sym.name, module_base + sym.value);
        }
    }
}

std::string Serialize(const GapData& d) {
    std::string o = "{\n";
    o += "  \"schema\": \"" + std::string{kSchemaName} + "\",\n";
    o += "  \"schema_version\": " + std::to_string(kSchemaVersion) + ",\n";
    o += "  \"title_id\": \"" + SanitizeName(d.title_id) + "\",\n";
    o += "  \"runs\": " + std::to_string(d.runs) + ",\n";
    o += "  \"hybrid_runs\": " + std::to_string(d.hybrid_runs) + ",\n";
    o += "  \"strict_static_runs\": " + std::to_string(d.strict_runs) + ",\n";
    o += "  \"clean_runs\": " + std::to_string(d.clean_runs) + ",\n";
    o += std::string{"  \"last_run_strict_static\": "} + (d.last_run_strict ? "true" : "false") +
         ",\n";
    o += std::string{"  \"truncated\": "} + (d.truncated ? "true" : "false") + ",\n";
    o += "  \"unattributed_misses\": " + std::to_string(d.unattributed_misses) + ",\n";
    if (d.imported_unsupported_kinds != 0 || d.imported_unsupported_hits != 0) {
        o += "  \"imported_unsupported_instruction_kinds\": " +
             std::to_string(d.imported_unsupported_kinds) + ",\n";
        o += "  \"imported_unsupported_instruction_hits\": " +
             std::to_string(d.imported_unsupported_hits) + ",\n";
    }
    WriteModules(o, "modules", d.modules, true);
    WriteModules(o, "modules_without_image", d.no_image, false);
    o += "  \"unimplemented_opcodes\": [";
    bool first = true;
    for (const auto& [insn, hits] : d.unimplemented) {
        char enc[16];
        std::snprintf(enc, sizeof enc, "%08x", insn);
        o += first ? "" : ", ";
        first = false;
        o += "[\"" + std::string{enc} + "\", " + std::to_string(hits) + "]";
    }
    o += "]\n}\n";
    return o;
}

std::optional<GapData> Parse(std::string_view json, std::string* error) {
    std::string local;
    std::string& e = error ? *error : local;
    if (json.size() > kMaxFileBytes) {
        e = "file is too large";
        return std::nullopt;
    }
    Value root;
    if (!Reader{json}.Document(root, e)) {
        return std::nullopt;
    }
    if (root.type != Value::Type::Object) {
        e = "not a JSON object";
        return std::nullopt;
    }
    const Value* schema = root.Get("schema");
    if (!schema || schema->type != Value::Type::String || schema->text != kSchemaName) {
        e = "not a suyu coverage file";
        return std::nullopt;
    }
    const auto version = AsCount(root.Get("schema_version"));
    if (!version || *version == 0) {
        e = "missing schema_version";
        return std::nullopt;
    }
    if (*version > kSchemaVersion) {
        e = "schema_version " + std::to_string(*version) + " is newer than this suyu reads (" +
            std::to_string(kSchemaVersion) + ")";
        return std::nullopt;
    }
    GapData d;
    if (const Value* t = root.Get("title_id"); t && t->type == Value::Type::String) {
        std::uint64_t id = 0;
        if (!t->text.empty() && !ParseHex(t->text, id)) {
            e = "bad title_id";
            return std::nullopt;
        }
        d.title_id = t->text.empty() ? std::string{} : TitleIdHex(id);
    }
    d.runs = CountOr0(root.Get("runs"));
    d.hybrid_runs = CountOr0(root.Get("hybrid_runs"));
    d.strict_runs = CountOr0(root.Get("strict_static_runs"));
    d.clean_runs = CountOr0(root.Get("clean_runs"));
    d.unattributed_misses = CountOr0(root.Get("unattributed_misses"));
    d.imported_unsupported_kinds = CountOr0(root.Get("imported_unsupported_instruction_kinds"));
    d.imported_unsupported_hits = CountOr0(root.Get("imported_unsupported_instruction_hits"));
    if (const Value* b = root.Get("last_run_strict_static"); b && b->type == Value::Type::Bool) {
        d.last_run_strict = b->boolean;
    }
    if (const Value* b = root.Get("truncated"); b && b->type == Value::Type::Bool) {
        d.truncated = b->boolean;
    }
    if (!ReadModules(root.Get("modules"), d.modules, true, d.truncated, e) ||
        !ReadModules(root.Get("modules_without_image"), d.no_image, false, d.truncated, e)) {
        return std::nullopt;
    }
    if (const Value* ops = root.Get("unimplemented_opcodes")) {
        if (ops->type != Value::Type::Array) {
            e = "unimplemented_opcodes is not an array";
            return std::nullopt;
        }
        for (const Value& pair : ops->array) {
            std::uint64_t insn = 0;
            if (pair.type != Value::Type::Array || pair.array.size() != 2 ||
                pair.array[0].type != Value::Type::String ||
                !ParseHex(pair.array[0].text, insn) || insn > 0xFFFFFFFFull ||
                !AsCount(&pair.array[1])) {
                e = "bad opcode entry";
                return std::nullopt;
            }
            AddOpcode(d, static_cast<std::uint32_t>(insn), *AsCount(&pair.array[1]));
        }
    }
    return d;
}

void Merge(GapData& into, const GapData& from) {
    if (into.title_id.empty()) {
        into.title_id = from.title_id;
    }
    into.runs = SatAdd(into.runs, from.runs);
    into.hybrid_runs = SatAdd(into.hybrid_runs, from.hybrid_runs);
    into.strict_runs = SatAdd(into.strict_runs, from.strict_runs);
    into.clean_runs = SatAdd(into.clean_runs, from.clean_runs);
    into.unattributed_misses = SatAdd(into.unattributed_misses, from.unattributed_misses);
    if (from.runs != 0) {
        into.last_run_strict = from.last_run_strict;
    }
    into.truncated |= from.truncated;
    MergeModules(into.modules, from.modules, into.truncated);
    MergeModules(into.no_image, from.no_image, into.truncated);
    for (const auto& [insn, hits] : from.unimplemented) {
        AddOpcode(into, insn, hits);
    }
    into.imported_unsupported_kinds =
        std::max(into.imported_unsupported_kinds, from.imported_unsupported_kinds);
    into.imported_unsupported_hits =
        SatAdd(into.imported_unsupported_hits, from.imported_unsupported_hits);
}

// ---- Shared coverage ----

namespace {

std::uint64_t Cap(std::uint64_t value) {
    return std::min(value, kMaxSharedCount);
}

bool ValidSharedOffset(std::uint64_t offset) {
    return offset < kMaxSharedOffset && (offset & 3) == 0;
}

bool IsCanonicalBuildId(std::string_view id) {
    return id.size() == 64 && NormalizeBuildId(id) == id;
}

/// Exactly these keys, each at most once.
bool KeysAre(const Value& object, std::initializer_list<std::string_view> allowed,
             std::string& error, std::string_view where) {
    std::vector<std::string_view> seen;
    for (const std::string& key : object.keys) {
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end()) {
            error = "unsupported field '" + SanitizeName(key) + "' in " + std::string{where};
            return false;
        }
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            error = "duplicate field '" + SanitizeName(key) + "' in " + std::string{where};
            return false;
        }
        seen.push_back(key);
    }
    return true;
}

std::optional<std::uint64_t> SharedCount(const Value* v, std::string& error,
                                         std::string_view what) {
    const auto count = AsCount(v);
    if (!count || *count > kMaxSharedCount) {
        error = std::string{what} + " is not a count";
        return std::nullopt;
    }
    return count;
}

} // namespace

SharedCoverage ToShared(const GapData& d) {
    SharedCoverage out;
    std::uint64_t id = 0;
    out.title_id = d.title_id.size() == 16 && ParseHex(d.title_id, id) && id != 0
                       ? TitleIdHex(id)
                       : std::string{};
    out.runs = Cap(d.runs);
    out.hybrid_runs = Cap(d.hybrid_runs);
    out.strict_runs = Cap(d.strict_runs);
    out.clean_runs = Cap(d.clean_runs);
    out.truncated = d.truncated;
    out.unattributed_misses = Cap(d.unattributed_misses);
    // Counted, never listed: the encodings themselves stay in local diagnostics.
    out.unsupported_instruction_kinds = Cap(d.UnsupportedInstructionKinds());
    std::uint64_t hits = d.imported_unsupported_hits;
    for (const auto& [insn, count] : d.unimplemented) {
        hits = SatAdd(hits, count);
    }
    out.unsupported_instruction_hits = Cap(hits);
    for (const auto& [build_id, m] : d.modules) {
        if (!IsCanonicalBuildId(build_id)) {
            continue;
        }
        if (out.modules.size() >= kMaxModules) {
            out.truncated = true;
            break;
        }
        SharedModule& target = out.modules[build_id];
        target.hits = Cap(m.hits);
        for (const auto& [offset, count] : m.offsets) {
            if (ValidSharedOffset(offset) && target.offsets.size() < kMaxOffsetsPerModule) {
                target.offsets.emplace(offset, Cap(count));
            }
        }
    }
    for (const auto& [build_id, m] : d.no_image) {
        if (IsCanonicalBuildId(build_id) && out.modules_without_image.size() < kMaxModules) {
            out.modules_without_image.emplace(build_id, Cap(m.hits));
        }
    }
    return out;
}

std::string SerializeShared(const SharedCoverage& d) {
    std::string o = "{\n";
    o += "  \"schema\": \"" + std::string{kSharedSchemaName} + "\",\n";
    o += "  \"schema_version\": " + std::to_string(kSharedSchemaVersion) + ",\n";
    o += "  \"description\": \"" + std::string{kSharedDescription} + "\",\n";
    o += "  \"title_id\": \"" + d.title_id + "\",\n";
    o += "  \"runs\": " + std::to_string(d.runs) + ",\n";
    o += "  \"hybrid_runs\": " + std::to_string(d.hybrid_runs) + ",\n";
    o += "  \"strict_static_runs\": " + std::to_string(d.strict_runs) + ",\n";
    o += "  \"clean_runs\": " + std::to_string(d.clean_runs) + ",\n";
    o += std::string{"  \"truncated\": "} + (d.truncated ? "true" : "false") + ",\n";
    o += "  \"unattributed_misses\": " + std::to_string(d.unattributed_misses) + ",\n";
    o += "  \"unsupported_instruction_kinds\": " +
         std::to_string(d.unsupported_instruction_kinds) + ",\n";
    o += "  \"unsupported_instruction_hits\": " + std::to_string(d.unsupported_instruction_hits) +
         ",\n";
    o += "  \"modules\": [";
    bool first = true;
    for (const auto& [id, m] : d.modules) {
        o += first ? "\n    {" : ",\n    {";
        first = false;
        o += "\"build_id\": \"" + id + "\", \"hits\": " + std::to_string(m.hits) +
             ", \"offsets\": [";
        bool first_offset = true;
        for (const auto& [offset, hits] : m.offsets) {
            o += first_offset ? "" : ", ";
            first_offset = false;
            o += "[\"" + Hex(offset) + "\", " + std::to_string(hits) + "]";
        }
        o += "]}";
    }
    o += first ? "],\n" : "\n  ],\n";
    o += "  \"modules_without_image\": [";
    first = true;
    for (const auto& [id, hits] : d.modules_without_image) {
        o += first ? "\n    {" : ",\n    {";
        first = false;
        o += "\"build_id\": \"" + id + "\", \"hits\": " + std::to_string(hits) + "}";
    }
    o += first ? "]\n}\n" : "\n  ]\n}\n";
    return o;
}

std::optional<SharedCoverage> ParseShared(std::string_view json, std::string* error) {
    std::string local;
    std::string& e = error ? *error : local;
    if (json.size() > kMaxFileBytes) {
        e = "file is too large";
        return std::nullopt;
    }
    Value root;
    if (!Reader{json}.Document(root, e)) {
        return std::nullopt;
    }
    if (root.type != Value::Type::Object) {
        e = "not a JSON object";
        return std::nullopt;
    }
    if (!KeysAre(root,
                 {"schema", "schema_version", "description", "title_id", "runs", "hybrid_runs",
                  "strict_static_runs", "clean_runs", "truncated", "unattributed_misses",
                  "unsupported_instruction_kinds", "unsupported_instruction_hits", "modules",
                  "modules_without_image"},
                 e, "the file")) {
        return std::nullopt;
    }
    const Value* schema = root.Get("schema");
    if (!schema || schema->type != Value::Type::String || schema->text != kSharedSchemaName) {
        e = "not a suyu shared coverage file";
        return std::nullopt;
    }
    const auto version = AsCount(root.Get("schema_version"));
    if (!version || *version != kSharedSchemaVersion) {
        e = "unsupported schema_version";
        return std::nullopt;
    }
    if (const Value* text = root.Get("description");
        text && (text->type != Value::Type::String || text->text != kSharedDescription)) {
        e = "description is not the schema's";
        return std::nullopt;
    }
    SharedCoverage d;
    const Value* title = root.Get("title_id");
    std::uint64_t title_id = 0;
    if (!title || title->type != Value::Type::String || title->text.size() != 16 ||
        !ParseHex(title->text, title_id) || title_id == 0) {
        e = "bad title_id";
        return std::nullopt;
    }
    d.title_id = TitleIdHex(title_id);
    const std::pair<const char*, std::uint64_t*> counts[] = {
        {"runs", &d.runs},
        {"hybrid_runs", &d.hybrid_runs},
        {"strict_static_runs", &d.strict_runs},
        {"clean_runs", &d.clean_runs},
        {"unattributed_misses", &d.unattributed_misses},
        {"unsupported_instruction_kinds", &d.unsupported_instruction_kinds},
        {"unsupported_instruction_hits", &d.unsupported_instruction_hits},
    };
    for (const auto& [key, target] : counts) {
        const auto count = SharedCount(root.Get(key), e, key);
        if (!count) {
            return std::nullopt;
        }
        *target = *count;
    }
    const Value* truncated = root.Get("truncated");
    if (!truncated || truncated->type != Value::Type::Bool) {
        e = "truncated is not a boolean";
        return std::nullopt;
    }
    d.truncated = truncated->boolean;

    const auto bounded_list = [&e](const Value* list, const char* what) -> const Value* {
        if (!list || list->type != Value::Type::Array) {
            e = std::string{what} + " is not an array";
            return nullptr;
        }
        if (list->array.size() > kMaxModules) {
            e = std::string{what} + " has too many modules";
            return nullptr;
        }
        return list;
    };
    const auto build_id_of = [&e](const Value& item) -> std::optional<std::string> {
        const Value* id = item.Get("build_id");
        if (!id || id->type != Value::Type::String || !IsCanonicalBuildId(id->text)) {
            e = "bad build_id";
            return std::nullopt;
        }
        return id->text;
    };

    const Value* modules = bounded_list(root.Get("modules"), "modules");
    if (!modules) {
        return std::nullopt;
    }
    for (const Value& item : modules->array) {
        if (item.type != Value::Type::Object) {
            e = "module entry is not an object";
            return std::nullopt;
        }
        if (!KeysAre(item, {"build_id", "hits", "offsets"}, e, "a module")) {
            return std::nullopt;
        }
        const auto id = build_id_of(item);
        if (!id) {
            return std::nullopt;
        }
        if (d.modules.count(*id)) {
            e = "duplicate build_id";
            return std::nullopt;
        }
        const auto hits = SharedCount(item.Get("hits"), e, "hits");
        if (!hits) {
            return std::nullopt;
        }
        const Value* offsets = item.Get("offsets");
        if (!offsets || offsets->type != Value::Type::Array ||
            offsets->array.size() > kMaxOffsetsPerModule) {
            e = "offsets is not a bounded array";
            return std::nullopt;
        }
        SharedModule m;
        m.hits = *hits;
        for (const Value& pair : offsets->array) {
            std::uint64_t offset = 0;
            if (pair.type != Value::Type::Array || pair.array.size() != 2 ||
                pair.array[0].type != Value::Type::String || pair.array[0].text.size() > 8 ||
                !ParseHex(pair.array[0].text, offset) || !ValidSharedOffset(offset)) {
                e = "bad offset entry";
                return std::nullopt;
            }
            const auto count = SharedCount(&pair.array[1], e, "offset count");
            if (!count) {
                return std::nullopt;
            }
            if (!m.offsets.emplace(offset, *count).second) {
                e = "duplicate offset";
                return std::nullopt;
            }
        }
        d.modules.emplace(*id, std::move(m));
    }
    const Value* without = bounded_list(root.Get("modules_without_image"), "modules_without_image");
    if (!without) {
        return std::nullopt;
    }
    for (const Value& item : without->array) {
        if (item.type != Value::Type::Object) {
            e = "module entry is not an object";
            return std::nullopt;
        }
        if (!KeysAre(item, {"build_id", "hits"}, e, "a module without image")) {
            return std::nullopt;
        }
        const auto id = build_id_of(item);
        if (!id) {
            return std::nullopt;
        }
        if (d.modules_without_image.count(*id)) {
            e = "duplicate build_id";
            return std::nullopt;
        }
        const auto hits = SharedCount(item.Get("hits"), e, "hits");
        if (!hits) {
            return std::nullopt;
        }
        d.modules_without_image.emplace(*id, *hits);
    }
    return d;
}

std::optional<SharedCoverage> ParseImport(std::string_view json, std::string* error,
                                          bool* legacy) {
    std::string local;
    std::string& e = error ? *error : local;
    if (legacy) {
        *legacy = false;
    }
    Value root;
    if (json.size() > kMaxFileBytes) {
        e = "file is too large";
        return std::nullopt;
    }
    if (!Reader{json}.Document(root, e)) {
        return std::nullopt;
    }
    const Value* schema = root.type == Value::Type::Object ? root.Get("schema") : nullptr;
    if (schema && schema->type == Value::Type::String && schema->text == kSharedSchemaName) {
        return ParseShared(json, &e);
    }
    auto old = Parse(json, &e);
    if (!old) {
        return std::nullopt;
    }
    if (legacy) {
        *legacy = true;
    }
    // Module names and raw encodings stop here.
    return ToShared(*old);
}

void MergeShared(GapData& into, const SharedCoverage& from) {
    if (into.title_id.empty()) {
        into.title_id = from.title_id;
    }
    into.runs = SatAdd(into.runs, from.runs);
    into.hybrid_runs = SatAdd(into.hybrid_runs, from.hybrid_runs);
    into.strict_runs = SatAdd(into.strict_runs, from.strict_runs);
    into.clean_runs = SatAdd(into.clean_runs, from.clean_runs);
    into.unattributed_misses = SatAdd(into.unattributed_misses, from.unattributed_misses);
    into.truncated |= from.truncated;
    for (const auto& [id, m] : from.modules) {
        ModuleGaps* target = FindOrAddModule(into.modules, id, {}, into.truncated);
        if (!target) {
            continue;
        }
        target->hits = SatAdd(target->hits, m.hits);
        for (const auto& [offset, hits] : m.offsets) {
            AddOffset(*target, offset, hits, into.truncated);
        }
    }
    for (const auto& [id, hits] : from.modules_without_image) {
        if (ModuleGaps* target = FindOrAddModule(into.no_image, id, {}, into.truncated)) {
            target->hits = SatAdd(target->hits, hits);
        }
    }
    into.imported_unsupported_kinds =
        std::max(into.imported_unsupported_kinds, from.unsupported_instruction_kinds);
    into.imported_unsupported_hits =
        SatAdd(into.imported_unsupported_hits, from.unsupported_instruction_hits);
}

std::vector<std::uint64_t> RootsFor(const GapData& data, std::string_view build_id) {
    std::vector<std::uint64_t> roots;
    const std::string id = NormalizeBuildId(build_id);
    if (id.empty()) {
        return roots;
    }
    const auto it = data.modules.find(id);
    if (it == data.modules.end()) {
        return roots;
    }
    roots.reserve(it->second.offsets.size());
    for (const auto& [offset, hits] : it->second.offsets) {
        roots.push_back(offset);
    }
    return roots;
}

std::string Fingerprint(const GapData& data) {
    // FNV-1a 64: only has to notice a change, not resist one.
    std::uint64_t h = 0xcbf29ce484222325ull;
    const auto mix = [&h](std::string_view bytes) {
        for (const char c : bytes) {
            h ^= static_cast<unsigned char>(c);
            h *= 0x100000001b3ull;
        }
    };
    bool any = false;
    for (const auto& [id, m] : data.modules) {
        for (const auto& [offset, hits] : m.offsets) {
            mix(id);
            mix(":");
            mix(Hex(offset));
            mix(";");
            any = true;
        }
    }
    if (!any) {
        return {};
    }
    char buf[24];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

namespace {
std::optional<std::string> ReadText(const std::filesystem::path& path, std::string* error) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
        if (error) {
            *error = "cannot read file";
        }
        return std::nullopt;
    }
    if (size > kMaxFileBytes) {
        if (error) {
            *error = "file is too large";
        }
        return std::nullopt;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) {
            *error = "cannot open file";
        }
        return std::nullopt;
    }
    return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool WriteText(const std::filesystem::path& path, const std::string& text, std::string* error) {
    std::filesystem::path tmp = path;
    tmp += ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!out) {
            if (error) {
                *error = "cannot write file";
            }
            return false;
        }
    }
    std::error_code ec;
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        if (error) {
            *error = "cannot replace file";
        }
        return false;
    }
    return true;
}
} // namespace

std::optional<GapData> ReadFile(const std::filesystem::path& path, std::string* error) {
    const auto text = ReadText(path, error);
    return text ? Parse(*text, error) : std::nullopt;
}

bool WriteFile(const std::filesystem::path& path, const GapData& data, std::string* error) {
    return WriteText(path, Serialize(data), error);
}

std::optional<SharedCoverage> ReadImportFile(const std::filesystem::path& path, std::string* error,
                                             bool* legacy) {
    const auto text = ReadText(path, error);
    return text ? ParseImport(*text, error, legacy) : std::nullopt;
}

bool WriteSharedFile(const std::filesystem::path& path, const SharedCoverage& data,
                     std::string* error) {
    if (data.title_id.size() != 16) {
        if (error) {
            *error = "the coverage has no valid title ID";
        }
        return false;
    }
    return WriteText(path, SerializeShared(data), error);
}

// ---- SessionRecorder ----

void SessionRecorder::Begin(std::uint64_t title, bool strict_run) {
    std::scoped_lock lk{lock};
    title_id = title;
    strict = strict_run;
    dirty = true;
    loaded.clear();
    run = GapData{};
}

void SessionRecorder::NoteModule(std::uint64_t base, std::uint64_t size, std::string_view name,
                                 std::string build_id) {
    std::scoped_lock lk{lock};
    // A new module at an address replaces whatever was noted there before.
    for (auto it = loaded.begin(); it != loaded.end();) {
        const bool overlaps = it->first < base + size && base < it->first + it->second.size;
        it = overlaps ? loaded.erase(it) : std::next(it);
    }
    if (loaded.size() >= 256 || size == 0) {
        return;
    }
    loaded[base] = Module{size, SanitizeName(name), NormalizeBuildId(build_id), false};
}

void SessionRecorder::ForgetModule(std::uint64_t base) {
    std::scoped_lock lk{lock};
    loaded.erase(base);
}

void SessionRecorder::NoteImage(std::uint64_t base, std::string_view image_name) {
    std::scoped_lock lk{lock};
    const auto it = loaded.find(base);
    if (it != loaded.end()) {
        it->second.has_image = true;
        if (!image_name.empty()) {
            it->second.name = SanitizeName(image_name);
        }
    }
}

std::string SessionRecorder::BuildIdAt(std::uint64_t address) const {
    std::scoped_lock lk{lock};
    auto it = loaded.upper_bound(address);
    if (it == loaded.begin()) {
        return {};
    }
    --it;
    return address - it->first < it->second.size ? it->second.build_id : std::string{};
}

bool SessionRecorder::HasModule(std::string_view build_id, std::string_view name) const {
    std::scoped_lock lk{lock};
    const std::string wanted = NormalizeBuildId(build_id);
    const std::string wanted_name = SanitizeName(name);
    const auto lower = [](char c) {
        return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    };
    return std::any_of(loaded.begin(), loaded.end(), [&](const auto& entry) {
        const Module& m = entry.second;
        if (!build_id.empty()) {
            return !wanted.empty() && m.build_id == wanted;
        }
        return !wanted_name.empty() && m.name.size() == wanted_name.size() &&
               std::equal(m.name.begin(), m.name.end(), wanted_name.begin(),
                          [&](char a, char b) { return lower(a) == lower(b); });
    });
}

void SessionRecorder::RecordMiss(std::uint64_t pc) {
    std::scoped_lock lk{lock};
    dirty = true;
    auto it = loaded.upper_bound(pc);
    if (it == loaded.begin()) {
        run.unattributed_misses = SatAdd(run.unattributed_misses, 1);
        return;
    }
    --it;
    const Module& m = it->second;
    if (pc - it->first >= m.size || m.build_id.empty()) {
        run.unattributed_misses = SatAdd(run.unattributed_misses, 1);
        return;
    }
    ModuleGaps* target = FindOrAddModule(m.has_image ? run.modules : run.no_image, m.build_id,
                                         m.name, run.truncated);
    if (!target) {
        return;
    }
    target->hits = SatAdd(target->hits, 1);
    if (m.has_image) {
        AddOffset(*target, pc - it->first, 1, run.truncated);
    }
}

void SessionRecorder::RecordUnimplemented(std::uint32_t insn) {
    std::scoped_lock lk{lock};
    dirty = true;
    AddOpcode(run, insn, 1);
}

GapData SessionRecorder::Snapshot() const {
    std::scoped_lock lk{lock};
    GapData d = run;
    d.title_id = title_id ? TitleIdHex(title_id) : std::string{};
    d.runs = 1;
    d.hybrid_runs = strict ? 0 : 1;
    d.strict_runs = strict ? 1 : 0;
    d.last_run_strict = strict;
    d.clean_runs = run.Clean() ? 1 : 0;
    return d;
}

std::uint64_t SessionRecorder::TitleId() const {
    std::scoped_lock lk{lock};
    return title_id;
}

bool SessionRecorder::TakeDirty() {
    std::scoped_lock lk{lock};
    return std::exchange(dirty, false);
}

} // namespace Core::RecompGaps
