#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compile both Sirit callback contracts and execute actual emitted block guards.
No game files, keys, firmware, renderer mocks, or network access are used.
"""
from pathlib import Path
import ast, os, re, shlex, shutil, subprocess, sys, tempfile, unittest
ROOT=Path(__file__).resolve().parents[2]
CXX=shlex.split(os.environ.get('CXX','c++'))


def run(args, cwd=None, success=True):
    p=subprocess.run(list(map(str,args)),cwd=cwd,capture_output=True,text=True,timeout=90)
    if (p.returncode==0) != success:
        raise AssertionError(f'{args}\nexit={p.returncode}\n{p.stdout}\n{p.stderr}')
    return p

class BuildRegressions(unittest.TestCase):
    def test_new_memory_fixes_invalidate_earlier_abi5_export_caches(self):
        source=(ROOT/'src/suyu/game_export.cpp').read_text()
        self.assertEqual(source.count('20260920-fixedpoint-v1'),2)
        self.assertIn('same_scan && same_backend && same_image_abi && same_correctness_revision',source)
        self.assertIn('const bool same_correctness_revision = contents.contains(',source)
        self.assertIn('same_translate_all && same_source && same_fallback_policy &&',source)
        self.assertIn('"source_exefs_sha256"',source.replace('\\"', '"'))

    @unittest.skipUnless(os.name == 'nt', 'requires retained Windows Qt/MSVC toolchain')
    def test_synthetic_export_exefs_replacement(self):
        run([sys.executable, ROOT/'tests/export_regression/run_fixture.py'])

    def test_generated_registry_declares_public_c_entry_points(self):
        # The registration TU is built with the host warning policy, unlike
        # generated block units. Extract the actual declarations from the Qt
        # exporter's output strings and compile with -Wmissing-declarations.
        source=(ROOT/'src/suyu/game_export.cpp').read_text()
        begin=source.index('"} SuyuRecompStaticModule;\\n\\n"')
        end=source.index('"static const SuyuRecompStaticModule s_modules[]',begin)
        emitted=''.join(ast.literal_eval(s) for s in re.findall(r'"(?:[^"\\\n]|\\.)*"',source[begin:end]))
        required=[
            'const SuyuRecompStaticModule* suyu_recomp_static_modules_v4(unsigned* count);\n',
            'int suyu_recomp_static_guard_v2(unsigned version);\n',
        ]
        for declaration in required:self.assertIn(declaration,emitted)
        declarations=''.join(required)
        prefix='typedef struct { const char* name; } SuyuRecompStaticModule;\n'
        body=r"""
