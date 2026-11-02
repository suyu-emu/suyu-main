// smoke_gaps_unit: recomp_gaps.json on its own (core/arm/recomp/recomp_gaps.cpp):
// schema, merge and dedupe, bounds, build-ID matching, path stripping, and the
// session recorder's classification of misses.
#include <cstdint>
#include <cstdio>
#include <string>

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

const std::string kIdA = "aa00000000000000000000000000000000000000000000000000000000000001";
const std::string kIdB = "bb00000000000000000000000000000000000000000000000000000000000002";

G::GapData OneRun(std::uint64_t offset, std::uint64_t hits) {
    G::GapData d;
    d.title_id = "0100000000001000";
    d.runs = d.hybrid_runs = 1;
    auto& m = d.modules[kIdA];
    m.name = "main";
    m.build_id = kIdA;
    m.hits = hits;
    m.offsets[offset] = hits;
    d.unimplemented[0xd503233f] = 3;
    return d;
}

int Schema() {
    G::GapData d = OneRun(0x1234, 2);
    d.no_image[kIdB] = G::ModuleGaps{"nro", kIdB, 7, {}};
    d.unattributed_misses = 4;
    d.strict_runs = 1;
    d.last_run_strict = true;
    const std::string text = G::Serialize(d);
    CHECK(text.find("\"schema\": \"suyu-recomp-gaps\"") != std::string::npos);
    CHECK(text.find("\"schema_version\": 1") != std::string::npos);
    const auto back = G::Parse(text);
    CHECK(back.has_value());
    CHECK(G::Serialize(*back) == text); // round trip is exact
    CHECK(back->modules.at(kIdA).offsets.at(0x1234) == 2);
    CHECK(back->no_image.at(kIdB).hits == 7 && back->no_image.at(kIdB).offsets.empty());
    CHECK(back->unimplemented.at(0xd503233f) == 3);
    CHECK(back->strict_runs == 1 && back->last_run_strict && back->unattributed_misses == 4);

    std::string error;
    CHECK(!G::Parse("{\"schema\": \"other\", \"schema_version\": 1}", &error));
    CHECK(!G::Parse("{\"schema\": \"suyu-recomp-gaps\"}", &error));
    CHECK(!G::Parse("{\"schema\": \"suyu-recomp-gaps\", \"schema_version\": 2}", &error));
    CHECK(error.find("newer") != std::string::npos);
    CHECK(!G::Parse("[1, 2]", &error));
    CHECK(!G::Parse("{\"schema\": \"suyu-recomp-gaps\", \"schema_version\": 1,", &error));
    CHECK(!G::Parse("{\"schema\": \"suyu-recomp-gaps\", \"schema_version\": 1, \"modules\": "
                    "[{\"build_id\": \"" + kIdA + "\", \"offsets\": [[\"xyz\", 1]]}]}",
                    &error));
    // Unknown keys are ignored; an entry with no usable build ID is dropped.
    const auto lenient = G::Parse(
        "{\"schema\": \"suyu-recomp-gaps\", \"schema_version\": 1, \"future\": {\"a\": [1]},"
        " \"modules\": [{\"name\": \"x\", \"build_id\": \"0000\", \"offsets\": [[\"10\", 1]]}]}");
    CHECK(lenient && lenient->modules.empty());
    std::printf("PASS gaps schema\n");
    return 0;
}

int MergeDedupe() {
    G::GapData total;
    G::Merge(total, OneRun(0x1000, 1));
    G::Merge(total, OneRun(0x1000, 2)); // same address: counted, not repeated
    G::Merge(total, OneRun(0x2000, 1));
    CHECK(total.runs == 3 && total.hybrid_runs == 3);
    const auto& m = total.modules.at(kIdA);
    CHECK(m.offsets.size() == 2 && m.offsets.at(0x1000) == 3 && m.hits == 4);
    CHECK(total.unimplemented.size() == 1 && total.unimplemented.at(0xd503233f) == 9);
    CHECK(total.title_id == "0100000000001000");
    CHECK(!total.Clean());

    // A clean run adds a run and nothing else.
    G::GapData clean;
    clean.runs = clean.hybrid_runs = clean.clean_runs = 1;
    CHECK(clean.Clean());
    const std::string before = G::Fingerprint(total);
    G::Merge(total, clean);
    CHECK(total.runs == 4 && total.clean_runs == 1);
    // More runs or hits never change the fingerprint; a new address does.
    G::Merge(total, OneRun(0x1000, 5));
    CHECK(G::Fingerprint(total) == before && !before.empty());
    G::Merge(total, OneRun(0x3000, 1));
    CHECK(G::Fingerprint(total) != before);
    CHECK(G::Fingerprint(G::GapData{}).empty());

    // Bounds: offsets per module stop growing and the file says so.
    G::GapData big;
    for (std::uint64_t i = 0; i < G::kMaxOffsetsPerModule + 10; ++i) {
        G::Merge(big, OneRun(i * 4, 1));
    }
    CHECK(big.modules.at(kIdA).offsets.size() == G::kMaxOffsetsPerModule && big.truncated);
    CHECK(G::Parse(G::Serialize(big))->truncated);
    std::printf("PASS gaps merge/dedupe\n");
    return 0;
}

