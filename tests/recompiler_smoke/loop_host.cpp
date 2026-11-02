// smoke_loop: the coverage loop end to end on a synthetic module (loop_code.h),
// with the emulator's own recorder (core/arm/recomp/recomp_gaps.cpp) in the
// role ArmRecomp gives it.
//
//   record <gaps.json>  first export: the indirect call misses, as in a Hybrid
//                       run; the miss is recorded and merged into the file.
//   static <gaps.json>  export seeded from that file: the callee now has a
//                       block and the same call stays in recompiled code.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/arm/recomp/recomp_gaps.h"
#include "loop_code.h"

extern "C" {
void loopc_init(void);
int loopc_has_block(std::uint64_t pc);
int loopc_run(std::uint64_t* pc, std::uint64_t* x30);
std::uint32_t loopc_word(std::uint64_t pc);
}

namespace G = Core::RecompGaps;

#define CHECK(x)                                                                                 \
    do {                                                                                         \
        if (!(x)) {                                                                              \
            std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                            \
            return 1;                                                                            \
        }                                                                                        \
    } while (0)

namespace {

constexpr int kHaltUnhandled = 2; // RECOMP_HALT_UNHANDLED

// What ArmRecomp does with a run's outcome: a PC with no block is a lookup
// miss, a parked unhandled instruction is an unimplemented opcode.
void Dispatch(G::SessionRecorder& rec, int halted, std::uint64_t pc) {
    if (halted == kHaltUnhandled) {
        rec.RecordUnimplemented(loopc_word(pc));
    } else if (!halted && !loopc_has_block(pc)) {
        rec.RecordMiss(pc);
    }
}

bool MergeInto(const char* path, const G::GapData& run) {
    G::GapData merged;
    if (auto existing = G::ReadFile(path)) {
        merged = *existing;
    }
    G::Merge(merged, run);
    return G::WriteFile(path, merged);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        return 2;
    }
    const std::string mode = argv[1];
    const char* gaps_path = argv[2];
    loopc_init();

    G::SessionRecorder rec;
    rec.Begin(LOOP_TITLE_ID, false);
    // The loader's report, then the frontend's: this module runs from an image.
    rec.NoteModule(0, 0x2000, "loop", LOOP_BUILD_ID);
    rec.NoteImage(0, "loop");

    std::uint64_t pc = 0, x30 = 0;
    if (mode == "record") {
        CHECK(loopc_has_block(LOOP_BASE));
        CHECK(!loopc_has_block(LOOP_CALLEE));
        for (int i = 0; i < 2; ++i) {
            const int halted = loopc_run(&pc, &x30);
            CHECK(halted == 0 && pc == LOOP_CALLEE && x30 == LOOP_BASE + 8);
            Dispatch(rec, halted, pc);
        }
        const G::GapData run = rec.Snapshot();
        // Two misses at one address: deduplicated, counted.
        CHECK(run.modules.size() == 1 && run.no_image.empty() && run.unattributed_misses == 0);
        const auto& m = run.modules.begin()->second;
        CHECK(m.build_id == LOOP_BUILD_ID && m.name == "loop" && m.hits == 2);
        CHECK(m.offsets.size() == 1 && m.offsets.at(LOOP_CALLEE) == 2);
        CHECK(run.runs == 1 && run.hybrid_runs == 1 && run.clean_runs == 0);
        CHECK(MergeInto(gaps_path, run));
        std::printf("PASS loop record: miss at %#llx recorded\n",
                    static_cast<unsigned long long>(pc));
        return 0;
    }
    if (mode == "static") {
        // The re-export was seeded at the recorded offset.
        CHECK(loopc_has_block(LOOP_CALLEE));
        const int halted = loopc_run(&pc, &x30);
        // The call now enters the callee's block without leaving the image:
        // what stops it is the callee's own first instruction (udf #0), which
        // the block reports as an unimplemented opcode at its PC.
        CHECK(halted == kHaltUnhandled && pc == LOOP_CALLEE && x30 == LOOP_BASE + 8);
        Dispatch(rec, halted, pc);
        const G::GapData run = rec.Snapshot();
        CHECK(run.Misses() == 0 && run.modules.empty());
        CHECK(run.unimplemented.size() == 1 && run.unimplemented.at(0) == 1);
        CHECK(MergeInto(gaps_path, run));
        const auto pooled = G::ReadFile(gaps_path);
        CHECK(pooled && pooled->runs == 2 && pooled->hybrid_runs == 2);
        CHECK(pooled->title_id == G::TitleIdHex(LOOP_TITLE_ID));
        CHECK(G::RootsFor(*pooled, LOOP_BUILD_ID).size() == 1);
        CHECK(pooled->unimplemented.at(0) == 1);
        std::printf("PASS loop static: %#llx has a block and runs in the image\n",
                    static_cast<unsigned long long>(pc));
        return 0;
    }
    return 2;
}
