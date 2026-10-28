#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "app/tuning_profiles.h"
#include <cassert>
#include <cmath>
#include <fstream>
#include <iostream>
#include <limits>
#include <mutex>

namespace fixture {std::filesystem::path folder;}
namespace app {
Settings& Get(){static Settings s;return s;}
std::filesystem::path ExeDir(){return fixture::folder;}
std::recursive_mutex& UiLock(){static std::recursive_mutex lock;return lock;}
}
namespace p=app::tuning_profiles;
namespace fs=std::filesystem;
std::string Read(const fs::path& f){std::ifstream in(f,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
void Write(const fs::path& f,const std::string& bytes){std::ofstream out(f,std::ios::binary);out<<bytes;assert(out.good());}
bool Near(float a,float b){return std::abs(a-b)<.00001f;}

int main(){
    const auto temp=fs::canonical(fs::temp_directory_path());
    fixture::folder=temp/("mc3-car-profile-store-"+std::to_string(GetCurrentProcessId())+"-"+std::to_string(GetTickCount64()));
    assert(!fs::exists(fixture::folder)&&fs::create_directory(fixture::folder));
    assert(fs::equivalent(fixture::folder.parent_path(),temp));
    const auto file=fixture::folder/"config/tuning_profiles.toml";
    auto& global=app::Get().tuning;global.ride_front=.05f;global.engine_power=1.1f;
    p::Load();assert(!p::Dirty()&&!p::Snapshot().ready&&!fs::exists(file));
    const p::Car twin{"vp_belair_57","abcdef"};std::vector<p::Car> roster{twin,twin};
    p::Observe("hdd7/Player 1","Player 1",roster,0);auto first=p::Snapshot().key;
    assert(p::Snapshot().ready&&!first.empty()&&!p::Snapshot().override_enabled);
    // Merely viewing controls must not create an override.
    {std::lock_guard lock(app::UiLock());p::Editor();}assert(!p::Snapshot().override_enabled);
    {std::lock_guard lock(app::UiLock());auto& t=p::Editor();t.ride_front=-.4f;t.engine_power=1.8f;assert(p::CommitEditor());}
    assert(Near(p::Effective().ride_front,-.4f));
    p::Observe("hdd7/Player 1","Player 1",roster,1);auto second=p::Snapshot().key;
    assert(second!=first&&Near(p::Effective().ride_front,.05f));
    {std::lock_guard lock(app::UiLock());auto& t=p::Editor();t.ride_front=.2f;t.camber_rear=-8;assert(p::CommitEditor());}
    p::Observe("hdd7/Player 1","Player 1",roster,0);assert(p::Snapshot().key==first&&Near(p::Effective().ride_front,-.4f));
    assert(Near(p::EffectiveForKey(second).ride_front,.2f));
    // An occupied CREATE_NEW name is foreign to Save, even if it looks like our
    // PID/serial pattern. Failed creation must never clean up that existing file.
    assert(fs::create_directory(file.parent_path()));
    auto occupied=file;occupied+=L".new-"+std::to_wstring(GetCurrentProcessId())+L"-1";
    Write(occupied,"existing unrelated file");assert(!p::Save());
    assert(Read(occupied)=="existing unrelated file"&&fs::remove(occupied));
    assert(p::Save()&&!p::Dirty());const auto serialized=Read(file);assert(!serialized.empty());
    p::Load();p::Observe("hdd7/Player 1","Player 1",roster,1);
    assert(p::Snapshot().key==second&&Near(p::Effective().camber_rear,-8));
    assert(p::SetProfilesEnabled(false)&&!p::Snapshot().profiles_enabled);
    assert(Near(p::EffectiveForKey(first).ride_front,.05f)&&Near(p::EffectiveForKey(second).ride_front,.05f));
    assert(p::Save());p::Load();p::Observe("hdd7/Player 1","Player 1",roster,1);
    assert(!p::Snapshot().profiles_enabled&&Near(p::Effective().ride_front,.05f));
    assert(p::SetProfilesEnabled(true)&&p::Save());p::Load();p::Observe("hdd7/Player 1","Player 1",roster,1);
    assert(Near(p::EffectiveForKey(first).ride_front,-.4f)&&Near(p::EffectiveForKey(second).ride_front,.2f));
    // An observed parts edit keeps an owned car's identity. Normal serialization
    // is handled by the game-adapter test, not simulated by this store test.
    roster[1].signature="012345";p::Observe("hdd7/Player 1","Player 1",roster,1);
    assert(p::Snapshot().key==second&&Near(p::Effective().ride_front,.2f));assert(p::Save());
    p::Load();p::Observe("hdd7/Player 1","Player 1",roster,1);assert(p::Snapshot().key==second);
    // Observed sale/compaction carries the second identical-model car's ID forward.
    const auto before=roster;roster.erase(roster.begin());
    assert(p::Removed("hdd7/Player 1",0,before,roster,0));
    p::Observe("hdd7/Player 1","Player 1",roster,0);assert(p::Snapshot().key==second&&Near(p::Effective().ride_front,.2f));
    assert(p::UseGlobal()&&Near(p::Effective().ride_front,.05f));
    assert(p::CreateOverride());{std::lock_guard lock(app::UiLock());auto& t=p::Editor();t.ride_front=-99;t.engine_power=std::numeric_limits<float>::infinity();assert(p::CommitEditor());}
    assert(p::Effective().ride_front==-2&&p::Effective().engine_power==1);
    assert(p::ResetOverride()&&p::Effective().ride_front==0);
    assert(p::DeleteOverride()&&Near(p::Effective().ride_front,.05f));
    assert(p::SetGlobalEditing(true));{std::lock_guard lock(app::UiLock());p::Editor().ride_front=.1f;assert(p::CommitEditor());}
    assert(Near(global.ride_front,.1f));assert(p::SetGlobalEditing(false));assert(p::Save());
    // A different HDD/profile namespace cannot inherit the old car overrides.
    p::Observe("hdd8/Player 1","Player 1",roster,0);assert(p::Snapshot().key!=second&&!p::Snapshot().override_enabled);
    auto legacy=serialized;const auto mode_line=legacy.find("profiles_enabled = true");assert(mode_line!=std::string::npos);
    legacy.erase(mode_line,legacy.find('\n',mode_line)-mode_line+1);Write(file,legacy);p::Load();
    assert(p::Snapshot().profiles_enabled&&!p::Snapshot().storage_read_only);
    // Unknown formats and concurrent external edits are kept byte-for-byte.
    Write(file,"version=999\n");p::Load();assert(p::Snapshot().storage_read_only&&!p::Save()&&Read(file)=="version=999\n");
    Write(file,serialized);p::Load();p::Observe("hdd7/Player 1","Player 1",before,0);assert(p::CreateOverride());
    Write(file,"# external user edit\n"+serialized);const auto external=Read(file);assert(!p::Save()&&Read(file)==external&&p::Snapshot().storage_read_only);
    assert(fs::remove(file)&&fs::remove(file.parent_path())&&fs::remove(fixture::folder));
    std::cout<<"car-profile store: distinct twins, switch/reload, sale, fallbacks and preserved malformed/external data passed\n";
}