int BuildIds() {
    std::uint8_t raw[32] = {0xAA};
    raw[31] = 0x01;
    CHECK(G::BuildIdHex(raw, sizeof raw) == kIdA);
    // Case and zero padding do not matter; anything else does.
    CHECK(G::BuildIdMatches(kIdA, "AA00000000000000000000000000000000000000000000000000000000000001"));
    CHECK(G::BuildIdMatches("5eed", "5eed000000000000000000000000000000000000000000000000000000000000"));
    CHECK(!G::BuildIdMatches(kIdA, kIdB));
    CHECK(!G::BuildIdMatches(kIdA, kIdA.substr(0, 62) + "02"));
    CHECK(!G::BuildIdMatches("", ""));
    CHECK(!G::BuildIdMatches(std::string(64, '0'), std::string(64, '0'))); // all zero: no identity
    CHECK(G::NormalizeBuildId("zz").empty() && G::NormalizeBuildId(std::string(65, 'a')).empty());

    G::GapData d = OneRun(0x40, 1);
    CHECK(G::RootsFor(d, kIdA).size() == 1 && G::RootsFor(d, kIdA)[0] == 0x40);
    CHECK(G::RootsFor(d, "AA00000000000000000000000000000000000000000000000000000000000001").size() == 1);
    CHECK(G::RootsFor(d, kIdB).empty());
    CHECK(G::RootsFor(d, "").empty());
    std::printf("PASS gaps build IDs\n");
    return 0;
}

int NoPaths() {
    CHECK(G::SanitizeName("C:\\Users\\someone\\Games\\main") == "main");
    CHECK(G::SanitizeName("/home/someone/nro/app.nro") == "app.nro");
    CHECK(G::SanitizeName("a\"b<c>") == "abc");
    G::GapData d = OneRun(0x10, 1);
    d.modules.at(kIdA).name = "D:\\private\\folder\\main";
    const std::string text = G::Serialize(d);
    CHECK(text.find("private") == std::string::npos && text.find('\\') == std::string::npos);
    // A path smuggled into a file does not survive a read and write either.
    const auto back = G::Parse(
        "{\"schema\": \"suyu-recomp-gaps\", \"schema_version\": 1, \"title_id\": \"0100\", "
        "\"modules\": [{\"name\": \"C:\\\\Users\\\\me\\\\main\", \"build_id\": \"" +
        kIdA + "\", \"offsets\": []}]}");
    CHECK(back && back->modules.at(kIdA).name == "main");
    CHECK(G::Serialize(*back).find("Users") == std::string::npos);
    std::printf("PASS gaps carry no paths\n");
    return 0;
}

int Recorder() {
    G::SessionRecorder rec;
    rec.Begin(0x0100000000001000, false);
    rec.NoteModule(0x8000000, 0x100000, "main", kIdA);
    rec.NoteModule(0x9000000, 0x10000, "nro", kIdB);
    rec.NoteImage(0x8000000, "main");
    rec.RecordMiss(0x8000040);  // image module: an offset
    rec.RecordMiss(0x8000040);
    rec.RecordMiss(0x9000100);  // no image: the module, no offsets
    rec.RecordMiss(0x8100000);  // one past main's end: nobody's
    rec.RecordMiss(0x1000);     // below every module
    rec.RecordUnimplemented(0x12345678);
    G::GapData run = rec.Snapshot();
    CHECK(run.title_id == "0100000000001000" && run.runs == 1 && run.hybrid_runs == 1);
    CHECK(run.modules.at(kIdA).offsets.size() == 1 && run.modules.at(kIdA).offsets.at(0x40) == 2);
    CHECK(run.no_image.at(kIdB).hits == 1 && run.no_image.at(kIdB).offsets.empty());
    CHECK(run.unattributed_misses == 2 && run.unimplemented.at(0x12345678) == 1);
    CHECK(run.clean_runs == 0);
    // An unloaded module's later misses are no longer its.
    rec.ForgetModule(0x9000000);
    rec.RecordMiss(0x9000100);
    CHECK(rec.Snapshot().unattributed_misses == 3);
    // A module loaded over another replaces it.
    rec.NoteModule(0x8000000, 0x1000, "main2", kIdB);
    rec.RecordMiss(0x8000080);
    CHECK(rec.Snapshot().no_image.at(kIdB).hits == 2);

    // A strict run with nothing to report is a clean strict run.
    rec.Begin(0x0100000000001000, true);
    CHECK(rec.TakeDirty() && !rec.TakeDirty());
    run = rec.Snapshot();
    CHECK(run.strict_runs == 1 && run.hybrid_runs == 0 && run.clean_runs == 1 &&
          run.last_run_strict && run.Clean());
    std::printf("PASS gaps recorder\n");
    return 0;
}

