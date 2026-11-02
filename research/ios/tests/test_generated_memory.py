#!/usr/bin/env python3
"""Compile actual emitted runtime against synthetic discontiguous guest pages."""
import os
import sys
from pathlib import Path
import subprocess
import tempfile
import unittest

LINK_GC = '-Wl,-dead_strip' if sys.platform == 'darwin' else '-Wl,--gc-sections'
ROOT = Path(__file__).resolve().parents[3]
HARNESS = r'''
#include "recomp_runtime.c"
#include <assert.h>
#include <stdlib.h>
#include <stdio.h>
static unsigned char backing[3][8192];
static unsigned char* pages[4];
static uintptr_t entries[4];
static unsigned loads, stores, faults;
static void map_page(unsigned page, unsigned char* ptr) {
  pages[page]=ptr;
  entries[page]=ptr ? (uintptr_t)ptr-page*4096 : 0;
}
static uint64_t read_cb(void* u,uint64_t a,uint32_t sz) {
  uint64_t v=0; (void)u; ++loads; a &= 0xffffffffffffULL;
  for(unsigned i=0;i<sz;++i) {
    if(a+i>=16384 || !pages[(a+i)>>12]) { ++faults; continue; }
    v |= (uint64_t)pages[(a+i)>>12][(a+i)&4095] << (i*8);
  } return v;
}
static void write_cb(void* u,uint64_t a,uint32_t sz,uint64_t v) {
  (void)u; ++stores; a &= 0xffffffffffffULL;
  for(unsigned i=0;i<sz;++i) {
    if(a+i>=16384 || !pages[(a+i)>>12]) { ++faults; continue; }
    pages[(a+i)>>12][(a+i)&4095]=(unsigned char)(v>>(i*8));
  }
}
int main(void) {
  RecompHostMem hm={0}; GuestContext c={0};
  hm.load=read_cb; hm.store=write_cb; hm.page_entries=entries;
  hm.page_entry_stride=sizeof(uintptr_t); hm.page_bits=12;
  hm.pointer_mask=UINT64_MAX; hm.address_space_max=16384; c.host_mem=&hm;
  uint64_t (*ld[])(GuestContext*,uint64_t)={recomp_load16,recomp_load32,recomp_load64};
  void (*st[])(GuestContext*,uint64_t,uint64_t)={recomp_store16,recomp_store32,recomp_store64};
  for(unsigned k=0;k<3;++k) {
    unsigned size=2U<<k; uint64_t mask=UINT64_MAX>>(64-size*8);
    for(unsigned cross=1;cross<size;++cross) {
      memset(backing,0xA5,sizeof(backing));
      map_page(0,backing[0]); map_page(1,backing[2]); map_page(2,backing[0]); map_page(3,0);
      uint64_t a=4096-cross, v=0x8877665544332211ULL & mask;
      unsigned before=stores; st[k](&c,a,v); assert(stores==before+1);
      before=loads; assert(ld[k](&c,a)==v); assert(loads==before+1);
      assert(backing[0][4096]==0xA5); /* Must not touch adjacent host bytes. */
      assert(ld[k](&c,8192+4096-cross)==(v & (UINT64_MAX>>(64-cross*8))));
      map_page(1,backing[1]); assert(ld[k](&c,a)==read_cb(0,a,size));
      map_page(1,0); before=faults; (void)ld[k](&c,a); assert(faults>before);
      before=faults; st[k](&c,a,v); assert(faults>before);
    }
    map_page(0,backing[0]); unsigned before=loads;
    st[k](&c,128,0x1234); assert(ld[k](&c,128)==0x1234); assert(loads==before);
    assert(ld[k](&c,0xabcd000000000080ULL)==0x1234);
    map_page(3,backing[1]); hm.address_space_max=16381;
    before=loads; (void)ld[k](&c,16380); assert(loads==before+1);
    hm.address_space_max=16384;
  }
  map_page(0,backing[0]); map_page(1,backing[2]);
  for(unsigned shift=64;shift<=65;++shift) {
    hm.page_bits=shift; unsigned before=loads;
    assert(recomp_load32(&c,128)==read_cb(0,128,4)); assert(loads==before+2);
  }
  hm.page_bits=1; { unsigned before=loads; (void)recomp_load64(&c,0); assert(loads==before+1); }
  hm.page_bits=12;
  for(unsigned size=8;size<=16;size+=8) {
    hm.address_space_max=129; unsigned before=loads; uint64_t lo,hi;
    if(size==8) recomp_ldp32(&c,128,&lo,&hi); else recomp_ldp64(&c,128,&lo,&hi);
    assert(loads==before+2);
    before=stores;
    if(size==8) recomp_stp32(&c,128,lo,hi); else recomp_stp64(&c,128,lo,hi);
    assert(stores==before+2);
  }
  hm.address_space_max=16384;
  puts("PASS: emitted scalar memory spans, discontiguous pages, alias/remap/unmap, callbacks");
}
'''

