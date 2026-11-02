#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Real Translate/RuntimeH fixed-point tests. No game content or JIT.
Python 3.9+, C/C++ toolchain, CMake and Ninja. FP_SANITIZE=1 enables ASan/UBSan.
"""
from __future__ import annotations
from fractions import Fraction
import math, os, random, shlex, struct, subprocess, tempfile, unittest
from pathlib import Path
ROOT=Path(__file__).resolve().parents[2]

def run(argv, **kw):
    p=subprocess.run(list(map(str,argv)),capture_output=True,text=True,timeout=180,**kw)
    if p.returncode:raise AssertionError(f'{argv}\nexit={p.returncode}\n{p.stdout[-5000:]}\n{p.stderr[-10000:]}')
    return p

def build(src,out,cpp=False,includes=(),sanitize=False):
    tool=os.environ.get('CXX' if cpp else 'CC')
    cmd=shlex.split(tool,posix=os.name!='nt') if tool else (['cl'] if os.name=='nt' else ['c++' if cpp else 'cc'])
    msvc=Path(cmd[0]).name.lower() in ('cl','cl.exe')
    if msvc:
        cmd+=['/nologo','/EHsc','/std:c++20' if cpp else '/std:c11','/TP' if cpp else '/TC','/Od','/Fe:'+str(out),'/Fo:'+str(out)+'.obj']
        cmd+=['/I'+str(p) for p in includes]
    else:
        cmd+=['-std=c++20' if cpp else '-std=c11','-O1','-o',str(out)]
        cmd+=['-I'+str(p) for p in includes]
        if not cpp:cmd+=['-Wall','-Wextra','-Werror','-Wconversion']
        if sanitize:cmd+=['-fsanitize=address,undefined','-fno-omit-frame-pointer','-fno-pie','-no-pie']
    return run(cmd+[src])

def specs():
    out=[]
    bases=[('zs',0x1e180000),('zu',0x1e190000),('sc',0x1e020000),('uc',0x1e030000)]
    for kind,base in bases:
        for fw in (32,64):
            for iw in (32,64):
                for frac in range(1,iw+1):
                    word=base|((iw==64)<<31)|((fw==64)<<22)|((64-frac)<<10)|(2<<5)|9
                    out.append((kind,fw,iw,frac,2,9,word))
    for kind,base in bases:
        for rn,rd in [(31,9),(2,31),(31,31),(9,9)]:
            out.append((kind,32,32,8,rn,rd,base|((64-8)<<10)|(rn<<5)|rd))
    return out

def fval(raw,w):return struct.unpack('<d' if w==64 else '<f',(raw&((1<<w)-1)).to_bytes(w//8,'little'))[0]
def fbits(x,w):return int.from_bytes(struct.pack('<d' if w==64 else '<f',x),'little')

def to_int(raw,fw,iw,frac,signed,fpcr,status):
    raw&=(1<<fw)-1;f=52 if fw==64 else 23;ex=(raw>>f)&(2047 if fw==64 else 255)
    payload=raw&((1<<f)-1);x=fval(raw,fw);lo=-(1<<(iw-1)) if signed else 0;hi=(1<<(iw-1))-1 if signed else (1<<iw)-1
    if math.isnan(x):return 0,status|1
    if math.isinf(x):return (lo if x<0 else hi)&((1<<iw)-1),status|1
    if ex==0 and payload and fpcr&(1<<24):return 0,status|128
    exact=Fraction.from_float(x)*(1<<frac);rounded=int(exact)
    if not lo<=rounded<=hi:return min(max(rounded,lo),hi)&((1<<iw)-1),status|1
    return rounded&((1<<iw)-1),status|(16 if Fraction(rounded)!=exact else 0)

def to_fp(raw,fw,iw,frac,signed,fpcr,status):
    x=raw&((1<<iw)-1)
    if signed and x&(1<<(iw-1)):x-=1<<iw
    if not x:return 0,status
    target=Fraction(abs(x),1<<frac);approx=fbits(float(target),fw)
    # Exact rational distances correct any double-rounding in the candidate.
    choices=[(b,Fraction.from_float(fval(b,fw))) for b in (approx-1,approx,approx+1)]
    rm=(fpcr>>22)&3
    if rm==0:b,v=min(choices,key=lambda p:(abs(p[1]-target),p[0]&1))
    elif (rm==1 and x>0) or (rm==2 and x<0):b,v=min((p for p in choices if p[1]>=target),key=lambda p:p[1])
    else:b,v=max((p for p in choices if p[1]<=target),key=lambda p:p[1])
    return b|((x<0)<<(fw-1)),status|(16 if v!=target else 0)

INTEGRATION=r'''
#include "recomp_runtime.h"
#include <assert.h>
#include <stdio.h>
extern BlockFn recomp_image_lookup_fixed(uint64_t);
extern void recomp_image_set_base_fixed(uint64_t);
extern unsigned recomp_image_guard_v2_fixed(unsigned);
extern unsigned recomp_image_abi_fixed(void);
int main(int argc,char**argv){
 uint32_t words[2]={0x1e19e049U,0xd4000021U};GuestContext c={0};
 c.mem=(uint8_t*)words;c.mem_size=sizeof(words);c.mem_base_vaddr=0x100100;c.pc=0x100100;
 c.vreg[2][0]=0x3fa00000;c.fpsr=16;c.chain_budget=32;c.pending_svc=UINT64_MAX;
 recomp_image_set_base_fixed(0x100000);assert(recomp_image_abi_fixed()==5);assert(recomp_image_guard_v2_fixed(2)==2);
 if(argc>1){(void)argv;words[0]^=1;}
 BlockFn f=recomp_image_lookup_fixed(c.pc);assert(f);f(&c);
 assert(c.x[9]==320&&c.fpsr==16&&c.pending_svc==1&&c.pc==0x100108);
 c.pc=0x100104;c.x[9]=77;c.pending_svc=UINT64_MAX;c.halted=0;
 f=recomp_image_lookup_fixed(c.pc);assert(f);f(&c);assert(c.x[9]==77&&c.pending_svc==1);
 puts("PASS: real FCVTZU/SVC module, base relocation, guard, and side entry");return 0;
}
'''
class FixedPointTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp=tempfile.TemporaryDirectory(prefix='suyu-fixed-point-');cls.d=Path(cls.tmp.name);cls.cases=specs()
        suffix='.exe' if os.name=='nt' else '';exe=cls.d/('generator'+suffix)
        build(ROOT/'research/tests/fixed_point_emit_fixture.cpp',exe,cpp=True,includes=[ROOT/'src'])
        run([exe,cls.d],input='\n'.join(f'{case[-1]:08x}' for case in cls.cases)+'\n')
        cls.runner=cls.d/('operations'+suffix)
        build(cls.d/'operations.c',cls.runner,includes=[cls.d],sanitize=os.environ.get('FP_SANITIZE')=='1')
    @classmethod
    def tearDownClass(cls):cls.tmp.cleanup()
    def evaluate(self,requests):
        inputs=[];expected=[];kinds={'zs':0,'zu':1,'sc':2,'uc':3}
        for idx,bits,cr,status in requests:
            kind,fw,iw,frac,rn,rd,_=self.cases[idx];value=bits if kind in ('zs','zu') or rn!=31 else 0
            fn=to_int if kind in ('zs','zu') else to_fp
            result,flags=fn(value,fw,iw,frac,kind in ('zs','sc'),cr,status)
            if kind in ('zs','zu') and rd==31:result=0xa5a5a5a5a5a5a5a5
            inputs.append(f'{idx} {kinds[kind]} {rn} {rd} {bits:x} {cr:x} {status:x}\n');expected.append((result,flags))
        p=run([self.runner],input=''.join(inputs));lines=p.stdout.splitlines();self.assertEqual(len(lines),len(expected))
        for req,want,line in zip(requests,expected,lines):
            self.assertEqual(tuple(int(v,16) for v in line.split()),want,msg=f'spec={self.cases[req[0]]} inputs={req[1:]}')
        return len(expected)
    def test_01_reported_opcode_and_status(self):
        idx=next(i for i,c in enumerate(self.cases) if c[-1]==0x1e19e049)
        values=[0,0x80000000,1,0x80000001,0x3fa00000,0x3f800001,0xbb000000,0xbb800000,0x4b7fffff,0x4b800000,0x7f7fffff,0x7f800000,0xff800000,0x7fc00001,0x7f800001]
        self.evaluate([(idx,v,cr,16) for v in values for cr in (0,1<<24,1<<22,2<<22,3<<22)])
    def test_02_all_encodings_edges_and_seeded_random(self):
        rng=random.Random(0x1e19e049);requests=[]
        for idx,(kind,fw,iw,frac,rn,rd,_) in enumerate(self.cases):
            if kind in ('zs','zu'):
                f=52 if fw==64 else 23;e=2047 if fw==64 else 255
                values=[0,1,(1<<f)-1,1<<f,e<<f,(e<<f)|1,(e<<f)|(1<<(f-1)),(e<<f)-1]
                for v in (1.0,1.5,2.0**-frac,0.5*2.0**-frac,2.0**(iw-frac),2.0**(iw-1-frac)):
                    b=fbits(v,fw);values.extend([b-1,b,b+1])
                values += [b|(1<<(fw-1)) for b in list(values)]
                values += [rng.getrandbits(fw) for _ in range(24)]
                values=[b|(rng.getrandbits(32)<<32) if fw==32 else b for b in values]
            else:
                values=[0,1,2,3,(1<<iw)-1,1<<(iw-1),(1<<(iw-1))-1]
                for v in (1<<23,1<<24,1<<52,1<<53,1<<63):
                    if v<(1<<iw):values.extend([v-1,v+1,v+3])
                values += [rng.getrandbits(64) for _ in range(24)]
            for b in values:
                for cr in (0,1<<22,2<<22,3<<22,(1<<24)|(1<<25)):requests.append((idx,b,cr,0x08000002))
        n=self.evaluate(requests);print(f'PASS: {len(self.cases)} instruction forms; {n} result/FPSR/state cases',flush=True)
    def test_03_invalid_encodings_are_not_enabled(self):
        src=r'''
#include "core/recompiler/arm64_to_c.h"
#include <cassert>
int main(){const uint32_t invalid[]={0x1e190000U,0x3e19e049U,0x1e99e049U,0x1ed9e049U,0x1e09e049U,0x1e01e049U};
 for(bool flag:{false,true}){suyu::recomp::g_translate_all=flag;for(auto word:invalid){std::string out;bool bad=false;suyu::recomp::Translate(word,0x100,out,&bad);assert(bad);}}}
'''
        p=self.d/'invalid.cpp';p.write_text(src);exe=self.d/('invalid.exe' if os.name=='nt' else 'invalid')
        build(p,exe,cpp=True,includes=[ROOT/'src']);run([exe])
    def test_04_real_emitted_module_guard_and_side_entry(self):
        (self.d/'integration.c').write_text(INTEGRATION)
        (self.d/'CMakeLists.txt').write_text('''cmake_minimum_required(VERSION 3.15)
project(fixed_point_integration C)
set(CMAKE_C_STANDARD 11)
set(RECOMP_STATIC_ONLY ON)
add_subdirectory(module)
add_executable(integration integration.c)
target_link_libraries(integration PRIVATE recomp_static_fixed)
if(UNIX AND NOT APPLE)
 target_link_libraries(integration PRIVATE m)
endif()
''')
        run(['cmake','-S',self.d,'-B',self.d/'build','-G','Ninja','-DCMAKE_BUILD_TYPE=Debug'])
        run(['cmake','--build',self.d/'build','--parallel','2'])
        exe=self.d/'build'/('integration.exe' if os.name=='nt' else 'integration')
        self.assertIn('PASS:',run([exe]).stdout)
        p=subprocess.run([str(exe),'mutate'],capture_output=True,text=True,timeout=15)
        self.assertNotEqual(p.returncode,0);self.assertIn('unsupported code change or unavailable code',p.stderr)
    def test_05_exporter_policy_revision_and_diagnostics(self):
        t=(ROOT/'src/suyu/game_export.cpp').read_text()
        self.assertIn('const bool translate_all = suyu::recomp::TranslateAllForExport(',t)
        self.assertIn('backend == RecompileBackend::SuyuStatic,',t)
        self.assertIn('suyu::recomp::g_translate_all = translate_all;',t)
        self.assertEqual(t.count('20260920-fixedpoint-v1'),2)
        self.assertIn('same_translate_all &&',t);self.assertIn('\\"translate_all\\": ',t)
        t=(ROOT/'src/core/arm/recomp/arm_recomp.cpp').read_text()
        self.assertNotIn('unimplemented opcode at {:#x}; running on JIT',t)
        self.assertIn('unimplemented opcode {:#010x} at {:#x} and no JIT fallback',t)
        self.assertIn('if (!EnterFallback())',t)
if __name__=='__main__':unittest.main(verbosity=2)
