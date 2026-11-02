#!/usr/bin/env python3
"""Compile the production Windows path resolver against controlled OS responses."""
import argparse
from pathlib import Path
import subprocess
import tempfile
import os

p=argparse.ArgumentParser();p.add_argument('--source',type=Path,default=Path(__file__).resolve().parents[2]);p.add_argument('--cc',default='cl');a=p.parse_args()
if os.name!='nt':
    print('SKIP: Windows APPDATA semantics');raise SystemExit(0)
source=(a.source/'src/common/fs/path_util.cpp').read_text()
start=source.index('fs::path GetAppDataRoamingDirectory() {');end=source.index('{',start);depth=1;end+=1
while depth:
    depth+=(source[end]=='{')-(source[end]=='}');end+=1
resolver=source[start:end]
harness=r'''
#include <filesystem>
#include <string>
#include <cassert>
#include <algorithm>
#include <iostream>
namespace fs=std::filesystem;
using PWSTR=wchar_t*;using HRESULT=long;
constexpr int FOLDERID_RoamingAppData=1;
static std::wstring environment, known;
static HRESULT status=0;static bool return_path=true;
static int calls=0,frees=0,errors=0;
const wchar_t* test_getenv(const wchar_t*){return environment.empty()?nullptr:environment.c_str();}
HRESULT SHGetKnownFolderPath(int,int,void*,PWSTR* out){
 ++calls;*out=nullptr;if(return_path){*out=new wchar_t[known.size()+1];std::copy(known.begin(),known.end(),*out);(*out)[known.size()]=0;}return status;
}
void CoTaskMemFree(PWSTR p){if(p)++frees;delete[] p;}
#define _wgetenv test_getenv
#define SUCCEEDED(x) ((x)>=0)
#define LOG_ERROR(...) (++errors)
'''+resolver+r'''
int main(int argc,char**argv){
 const fs::path root=fs::absolute(argv[1]);fs::create_directories(root);
 const fs::path override_path=root/fs::path(L"override-\u03c0");fs::create_directories(override_path);
 known=(root/"known").wstring();fs::create_directories(known);
 environment=override_path.wstring();assert(GetAppDataRoamingDirectory()==override_path);assert(calls==0&&frees==0);
 environment=(root/"missing").wstring();assert(GetAppDataRoamingDirectory()==fs::path(known));assert(calls==1&&frees==1);
 const fs::path file=root/"not-directory";{std::ofstream out(file);out<<"x";}
 environment=file.wstring();assert(GetAppDataRoamingDirectory()==fs::path(known));assert(calls==2&&frees==2);
 environment.clear();status=-1;return_path=false;assert(GetAppDataRoamingDirectory().empty());assert(errors==1);
 status=0;assert(GetAppDataRoamingDirectory().empty());assert(errors==2);
 status=-1;return_path=true;assert(GetAppDataRoamingDirectory().empty());assert(frees==3&&errors==3);
 std::cout<<"PASS valid Unicode APPDATA, invalid directory/file fallback, HRESULT failure, null response and failed allocated response\n";
}
'''
harness=harness.replace('#include <iostream>','#include <iostream>\n#include <fstream>')
with tempfile.TemporaryDirectory(prefix='suyu-appdata-test-') as d:
    directory=Path(d);cpp=directory/'test.cpp';exe=directory/'test.exe';cpp.write_text(harness)
    subprocess.run([a.cc,'/nologo','/EHsc','/std:c++20',str(cpp),f'/Fe:{exe}',f'/Fo:{directory / "test.obj"}'],check=True)
    subprocess.run([str(exe),str(directory/'files')],check=True)
