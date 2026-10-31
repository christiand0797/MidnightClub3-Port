#include "gpu/texture_pack.h"
#include "app/app.h"
#include "core/log.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <stdexcept>

namespace {
app::Settings preferences;
unsigned settings_reads=0,executable_path_reads=0,log_calls=0,hash_calls=0;
}
namespace app {
Settings& Get() {++settings_reads;throw std::logic_error("quarantined lookup reached settings");}
std::filesystem::path ExeDir() {++executable_path_reads;throw std::logic_error("quarantined lookup reached filesystem root");}
}
namespace logx {
void Write(Level,const char*,...) {++log_calls;}
}
namespace gpu {
uint64_t Hash(const void*,size_t,uint64_t) {++hash_calls;return 0;}
}

int main() {
    static_assert(!gpu::pack::LegacyPbrCompanionsAllowed());
    for(bool enabled:{false,true}) {
        preferences.pbr.enabled=enabled;
        preferences.pbr.pack_path="unapproved-companion-directory";
        preferences.pbr.approved_manifest_sha256=std::string(64,'a');
        // These are the actual production functions linked from texture_pack.cpp, not a test clone.
        for(uint64_t hash:{0ull,1ull,0xffffffffffffffffull}) {
            assert(!gpu::pack::PbrEnabled());
            assert(!gpu::pack::FindPbr(hash));
            auto output=std::make_unique<gpu::pack::PbrMaps>();
            output->nrm.width=1;output->nrm.height=1;output->nrm.levels={{1,2,3,255}};
            assert(gpu::pack::PollPbr(hash,output)==gpu::pack::PbrPoll::None);
            assert(!output);
        }
    }
    assert(settings_reads==0&&executable_path_reads==0&&log_calls==0&&hash_calls==0);
    std::cout<<"legacy PBR quarantine: actual entry points returned disabled/null/None without reaching settings or path lookup\n";
    // This checks synchronous entry-point control flow. It is not native/GPU testing or system-wide worker observation.
}
