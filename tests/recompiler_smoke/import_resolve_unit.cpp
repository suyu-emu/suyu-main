// smoke_import_resolve_unit: finding each loaded module's MOD0 and exports
// (Core::RecompGaps::FindMod0 / IndexModuleExports), which the host
// pre-relocator resolves every module's imports through.
//
// Regression: the MOD0 search only scanned a module's first 0x4000 bytes. TOTK
// 1.4.3's main, subsdk0 and sdk keep MOD0 megabytes into rodata (the word at
// base+4 points there), so none of them was indexed. rtld imports
// nn::init::Start and nn::os::detail::UserExceptionHandler as weak symbols
// that sdk defines; with sdk missing from the index both slots were left 0
// and rtld's first call through the PLT went to PC 0. Synthetic modules laid
// out like the real ones; no game data.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "core/arm/recomp/recomp_gaps.h"

namespace G = Core::RecompGaps;

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

namespace {

/// Sparse guest memory: unmapped bytes read 0, as the guest memory reader does.
struct Memory {
    std::map<std::uint64_t, std::uint8_t> bytes;
    void Put(std::uint64_t va, std::uint64_t v, int n) {
        for (int i = 0; i < n; ++i) {
            bytes[va + i] = static_cast<std::uint8_t>(v >> (8 * i));
        }
    }
    void PutString(std::uint64_t va, const std::string& s) {
        for (std::size_t i = 0; i <= s.size(); ++i) {
            bytes[va + i] = i < s.size() ? static_cast<std::uint8_t>(s[i]) : 0;
        }
    }
    G::GuestRead8 Reader() const {
        return [this](std::uint64_t va) -> std::uint8_t {
            const auto it = bytes.find(va);
            return it == bytes.end() ? 0 : it->second;
        };
    }
};

struct Sym {
    std::string name;
    bool weak;
    bool defined;
    std::uint64_t value;
};

struct Module {
    std::uint64_t base, symtab, strtab;
};

constexpr std::uint64_t DT_STRTAB = 5, DT_SYMTAB = 6;

/// A module at `base` with MOD0 at `base + mod0_off` (named by the word at
/// +4), a .dynamic of DT_SYMTAB/DT_STRTAB, and .dynsym immediately followed by
/// .dynstr, as Switch modules have them.
Module MakeModule(Memory& m, std::uint64_t base, std::uint32_t mod0_off,
                  const std::vector<Sym>& syms) {
    m.Put(base, 0x14000002u, 4); // b over the header word
    m.Put(base + 4, mod0_off, 4);
    const std::uint64_t mod0 = base + mod0_off;
    m.PutString(mod0, "MOD0");
    const std::uint64_t dynamic = mod0 + 0x40;
    m.Put(mod0 + 4, dynamic - mod0, 4);
    const std::uint64_t symtab = dynamic + 0x40;
    const std::uint64_t strtab = symtab + 24 * (syms.size() + 1);
    m.Put(dynamic, DT_SYMTAB, 8);
    m.Put(dynamic + 8, symtab - base, 8);
    m.Put(dynamic + 16, DT_STRTAB, 8);
    m.Put(dynamic + 24, strtab - base, 8);
    m.Put(dynamic + 32, 0, 16); // DT_NULL
    std::uint64_t name_off = 1;
    m.Put(strtab, 0, 1);
    for (std::size_t i = 0; i < syms.size(); ++i) {
        const std::uint64_t e = symtab + 24 * (i + 1);
        m.Put(e, name_off, 4);
        m.Put(e + 4, (syms[i].weak ? 2u : 1u) << 4 | 2u, 1); // STT_FUNC
        m.Put(e + 5, 0, 1);
        m.Put(e + 6, syms[i].defined ? 2 : 0, 2);
        m.Put(e + 8, syms[i].value, 8);
        m.Put(e + 16, 0, 8);
        m.PutString(strtab + name_off, syms[i].name);
        name_off += syms[i].name.size() + 1;
    }
    return {base, symtab, strtab};
}

/// The table the pre-relocator reads .dynamic for: DT_SYMTAB/DT_STRTAB.
bool Tables(const Memory& m, std::uint64_t base, Module& out) {
    const auto read8 = m.Reader();
    const std::uint64_t mod0 = G::FindMod0(base, read8);
    if (!mod0) {
        return false;
    }
    const auto le = [&](std::uint64_t va, int n) {
        std::uint64_t v = 0;
        for (int i = 0; i < n; ++i) {
            v |= std::uint64_t{read8(va + i)} << (8 * i);
        }
        return v;
    };
    out = {base, 0, 0};
    for (std::uint64_t p = mod0 + le(mod0 + 4, 4);; p += 16) {
        const auto tag = le(p, 8);
        if (tag == 0) {
            break;
        }
        if (tag == DT_SYMTAB) out.symtab = base + le(p + 8, 8);
        if (tag == DT_STRTAB) out.strtab = base + le(p + 8, 8);
    }
    return true;
}

constexpr std::uint64_t kRtld = 0x80aad000, kMain = 0x80ab3000, kSdk = 0x84bfe000;
constexpr std::uint64_t kTrap = 0xdead0000;
const char* const kInitStart = "_ZN2nn4init5StartEmmPFvvES2_S2_";
const char* const kUserHandler = "_ZN2nn2os6detail20UserExceptionHandlerEv";
const char* const kNowhere = "_ZN2nn4diag6detail9NoSuchHookEv";

/// rtld with MOD0 near its start; main and sdk with MOD0 far past 0x4000.
void Load(Memory& m) {
    MakeModule(m, kRtld, 0x301c,
               {{kInitStart, true, false, 0}, {kUserHandler, true, false, 0},
                {kNowhere, true, false, 0}, {"nnrtld_local", false, true, 0x100}});
    MakeModule(m, kMain, 0x2bba060, {{"nnMain", false, true, 0x1234}});
    MakeModule(m, kSdk, 0x66401c,
               {{kInitStart, false, true, 0x110ee0}, {kUserHandler, false, true, 0x14a6c8}});
}

int Mod0() {
    Memory m;
    Load(m);
    const auto read8 = m.Reader();
    CHECK(G::FindMod0(kRtld, read8) == kRtld + 0x301c);
    CHECK(G::FindMod0(kMain, read8) == kMain + 0x2bba060);
    CHECK(G::FindMod0(kSdk, read8) == kSdk + 0x66401c);
    // A module whose word at +4 is wrong but has MOD0 early is still found;
    // one without MOD0 anywhere is not.
    Memory scan;
    scan.Put(0x1004, 0x99999999u, 4);
    scan.PutString(0x1000 + 0x2018, "MOD0");
    CHECK(G::FindMod0(0x1000, scan.Reader()) == 0x1000 + 0x2018);
    CHECK(G::FindMod0(0x9000000, scan.Reader()) == 0);
    std::printf("PASS MOD0 found through the header word, past the first pages\n");
    return 0;
}

int Resolve() {
    Memory m;
    Load(m);
    const auto read8 = m.Reader();
    std::unordered_map<std::string, std::uint64_t> exports;
    for (const auto base : {kRtld, kMain, kSdk}) {
        Module mod{};
        CHECK(Tables(m, base, mod));
        G::IndexModuleExports(mod.base, mod.symtab, mod.strtab, read8, exports);
    }
    CHECK(exports.size() == 4);
    CHECK(exports.at("nnrtld_local") == kRtld + 0x100);
    CHECK(exports.at("nnMain") == kMain + 0x1234);
    // rtld's imports are undefined there and not exported by it.
    Module rtld{};
    CHECK(Tables(m, kRtld, rtld));
    const auto start = G::ReadDynSymbol(rtld.symtab, rtld.strtab, 1, read8);
    CHECK(start.name == kInitStart && start.weak && !start.defined && start.value == 0);
    // Weak and defined in sdk: resolves to sdk's definition.
    CHECK(exports.count(kInitStart) && exports.at(kInitStart) == kSdk + 0x110ee0);
    CHECK(exports.count(kUserHandler) && exports.at(kUserHandler) == kSdk + 0x14a6c8);
    // Weak and defined nowhere: not exported, so the slot stays 0.
    const auto nowhere = G::ReadDynSymbol(rtld.symtab, rtld.strtab, 3, read8);
    CHECK(nowhere.name == kNowhere && nowhere.weak && !exports.count(kNowhere));
    CHECK(G::UndefinedImportValue(nowhere.weak, kTrap) == 0);
    CHECK(G::UndefinedImportValue(false, kTrap) == kTrap);
    std::printf("PASS weak imports resolve to another module's definition, else 0\n");
    return 0;
}

} // namespace

int main() {
    if (const int r = Mod0()) return r;
    if (const int r = Resolve()) return r;
    return 0;
}
