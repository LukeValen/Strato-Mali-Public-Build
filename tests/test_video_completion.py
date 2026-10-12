#!/usr/bin/env python3
"""Exercise the production THI method interface and completion ordering."""
import os
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[1]
thi = root / 'app/src/main/cpp/skyline/soc/host1x'
with tempfile.TemporaryDirectory() as directory:
    tmp = Path(directory)
    (tmp/'classes').mkdir()
    for name in ['tegra_host_interface.h', 'classes/class.h']:
        (tmp/name).write_text((thi/name).read_text())
    (tmp/'common.h').write_text('''#pragma once
#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <utility>
using u8=uint8_t;using u16=uint16_t;using u32=uint32_t;
#define LOGD(...) ((void)0)
#define LOGW(...) ((void)0)
#define LOGE(...) ((void)0)
''')
    (tmp/'syncpoint.h').write_text('''#pragma once
#include <common.h>
namespace skyline::soc::host1x {
struct Syncpoint {unsigned value{};std::function<void()> onIncrement;void Increment(){if(onIncrement)onIncrement();++value;}};
using SyncpointSet=std::array<Syncpoint,192>;
}
''')
    (tmp/'test.cpp').write_text('''#include <cassert>
#include "tegra_host_interface.h"
using namespace skyline::soc::host1x;
struct Engine {
 std::function<void()> done; bool& output; bool& fail;
 Engine(std::function<void()> done,bool& output,bool& fail):done(done),output(output),fail(fail){}
 void CallMethod(u32 method,u32 argument){assert(method==0xc0&&argument==1);if(fail)throw std::runtime_error("decoder failed");output=true;done();}
};
int main(){
 SyncpointSet points{};bool output=false,fail=false;
 TegraHostInterface<Engine> thi(points,output,fail);
 points[54].onIncrement=[&]{assert(output);};
 thi.CallMethod(0x10,0xc0);assert(points[54].value==0&&!output);
 thi.CallMethod(0x11,1);assert(output&&points[54].value==0);
 for(u32 condition:{1,2,3})thi.CallMethod(0,(condition<<8)|54);
 assert(points[54].value==3);
 fail=true;output=false;bool caught=false;
 try{thi.CallMethod(0x11,1);thi.CallMethod(0,0x100|54);}catch(const std::runtime_error&){caught=true;}
 assert(caught&&!output&&points[54].value==3);
}
''')
    subprocess.run([os.environ.get('CXX','c++'),'-std=c++20','-g','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I'+str(tmp),str(tmp/'test.cpp'),'-o',str(tmp/'test')],check=True)
    subprocess.run([str(tmp/'test')],check=True)
print('PASS: production THI method dispatch, completion after engine output and no completion after decode failure')
