// SPDX-License-Identifier: GPL-2.0-or-later
// Original fixed-point instructions, supplied on stdin. No game input.
#include "core/recompiler/arm64_to_c.h"
#include <array>
#include <fstream>
#include <iostream>
int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto dir = std::filesystem::path(argv[1]);
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "recomp_runtime.h") << suyu::recomp::RuntimeH();
    std::ofstream out(dir / "operations.c");
    out << "#include \"recomp_runtime.h\"\n#include <stdio.h>\n#include <inttypes.h>\n";
    uint32_t insn;
    unsigned index = 0;
    std::string cases;
    while (std::cin >> std::hex >> insn) {
        std::string code;
        bool unhandled = false;
        suyu::recomp::g_translate_all = false;
        suyu::recomp::Translate(insn, 0x100, code, &unhandled);
        if (unhandled) { std::cerr << "unexpected unhandled " << std::hex << insn; return 3; }
        suyu::recomp::g_translate_all = true;
        std::string second;
        suyu::recomp::Translate(insn, 0x100, second, &unhandled);
        if (unhandled || second != code) return 4;
        out << "static void op" << index << "(GuestContext*c){\n" << code << "}\n";
        cases += "case " + std::to_string(index) + ": op" + std::to_string(index) + "(&c);break;\n";
        ++index;
    }
    out << R"C(
int main(void){unsigned index,kind,rn,rd;uint64_t bits,fpcr,status;
 while(scanf("%u %u %u %u %" SCNx64 " %" SCNx64 " %" SCNx64,&index,&kind,&rn,&rd,&bits,&fpcr,&status)==7){
  GuestContext c={0};
  c.fpcr=fpcr;c.fpsr=status;c.pc=0x100;c.n=1;c.z=0;c.c=1;c.v=1;
  for(unsigned k=0;k<32;++k){c.x[k]=UINT64_C(0xa5a5a5a5a5a5a5a5);c.vreg[k][0]=UINT64_C(0xa5a5a5a5a5a5a5a5);c.vreg[k][1]=UINT64_MAX;}
  if(kind<2)c.vreg[rn][0]=bits;else if(rn!=31)c.x[rn]=bits;
  GuestContext before=c;
  switch(index){
)C" << cases << R"C(
   default:return 9;
  }
  for(unsigned k=0;k<32;++k){
   if(!(kind<2 && k==rd && rd!=31) && c.x[k]!=before.x[k])return 10;
   if(!(kind>=2 && k==rd) && (c.vreg[k][0]!=before.vreg[k][0]||c.vreg[k][1]!=before.vreg[k][1]))return 11;
  }
  if(c.pc!=before.pc||c.fpcr!=before.fpcr||c.n!=1||c.z!=0||c.c!=1||c.v!=1||c.halted)return 12;
  if(kind>=2 && c.vreg[rd][1]!=0)return 13;
  printf("%016" PRIx64 " %016" PRIx64 "\n",kind<2?c.x[rd]:c.vreg[rd][0],c.fpsr);
 }
 return ferror(stdin)?14:0;
}
)C";
    constexpr std::array<uint32_t,2> words{0x1e19e049U,0xd4000021U};
    const auto result = suyu::recomp::EmitProject("fixed", (const uint8_t*)words.data(),
        sizeof(words), 0x100, (dir / "module").string(), true);
    if(result.unhandled) return 5;
    if(!suyu::recomp::TranslateAllForExport(true,false) ||
       !suyu::recomp::TranslateAllForExport(true,true) ||
       !suyu::recomp::TranslateAllForExport(false,false) ||
       !suyu::recomp::TranslateAllForExport(false,true)) return 6;
    return 0;
}
