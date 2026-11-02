#!/usr/bin/env python3
"""Focused source-based tests. --package runs retrieved excerpts/new headers only.
Without --package, run from research/tests in an applied full checkout.
"""
from __future__ import annotations
import argparse
import importlib.util
import os
from pathlib import Path
import re
import shlex
import subprocess
import sys
import tempfile
import unittest

parser = argparse.ArgumentParser()
parser.add_argument('--package', type=Path)
parser.add_argument('--sanitize', choices=['thread', 'address,undefined'])
args, rest = parser.parse_known_args()
ROOT = Path(__file__).resolve().parents[2]
PKG = args.package.resolve() if args.package else None
if PKG:
    spec = importlib.util.spec_from_file_location('fixes', PKG/'apply_fixes.py')
    fixes = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(fixes)
    SOURCE = PKG/'payload'
else:
    SOURCE = ROOT
CXX = shlex.split(os.environ.get('CXX', 'c++'))
CC = shlex.split(os.environ.get('CC', 'cc'))
SAN = ['-fsanitize='+args.sanitize, '-fno-omit-frame-pointer'] if args.sanitize else []
# Low fixed-address non-PIE executables avoid a common Linux TSan address-space collision.
if args.sanitize == 'thread' and os.name == 'posix': SAN += ['-fno-pie', '-no-pie']


def run(cmd, **kw):
    p = subprocess.run([str(x) for x in cmd], text=True, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, timeout=45, **kw)
    if p.returncode:
        raise AssertionError(f'Command failed ({p.returncode}): {cmd}\n{p.stdout}\n{p.stderr}')
    return p.stdout


def section(text, start, end):
    return text[text.index(start):text.index(end, text.index(start))]


class ReviewTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix='suyu-review-')
        self.d = Path(self.tmp.name)
    def tearDown(self): self.tmp.cleanup()
    def build(self, text, name='test', c=False, sanitize=True, extra_flags=()):
        src=self.d/(name+('.c' if c else '.cpp'))
        src.write_text(text)
        exe=self.d/(name+('.exe' if os.name=='nt' else ''))
        run((CC if c else CXX)+['-std=c11' if c else '-std=c++20', '-O1', '-g',
                               '-I'+str(self.d), '-I'+str(SOURCE/'src'), str(src),
                               '-o', str(exe)] + list(extra_flags) + (SAN if sanitize else []) + ['-pthread'])
        return exe

    def test_01_per_entry_guard_concurrency_and_revalidation(self):
        if PKG: emission = fixes.GUARD_EMISSION
        else:
            header=(ROOT/'src/core/recompiler/arm64_to_c.h').read_text()
            emission=section(header, '        // Verify each entry,',
                             '        // Lookup indexes every emitted instruction')
        gen='''#include <iostream>
#include <string>
int main(){struct { unsigned long long vaddr=0; unsigned count=2;} b;
std::string rcu="void block(GuestContext* c){static const uint32_t _expected[]={0x11223344U,0x55667788U";
'''+emission+'std::cout<<rcu<<"}\\n";}\n'
        generated=run([self.build(gen, 'guard_generator', sanitize=False)])
        self.assertNotIn('_guard_seen', generated)
        self.assertNotIn('guard_generation', generated)
        pre='''#include <atomic>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <thread>
#include <vector>
struct GuestContext { const uint32_t* words; };
static uint64_t g_module_base=0;
static int g_recomp_guard_host_v2=2;
static std::atomic<unsigned> checks{0};
void recomp_code_guard(GuestContext*c,uint64_t,const uint32_t* expected,unsigned n,int version){
 assert(version==2); ++checks;
 for(unsigned i=0;i<n;++i) if(c->words[i]!=expected[i]) std::abort();
}
'''
        main='''int main(int argc,char**){
 const uint32_t original[]={0x11223344U,0x55667788U};
 const uint32_t changed[]={0x11223344U,0xdeadbeefU};
 GuestContext first{original}; block(&first);
 if(argc>1){ GuestContext reused_address{changed}; block(&reused_address); return 7; }
 std::vector<std::thread> workers;
 for(int t=0;t<4;++t) workers.emplace_back([&]{GuestContext c{original};for(int i=0;i<5000;++i)block(&c);});
 for(auto&worker:workers)worker.join();
 assert(checks==20001);
}
'''
        exe=self.build(pre+generated+main,'guard')
        run([exe])
        if not args.sanitize:
            p=subprocess.run([str(exe),'mutated'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            self.assertNotEqual(p.returncode,0)
            self.assertNotEqual(p.returncode,7) # guard must stop BEFORE guest execution

    def test_02_fp_value_and_fpsr(self):
        if PKG:
            native=fixes.NATIVE_HELPER
            division=(PKG/'reference/fp_divide.h').read_text()
        else:
            header=(ROOT/'src/core/recompiler/arm64_to_c.h').read_text()
            native=section(header,'inline std::string EmitFPNativeValue(',
                           'inline std::string EmitFPAddSubValue(')
            division=section(header,'inline std::string EmitFPDivideValue(',
                             '// Architectural reciprocal estimate.')
        gen='#include <iostream>\n#include <string>\n'+native+division+'''
int main(){
 for(bool d:{false,true}){
 if(!EmitFPNativeValue(d,"div").empty())return 1;
 std::cout<<"uint64_t divide"<<(d?64:32)<<"(Context*c,uint64_t _a,uint64_t _b){uint64_t _v=0;";
 std::cout<<EmitFPDivideValue(d)<<"return _v;}\\n";
 }
}
'''
        code=run([self.build(gen,'fp_generator',sanitize=False)])
        harness='''#include <stdint.h>
#include <assert.h>
typedef struct { uint64_t fpcr,fpsr; } Context;
'''+code+'''
#define CHECK(F,A,B,R,S,CR) do { Context c={CR,0}; assert(F(&c,A,B)==(uint64_t)(R)); assert(c.fpsr==(uint64_t)(S)); } while(0)
int main(void){
 CHECK(divide32,0x3f800000U,0,0x7f800000U,2,0);
 CHECK(divide32,0,0,0x7fc00000U,1,0);
 CHECK(divide32,0x7f800001U,0x3f800000U,0x7fc00001U,1,0);
 CHECK(divide32,0x7fc00001U,0x3f800000U,0x7fc00001U,0,0);
 CHECK(divide32,0x7f7fffffU,0x3f000000U,0x7f800000U,20,0);
 CHECK(divide32,1,0x40000000U,0,24,0);
 CHECK(divide32,1,0x3f800000U,0,128,1ULL<<24);
 CHECK(divide32,0x3f800000U,0x40400000U,0x3eaaaaabU,16,0);
 CHECK(divide32,0x3f800000U,0x40400000U,0x3eaaaaaaU,16,2ULL<<22);
 CHECK(divide32,0x40000000U,0x3f800000U,0x40000000U,0,0);
 CHECK(divide64,0x3ff0000000000000ULL,0,0x7ff0000000000000ULL,2,0);
 CHECK(divide64,0,0,0x7ff8000000000000ULL,1,0);
 CHECK(divide64,0x7ff0000000000001ULL,0x3ff0000000000000ULL,0x7ff8000000000001ULL,1,0);
 CHECK(divide64,0x7fefffffffffffffULL,0x3fe0000000000000ULL,0x7ff0000000000000ULL,20,0);
 CHECK(divide64,1,0x4000000000000000ULL,0,24,0);
 CHECK(divide64,0x3ff0000000000000ULL,0x4008000000000000ULL,0x3fd5555555555555ULL,16,0);
 CHECK(divide64,0x3ff0000000000000ULL,0x4008000000000000ULL,0x3fd5555555555556ULL,16,1ULL<<22);
 Context c={0,128}; (void)divide64(&c,0x3ff0000000000000ULL,0); assert(c.fpsr==130);
 return 0;
}
'''
        run([self.build(harness,'fp',c=True)])

    def test_03_diagnostic_lifetime_restart_and_disabled_path(self):
        cpp=r'''#include "core/arm/recomp/recomp_diagnostic_sampler.h"
#include <cassert>
#include <map>
#include <thread>
#include <mutex>
#include <atomic>
int main(){
 using D=Core::RecompDiagnosticSampler;
 const auto t=D::Clock::time_point{};
 std::map<uint64_t,std::string> modules{{0x1000,"main"}};
 std::atomic<int> reads{0}, publications{0};
 auto read32=[&](uint64_t){++reads;return uint32_t{1};};
 auto read64=[&](uint64_t){++reads;return uint64_t{2};};
 auto mapped=[](uint64_t){return true;};
 auto pub=[&](const std::string& s){assert(s.find("core=")!=std::string::npos);++publications;};
 auto emit=[](const std::string&){};
 {D off;off.Sample(t,1,0,0,0,modules,read32,read64,mapped,pub,emit);assert(reads==0);}
 for(int session=0;session<20;++session){
  D d;D::Config c;c.watch=true;c.snapshots=true;c.lr_offset=0x40;
  d.Configure(session,0,c);d.TrackSlot(0x2000,1,"symbol");d.TrackSlot(0x2000,1,"symbol");
  const int before=reads;
  d.Sample(t,1,0x1000,0x1040,0x3000,modules,read32,read64,mapped,pub,emit);
  assert(reads==before+17); // one slot, sixteen words; duplicate slot suppressed
  d.Sample(t,1,0x1000,0x1040,0x3000,modules,read32,read64,mapped,pub,emit);
  assert(reads==before+17); // bounded cadence
  auto unmapped=[](uint64_t){return false;};
  d.Sample(t+std::chrono::seconds(1),1,0x1000,0x1040,0x3000,modules,read32,read64,unmapped,pub,emit);
  assert(reads==before+17);
 }
 // Concurrent independent CPU owners. Shared observations are atomics; no
 // poller survives each owner's stack scope or retains any reader callback.
 std::vector<std::thread> workers;
 for(int core=0;core<4;++core)workers.emplace_back([&,core]{
  for(int session=0;session<100;++session){D d;D::Config c;c.snapshots=true;d.Configure(session,core,c);
   d.Sample(t,1,0x1000,0,0,modules,read32,read64,mapped,pub,emit);}
 });
 for(auto& w:workers)w.join();
 assert(publications==440);
 const int done=reads;std::this_thread::sleep_for(std::chrono::milliseconds(10));assert(reads==done);
}
'''
        run([self.build(cpp,'diagnostics')])

    def test_04_probe_on_off_and_pipeline_counts(self):
        # Minimal logging/types headers, not a Vulkan/GPU substitute.
        (self.d/'common/logging').mkdir(parents=True)
        (self.d/'common/common_types.h').write_text('#include <cstdint>\nusing u64=std::uint64_t;\n')
        (self.d/'common/logging/log.h').write_text('template<class...T>void test_log(T&&...){}\n#define LOG_INFO(C,...) test_log(__VA_ARGS__)\n')
        cpp=r'''#include "video_core/renderer_vulkan/vk_stall_probe.h"
#include <cassert>
#include <thread>
#include <vector>
int main(){using namespace Vulkan::StallProbe;
 std::vector<std::thread> workers;
 for(int t=0;t<4;++t)workers.emplace_back([]{
  for(int i=0;i<5000;++i){Accum draw{draw_ns,&draw_count};Accum build{build_wait_ns,&build_wait_count};}
 });
 for(auto& w:workers)w.join();
 if(Enabled()){assert(draw_count==20000);assert(build_wait_count==20000);}
 else {assert(draw_count==0);assert(build_wait_count==0);assert(draw_ns==0);assert(build_wait_ns==0);}
 ReportFrame();
}
'''
        exe=self.build(cpp,'probe',extra_flags=['-Wall','-Wextra','-Werror=conversion'])
        for value in [None,'0','1']:
            env=os.environ.copy()
            if value is None: env.pop('SUYU_STALL_PROBE',None)
            else: env['SUYU_STALL_PROBE']=value
            run([exe],env=env)

    def test_05_shader_ranges_and_dedup(self):
        if PKG:
            stub=(PKG/'reference/shader_stub.h').read_text()
            body=(PKG/'payload/DefineConstBuffers.txt').read_text()
        else:
            source=(ROOT/'research/ios/tests/test_shader_constant_buffers.py').read_text()
            stub=source.split("STUB = r'''",1)[1].split("'''",1)[0]
            source=(ROOT/'src/shader_recompiler/backend/spirv/spirv_emit_context.cpp').read_text()
            body=section(source,'void DefineConstBuffers(','\nvoid DefineSsbos(')
        checks=r'''int main(){
 for(u32 width:{1u,2u,4u,8u,16u}){
  Info info;info.constant_buffer_descriptors={{3,1},{5,1},{7,1}};
  info.constant_buffer_used_sizes[3]=256;info.constant_buffer_used_sizes[5]=65536;
  EmitContext c;DefineConstBuffers(c,info,&UniformDefinitions::field,0,1,'u',width);
  assert((c.lengths==std::vector<u32>{256/width,65536/width,1}));assert(c.interfaces.size()==3);
  info.uses_global_memory=true;EmitContext global;
  DefineConstBuffers(global,info,&UniformDefinitions::field,0,1,'u',width);
  assert((global.lengths==std::vector<u32>{65536/width}));
  Info shared;shared.constant_buffer_descriptors={{0,1},{1,1},{2,1}};
  shared.constant_buffer_used_sizes[0]=512;shared.constant_buffer_used_sizes[1]=512;shared.constant_buffer_used_sizes[2]=1024;
  EmitContext d;DefineConstBuffers(d,shared,&UniformDefinitions::field,0,1,'u',width);
  assert((d.lengths==std::vector<u32>{512/width,1024/width}));assert(d.interfaces.size()==3);
 }
}'''
        run([self.build(stub+body+checks,'shader')])

    def test_06_static_three_module_abi_transition(self):
        if PKG:
            cmake_text=fixes.ios_cmake((PKG/'reference/StaticModules.cmake').read_text())
            fixture_text=fixes.fixture((PKG/'reference/make_fixture.py').read_text())
        else:
            cmake_text=(ROOT/'research/ios/cmake/StaticModules.cmake').read_text()
            fixture_text=(ROOT/'research/ios/tests/make_fixture.py').read_text()
        (self.d/'StaticModules.cmake').write_text(cmake_text)
        (self.d/'make_fixture.py').write_text(fixture_text)
        exefs=self.d/'exefs [fixture]'
        run([sys.executable,self.d/'make_fixture.py',exefs])
        (self.d/'CMakeLists.txt').write_text("""cmake_minimum_required(VERSION 3.24)
project(ReviewStatic C)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
include("${CMAKE_CURRENT_SOURCE_DIR}/StaticModules.cmake")
switch_aot_add_images(review_images "${CMAKE_CURRENT_SOURCE_DIR}/exefs [fixture]")
add_executable(smoke main.c)
target_include_directories(smoke PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/exefs [fixture]/main")
target_link_libraries(smoke PRIVATE review_images)
""")
        (self.d/'main.c').write_text("""#include "recomp_runtime.h"
#include <assert.h>
struct Module { const char* name; BlockFn (*lookup)(uint64_t); void (*set_base)(uint64_t); };
const struct Module* suyu_recomp_static_modules(unsigned* count);
int switch_aot_enable_guards(void);
int main(int argc,char**argv) {
 (void)argv;
 if(argc>1){assert(!switch_aot_enable_guards());return 0;}
 assert(switch_aot_enable_guards());
 unsigned n=0;const struct Module*m=suyu_recomp_static_modules(&n);assert(n==3);
 for(unsigned i=0;i<n;++i){uint64_t base=0x10000ULL*(i+1);m[i].set_base(base);
  GuestContext c={0};c.pc=base+0x100;BlockFn f=m[i].lookup(c.pc);assert(f);f(&c);
  assert(c.x[0]==1 && c.pending_svc==1 && c.pc==base+0x108);}
 return 0;
}
""")
        build=self.d/'build'
        run(['cmake','-S',self.d,'-B',build,'-DCMAKE_BUILD_TYPE=Debug',
             '-DCMAKE_C_FLAGS='+' '.join(SAN)])
        run(['cmake','--build',build,'--parallel','2'])
        exe=build/('smoke.exe' if os.name=='nt' else 'smoke')
        run([exe])
        # Header advertises ABI 5 but the image returns 4: runtime must refuse.
        export=exefs/'main/recomp_export.c'
        export.write_text(export.read_text().replace('return RECOMP_IMAGE_ABI;', 'return 4;'))
        run(['cmake','--build',build,'--clean-first','--parallel','2'])
        run([exe,'old-abi'])
        # Actual pre-v5 headers must fail configuration, not merely fail to link.
        header=exefs/'main/recomp_runtime.h'
        header.write_text(header.read_text().replace('#define RECOMP_IMAGE_ABI 5','#define RECOMP_IMAGE_ABI 4'))
        p=subprocess.run(['cmake','-S',str(self.d),'-B',str(self.d/'reject')],
                         stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=45)
        self.assertNotEqual(p.returncode,0)
        self.assertIn('correctness ABI 5',p.stdout+p.stderr)

    @unittest.skipIf(PKG is not None, 'full-checkout integration assertions require the full source tree')
    def test_07_integrated_source_contracts(self):
        arm=(ROOT/'src/core/arm/recomp/arm_recomp.cpp').read_text()
        header=(ROOT/'src/core/recompiler/arm64_to_c.h').read_text()
        for banned in ['g_report_process','StartStateWatcher','StartSlotWatcher','_guard_seen',
                       'reinterpret_cast<const u64*>(&g_guard_generation)']:
            self.assertNotIn(banned,arm+header)
        self.assertIn('bridge.guard_generation = nullptr;',arm)
        self.assertIn('impl->SampleDiagnostics(thread);',arm)
        self.assertIn('#define RECOMP_IMAGE_ABI 5',header)
        self.assertNotIn('draw_count.fetch_add',
                         (ROOT/'src/video_core/renderer_vulkan/vk_rasterizer.cpp').read_text())
        for name in ['vk_graphics_pipeline.cpp','vk_compute_pipeline.cpp']:
            self.assertIn('StallProbe::Accum build_probe',
                          (ROOT/'src/video_core/renderer_vulkan'/name).read_text())
        self.assertIn('constexpr unsigned CurrentRecompImageAbi = 5;',
                      (ROOT/'src/suyu/main.cpp').read_text())


if __name__=='__main__':
    print('MODE:', 'retrieved source fragments + new production headers (NOT a full emulator build)' if PKG else 'full checkout; focused tests only')
    unittest.main(argv=[__file__]+rest,verbosity=2)