def run(args):
    result = subprocess.run(list(map(str,args)), stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode: raise AssertionError(result.stdout)
    return result.stdout

class GeneratedMemory(unittest.TestCase):
    def test_production_host_callbacks(self):
        source=(ROOT/'src/core/arm/recomp/arm_recomp.cpp').read_text()
        start=source.index('    static u64 HostLoad(void* user')
        end=source.index('    /// Base address',start)
        callbacks=source[start:end]
        # Compile the exact production callback bodies, not a test reimplementation.
        # Memory and logging are test doubles. The production callback bodies
        # are compiled unchanged, including scalar dispatch and span validation.
        stub = r'''
#include <algorithm>
#include <string>
using u64=uint64_t; using u32=uint32_t; using u16=uint16_t; using u8=uint8_t;
// Formatting is diagnostic-only and cannot affect the memory result.
namespace fmt { template<class... A> std::string format(const char*, A&&...) { return {}; } }
#define LOG_ERROR(...) ((void)0)
constexpr u64 kTrapStoreLo=0, kTrapStoreHi=0;
struct TestMemory {
    bool IsValidVirtualAddress(u64 a) const { return a < 16384 && pages[a>>12]; }
    u8 Read8(u64 a) { return (u8)read_cb(nullptr,a,1); }
    u16 Read16(u64 a) { return (u16)read_cb(nullptr,a,2); }
    u32 Read32(u64 a) { return (u32)read_cb(nullptr,a,4); }
    u64 Read64(u64 a) { return read_cb(nullptr,a,8); }
    void Write8(u64 a,u8 v) { write_cb(nullptr,a,1,v); }
    void Write16(u64 a,u16 v) { write_cb(nullptr,a,2,v); }
    void Write32(u64 a,u32 v) { write_cb(nullptr,a,4,v); }
    void Write64(u64 a,u64 v) { write_cb(nullptr,a,8,v); }
};
struct TestSystem { TestMemory memory; TestMemory& ApplicationMemory() { return memory; } };
struct Impl { TestSystem system;
    static constexpr size_t kTrail=32;
    size_t trail_pos=0; u64 trail[kTrail]{};
    struct { u64 pc=0; u64 x[32]{}; } ctx;
    static void ReportUnmapped(Impl*,u64,u32,const char*) {}
''' + callbacks + '\n};\n'
        h=HARNESS.replace('#include "recomp_runtime.c"', 'extern "C" {\n#include "recomp_runtime.h"\n}')
        h=h.replace('int main(void) {',stub+'int main(void) {\n  Impl impl;')
        h=h.replace('hm.load=read_cb; hm.store=write_cb;', 'hm.user=&impl; hm.load=Impl::HostLoad; hm.store=Impl::HostStore;')
        h=h.replace('  puts("PASS:', r'''  unsigned before=loads; assert(Impl::HostLoad(&impl,UINT64_MAX,8)==0); assert(loads==before);
  before=stores; Impl::HostStore(&impl,UINT64_MAX,8,1); assert(stores==before);
  for(u32 width : {3U,5U,16U,UINT32_MAX}) {
    before=loads; assert(Impl::HostLoad(&impl,0,width)==0); assert(loads==before);
    before=stores; Impl::HostStore(&impl,0,width,1); assert(stores==before);
  }
  before=loads; assert(Impl::HostLoad(&impl,UINT64_MAX-1,0)==0); assert(loads==before);
  map_page(0,backing[0]); memset(backing[0],0,4);
  assert(Impl::HostLoad(&impl,0,0)==(1ULL<<32)); // A mapped zero word is valid.
  map_page(0,0); assert(Impl::HostLoad(&impl,0,0)==0);
  puts("PASS:''')
        with tempfile.TemporaryDirectory(prefix='ihorizon-host-memory-') as tmp:
            out=Path(tmp)
            run([os.environ.get('CXX','c++'),'-std=c++20','-O0','-I'+str(ROOT/'src'),ROOT/'research/ios/tests/export_synthetic.cpp','-o',out/'emit'])
            run([out/'emit',out/'modules'])
            module=out/'modules/main'
            (module/'host_memory_test.cpp').write_text(h)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections','-c',module/'recomp_runtime.c','-o',out/'runtime.o'])
            run([os.environ.get('CXX','c++'),'-std=c++20','-O1','-ffunction-sections','-fdata-sections',module/'host_memory_test.cpp',out/'runtime.o',LINK_GC,'-lm','-o',out/'test'])
            self.assertIn('PASS:',run([out/'test']))

    def test_real_emitted_memory(self):
        with tempfile.TemporaryDirectory(prefix='ihorizon-memory-') as tmp:
            out=Path(tmp)
            run([os.environ.get('CXX','c++'),'-std=c++20','-O0','-I'+str(ROOT/'src'),ROOT/'research/ios/tests/export_synthetic.cpp','-o',out/'emit'])
            run([out/'emit',out/'modules'])
            module=out/'modules/main'
            (module/'memory_test.c').write_text(HARNESS)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections',module/'memory_test.c',LINK_GC,'-lm','-o',out/'test'])
            self.assertIn('PASS:',run([out/'test']))
            source=(module/'recomp_runtime.c').read_text()
            # Mutate only the actual current helper: accepting all spans must
            # fail the discontiguous-page/bounds assertions above.
            start=source.index('static unsigned char* recomp_host_ptr_n(GuestContext* c, uint64_t va, uint64_t bytes){')
            end=source.index('\n}',start)+2
            old=source[:start]+('static unsigned char* recomp_host_ptr_n(GuestContext* c, uint64_t va, uint64_t bytes){'
                 '(void)bytes; return recomp_host_ptr(c,va);}')+source[end:]
            # Prove the regression is detected against the pre-fix emitted runtime.
            (module/'recomp_runtime.c').write_text(old)
            run([os.environ.get('CC','cc'),'-std=c11','-O1','-DSUYU_HOSTED_RECOMP=1','-DRECOMP_STATIC_HOST=1','-ffunction-sections','-fdata-sections',module/'memory_test.c',LINK_GC,'-lm','-o',out/'old-test'])
            result=subprocess.run([out/'old-test'],stdout=subprocess.PIPE,stderr=subprocess.PIPE)
            self.assertNotEqual(result.returncode,0)

if __name__=='__main__': unittest.main(verbosity=2)