static const SuyuRecompStaticModule modules[]={{"test"}};
const SuyuRecompStaticModule* suyu_recomp_static_modules_v4(unsigned* count){*count=1;return modules;}
int suyu_recomp_static_guard_v2(unsigned version){return version==2;}
int main(void){unsigned count=0;return !(suyu_recomp_static_modules_v4(&count)&&count==1&&suyu_recomp_static_guard_v2(2));}
"""
        with tempfile.TemporaryDirectory(prefix='suyu-registry-declarations-') as tmp:
            d=Path(tmp); f=d/'registry.c'; exe=d/'registry'
            cc=shlex.split(os.environ.get('CC','cc'))
            cmd=cc+['-std=c11','-Wall','-Wextra','-Werror=missing-prototypes',f,'-o',exe]
            f.write_text(prefix+declarations+body);run(cmd);run([exe])
            f.write_text(prefix+body)
            result=run(cmd,success=False)
            self.assertRegex(result.stderr.lower(), r'prototype|declaration')

    def test_phi_values_and_parent_labels_for_both_sirit_apis(self):
        text=(ROOT/'src/shader_recompiler/backend/spirv/emit_spirv.cpp').read_text()
        callback=text[text.index('struct DeferredPhiPatch {'):text.index('\nvoid PatchPhiNodes(')]
        body=r'''
#include <cassert>
#include <cstddef>
#include <functional>
#include <utility>
#include <vector>
using Id=unsigned;
namespace IR { struct Inst { std::vector<unsigned> args; unsigned Arg(size_t i)const{return args.at(i);} }; }
struct EmitContext { Id Def(unsigned n)const{return n+100;} };
'''+callback+r'''
int main(){
 EmitContext ctx; IR::Inst a{{1,2}}, b{{3,4,5}};
 std::vector<IR::Inst*> instructions{&a,&b};
 std::function<Id(size_t)> old_api=DeferredPhiPatch{ctx,instructions};
 assert(old_api(0)==101 && old_api(1)==102);
 assert(old_api(0)==103 && old_api(1)==104 && old_api(2)==105);
 std::function<std::pair<Id,Id>(size_t,Id)> new_api=DeferredPhiPatch{ctx,instructions};
 assert((new_api(0,11)==std::pair<Id,Id>{101,11}));
 assert((new_api(1,22)==std::pair<Id,Id>{102,22}));
 assert((new_api(0,33)==std::pair<Id,Id>{103,33}));
 assert((new_api(1,44)==std::pair<Id,Id>{104,44}));
 assert((new_api(2,55)==std::pair<Id,Id>{105,55}));
}
'''
        with tempfile.TemporaryDirectory(prefix='suyu-phi-') as tmp:
            p=Path(tmp);(p/'test.cpp').write_text(body)
            run(CXX+['-std=c++20','-O1','-Wall','-Wextra','-Werror',p/'test.cpp','-o',p/'test'])
            run([p/'test'])
    def test_real_emitter_guard_start_side_entry_and_revalidation(self):
        with tempfile.TemporaryDirectory(prefix='suyu-real-guard-') as tmp:
            p=Path(tmp)
            run(CXX+['-std=c++20','-O1','-I'+str(ROOT/'src'),ROOT/'research/ios/tests/export_synthetic.cpp','-o',p/'emit'])
            run([p/'emit',p/'exefs'])
            (p/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.31)
project(real_guard C)
set(CMAKE_C_STANDARD 11)
set(RECOMP_STATIC_ONLY ON)
add_subdirectory(exefs/main)
add_executable(guard_test guard_test.c)
target_link_libraries(guard_test PRIVATE recomp_static_main)
if(UNIX AND NOT APPLE)
 target_link_libraries(guard_test PRIVATE m)
endif()
''')
            (p/'guard_test.c').write_text(r'''
#include "recomp_runtime.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
extern BlockFn recomp_image_lookup_main(uint64_t pc);
extern void recomp_image_set_base_main(uint64_t base);
extern unsigned recomp_image_guard_v2_main(unsigned version);
extern unsigned recomp_image_abi_main(void);
int main(int argc,char**argv){
 uint32_t words[2]={0x91000400U,0xd4000021U};
 uint32_t remapped[2]={0x91000401U,0xd4000021U};
 const uint64_t pc=0x100100ULL;
 GuestContext c={0}; c.mem=(uint8_t*)words; c.mem_size=sizeof(words); c.mem_base_vaddr=pc;
 c.pc=pc; c.x[0]=41; c.chain_budget=32; c.pending_svc=~UINT64_C(0);
 assert(recomp_image_abi_main()==5);
 recomp_image_set_base_main(0x100000ULL); assert(recomp_image_guard_v2_main(2)==2);
 BlockFn first=recomp_image_lookup_main(pc); assert(first); first(&c);
 assert(c.x[0]==42 && c.pc==pc+8 && c.pending_svc==1);
 c.pc=pc+4; c.pending_svc=~UINT64_C(0); c.x[0]=41;
 BlockFn side=recomp_image_lookup_main(c.pc); assert(side); side(&c);
 assert(c.x[0]==41 && c.pc==pc+8 && c.pending_svc==1);
 if(argc>1){
  c.pc=pc; c.pending_svc=~UINT64_C(0);
  if(!strcmp(argv[1],"mutate"))words[0]^=1;
  else if(!strcmp(argv[1],"remap"))c.mem=(uint8_t*)remapped;
  else if(!strcmp(argv[1],"side-mutate")){words[1]^=1;c.pc=pc+4;}
  else return 8;
  BlockFn reentry=recomp_image_lookup_main(c.pc); assert(reentry); reentry(&c);
  fputs("ERROR: mutated instruction was executed\n",stderr);return 7;
 }
 puts("PASS: real emitted ADD/SVC, instruction side entry and ABI 5");return 0;
}
''')
            run(['cmake','-S',p,'-B',p/'build','-G','Ninja','-DCMAKE_BUILD_TYPE=Debug'])
            run(['cmake','--build',p/'build','--parallel','2'])
            exe=p/'build'/('guard_test.exe' if os.name=='nt' else 'guard_test')
            self.assertIn('PASS:',run([exe]).stdout)
            for mode in ['mutate','remap','side-mutate']:
                with self.subTest(mode=mode):
                    result=run([exe,mode],success=False)
                    self.assertNotIn(result.returncode,[7,8])
                    self.assertIn('unsupported code change or unavailable code',result.stderr.lower())

if __name__=='__main__': unittest.main(verbosity=2)