// The shared form: execution metadata only, strictly read, and never a way back
// in for raw encodings, names or paths.
int Shared() {
    G::GapData local = OneRun(0x1234, 2);
    local.modules[kIdA].name = "C:/Users/someone/game/main";
    local.modules[kIdA].offsets[0x1235] = 1;          // not instruction-aligned: dropped
    local.modules[kIdA].offsets[0x1'0000'0000] = 1;   // past 4 GiB: dropped
    local.no_image[kIdB] = G::ModuleGaps{"nro", kIdB, 7, {}};
    local.unimplemented[0x12345678] = 4;
    const G::SharedCoverage shared = G::ToShared(local);
    const std::string text = G::SerializeShared(shared);
    // No raw words (d503233f / 12345678), no names, no paths.
    CHECK(text.find("d503233f") == std::string::npos && text.find("12345678") == std::string::npos);
    CHECK(text.find("main") == std::string::npos && text.find("Users") == std::string::npos &&
          text.find("nro") == std::string::npos);
    CHECK(text.find("no raw instruction bytes included") != std::string::npos);
    // Aggregates survive, so the UI does not report zero unsupported instructions.
    CHECK(shared.unsupported_instruction_kinds == 2 && shared.unsupported_instruction_hits == 7);
    CHECK(shared.modules.at(kIdA).offsets.size() == 1 && shared.modules.at(kIdA).offsets.at(0x1234) == 2);
    CHECK(shared.modules_without_image.at(kIdB) == 7);

    // Deterministic and round-trips exactly.
    auto back = G::ParseShared(text);
    CHECK(back && G::SerializeShared(*back) == text);

    // Strict reader: unknown fields, free text, bad values and raw opcode lists fail.
    const auto rejects = [&](std::string from, std::string to) {
        std::string doc = text;
        const auto at = doc.find(from);
        if (at == std::string::npos) {
            return false;
        }
        doc.replace(at, from.size(), to);
        std::string error;
        return !G::ParseShared(doc, &error) && !error.empty();
    };
    CHECK(rejects("\"runs\": 1", "\"runs\": 1, \"comment\": \"hello\""));
    CHECK(rejects("\"runs\": 1", "\"runs\": 1, \"unimplemented_opcodes\": [[\"d503233f\", 3]]"));
    CHECK(rejects("\"runs\": 1", "\"runs\": 1, \"runs\": 2"));
    CHECK(rejects("\"hits\": 2,", "\"hits\": 2, \"name\": \"main\","));
    CHECK(rejects("\"description\": \"Execution metadata; no raw instruction bytes included.\"",
                  "\"description\": \"C:/Users/someone\""));
    CHECK(rejects("[\"1234\", 2]", "[\"1235\", 2]"));
    CHECK(rejects("[\"1234\", 2]", "[\"100000000\", 2]"));
    CHECK(rejects("[\"1234\", 2]", "[\"1234\", -2]"));
    CHECK(rejects("[\"1234\", 2]", "[\"1234\", 9007199254740992]"));
    CHECK(rejects("\"title_id\": \"0100000000001000\"", "\"title_id\": \"\""));
    CHECK(rejects(kIdA, "AA00000000000000000000000000000000000000000000000000000000000001"));
    CHECK(rejects("\"schema_version\": 1", "\"schema_version\": 2"));
    CHECK(rejects("\"truncated\": false", "\"truncated\": 0"));

    // A legacy recomp_gaps file is accepted for import but converted: its opcodes and
    // names do not come through, and exporting afterwards stays clean.
    bool legacy = false;
    auto imported = G::ParseImport(G::Serialize(local), nullptr, &legacy);
    CHECK(imported && legacy);
    CHECK(G::SerializeShared(*imported).find("12345678") == std::string::npos);
    G::GapData store;
    store.title_id = "0100000000001000";
    store.unimplemented[0xd503201f] = 1; // this machine's own diagnostics
    G::MergeShared(store, *imported);
    // Local diagnostics keep their own opcode, gain no foreign ones, and know at least
    // as many unsupported kinds as the file reported.
    CHECK(store.unimplemented.size() == 1 && store.unimplemented.count(0xd503201f));
    CHECK(store.UnsupportedInstructionKinds() == 2 && !store.Clean());
    // The recompiler still gets usable, build-ID-matched roots.
    const auto roots = G::RootsFor(store, kIdA);
    CHECK(roots.size() == 1 && roots[0] == 0x1234);
    // The merged store round-trips through its own file without losing the totals.
    auto reread = G::Parse(G::Serialize(store));
    CHECK(reread && reread->imported_unsupported_kinds == 2 &&
          reread->imported_unsupported_hits == 7 && reread->unimplemented.size() == 1);
    const std::string again = G::SerializeShared(G::ToShared(*reread));
    CHECK(again.find("d503201f") == std::string::npos && again.find("12345678") == std::string::npos);
    // Shared input is parsed by the shared rules.
    auto shared_in = G::ParseImport(text, nullptr, &legacy);
    CHECK(shared_in && !legacy);
    CHECK(!G::ParseImport("{\"schema\": \"suyu-shared-coverage\", \"extra\": 1}"));
    std::printf("PASS shared coverage\n");
    return 0;
}

} // namespace

int main() {
    return Schema() || MergeDedupe() || BuildIds() || NoPaths() || Recorder() || Shared();
}
