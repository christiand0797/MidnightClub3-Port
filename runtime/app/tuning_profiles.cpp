#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "app/tuning_profiles.h"
#include "core/host_path.h"
#include <toml++/toml.hpp>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>

namespace app::tuning_profiles {
namespace {
struct Entry { uint64_t id = 0; Car car; bool enabled = false; Settings::Tuning tuning; };
struct Career { uint64_t next = 1; std::string label; std::vector<Entry> cars; };
std::map<std::string, Career> g_careers;
std::string g_active, g_editor_key, g_status;
uint32_t g_selected = 0;
bool g_live = false, g_dirty = false, g_global = false, g_read_only = false, g_disk_exists = false;
bool g_enabled = true;
std::string g_disk;
Settings::Tuning g_editor;
constexpr size_t kMaxBytes = 4 * 1024 * 1024, kMaxCareers = 64;
struct Field { const char* name; float Settings::Tuning::*f; bool Settings::Tuning::*b; float lo, hi; };
constexpr Field fields[] = {
    {"garage_extended",nullptr,&Settings::Tuning::garage_extended,0,1},
    {"ride_height_front",&Settings::Tuning::ride_front,nullptr,-2,2}, {"ride_height_rear",&Settings::Tuning::ride_rear,nullptr,-2,2},
    {"wheel_size_limit",&Settings::Tuning::wheel_limit,nullptr,1,3},
    {"camber_front",&Settings::Tuning::camber_front,nullptr,-30,20}, {"camber_rear",&Settings::Tuning::camber_rear,nullptr,-30,20},
    {"offset_front",&Settings::Tuning::offset_front,nullptr,-.3f,.3f}, {"offset_rear",&Settings::Tuning::offset_rear,nullptr,-.3f,.3f},
    {"spring_front",&Settings::Tuning::spring_front,nullptr,.25f,3}, {"spring_rear",&Settings::Tuning::spring_rear,nullptr,.25f,3},
    {"damper_front",&Settings::Tuning::damper_front,nullptr,.25f,3}, {"damper_rear",&Settings::Tuning::damper_rear,nullptr,.25f,3},
    {"brake_front",&Settings::Tuning::brake_front,nullptr,.25f,3}, {"brake_rear",&Settings::Tuning::brake_rear,nullptr,.25f,3},
    {"grip_front",&Settings::Tuning::grip_front,nullptr,.5f,2}, {"grip_rear",&Settings::Tuning::grip_rear,nullptr,.5f,2},
    {"engine_power",&Settings::Tuning::engine_power,nullptr,.5f,3},
    {"airbag_match_ride",nullptr,&Settings::Tuning::airbag_match_ride,0,1}, {"wheel_tuck",nullptr,&Settings::Tuning::wheel_tuck,0,1},
};
void Require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
bool Text(const std::string& s, size_t max) {
    return !s.empty() && s.size() <= max && std::all_of(s.begin(),s.end(),[](unsigned char c){return c >= 32 && c < 127;});
}
bool Valid(const Car& c) {
    return Text(c.model,32) && !c.signature.empty() && c.signature.size() <= 512 &&
           c.signature.size()%2 == 0 && std::all_of(c.signature.begin(),c.signature.end(),[](unsigned char x){return (x>='0'&&x<='9')||(x>='a'&&x<='f');});
}
void Sanitize(Settings::Tuning& t) {
    Settings::Tuning defaults;
    for (auto& f:fields) if (f.f) {
        auto& v=t.*f.f; v=std::isfinite(v)?std::clamp(v,f.lo,f.hi):defaults.*f.f;
    }
}
std::filesystem::path File() { return ExeDir()/"config"/"tuning_profiles.toml"; }
std::string Read(const std::filesystem::path& p) {
    const auto size=std::filesystem::file_size(hostpath::Native(p));
    Require(size <= kMaxBytes,"Profile file too large");
    std::ifstream in(hostpath::Native(p),std::ios::binary); Require(bool(in),"Cannot read profile file");
    std::string bytes(static_cast<size_t>(size),'\0');
    in.read(bytes.data(),std::streamsize(bytes.size()));
    Require(in.gcount()==std::streamsize(bytes.size()) && in.peek()==std::char_traits<char>::eof(),"Profile file changed during bounded read");
    return bytes;
}
Entry* Selected() {
    auto it=g_careers.find(g_active);
    return g_live && it!=g_careers.end() && g_selected<it->second.cars.size()? &it->second.cars[g_selected]:nullptr;
}
std::string Key(const std::string& career,const Entry& e) { return career+"#"+std::to_string(e.id); }
void Changed() { g_dirty=true; g_editor_key.clear(); }
Entry New(Career& c,const Car& car) {
    Require(c.next > 0 && c.next < uint64_t(INT64_MAX),"Profile identity space exhausted");
    Entry e; e.id=c.next++; e.car=car; return e;
}
void Keys(const toml::table& t,std::initializer_list<std::string_view> allowed) {
    for (auto&& [k,v]:t) Require(std::find(allowed.begin(),allowed.end(),k.str())!=allowed.end(),"Unknown profile field");
}
toml::table TuningTable(Settings::Tuning t) {
    Sanitize(t); toml::table out;
    for(auto& f:fields) if(f.f) out.insert(f.name,double(t.*f.f)); else out.insert(f.name,t.*f.b);
    return out;
}
Settings::Tuning ReadTuning(const toml::table& t) {
    Settings::Tuning out;
    for(auto&& [key,node]:t) {
        auto it=std::find_if(std::begin(fields),std::end(fields),[&](const Field& f){return key.str()==f.name;});
        Require(it!=std::end(fields),"Unknown tuning field");
        if(it->f) {auto n=node.value<double>(); Require(n.has_value() && std::isfinite(*n),"Non-finite tuning value");out.*it->f=float(*n);}
        else {auto n=node.value<bool>();Require(n.has_value(),"Invalid tuning toggle");out.*it->b=*n;}
    }
    Sanitize(out);return out;
}
}

void Load() {
    std::lock_guard lock(UiLock()); g_careers.clear();g_active.clear();g_editor_key.clear();g_live=false;g_dirty=false;
    g_global=false;g_enabled=true;g_read_only=false;g_disk.clear();g_disk_exists=false;g_status.clear();
    try {
        const auto p=File(); if(!std::filesystem::exists(hostpath::Native(p))) return;
        Require(!std::filesystem::is_symlink(p) && !(GetFileAttributesW(hostpath::Native(p).c_str())&FILE_ATTRIBUTE_REPARSE_POINT),"Linked profile file refused");
        g_disk_exists=true;g_disk=Read(p);auto t=toml::parse(g_disk);Keys(t,{"version","global_editing","profiles_enabled","career"});
        Require(t["version"].value<int64_t>()==1,"Unknown profile format version");
        auto mode=t["global_editing"].value<bool>();Require(mode.has_value(),"Invalid edit mode");
        const auto enabled=t.contains("profiles_enabled")?t["profiles_enabled"].value<bool>():std::optional<bool>{true};
        Require(enabled.has_value(),"Invalid per-car enable mode");
        std::map<std::string,Career> loaded;
        const auto* array=t["career"].as_array();Require(array && array->size()<=kMaxCareers,"Invalid career list");
        for(auto& node:*array) {
            const auto* item=node.as_table();Require(item,"Invalid career record");Keys(*item,{"key","label","next_id","cars"});
            auto key=(*item)["key"].value<std::string>();auto label=(*item)["label"].value<std::string>();auto next=(*item)["next_id"].value<int64_t>();
            Require(key && Text(*key,96) && label && Text(*label,32) && next && *next>0,"Invalid career identity");
            Career c;c.label=*label;c.next=uint64_t(*next);std::set<uint64_t> ids;
            auto cars=(*item)["cars"].as_array();Require(cars && cars->size()<=30,"Invalid owned-car list");
            for(auto& n:*cars) {
                auto row=n.as_table();Require(row,"Invalid car profile");Keys(*row,{"id","model","signature","enabled","tuning"});
                auto id=(*row)["id"].value<int64_t>();auto model=(*row)["model"].value<std::string>();auto sig=(*row)["signature"].value<std::string>();auto en=(*row)["enabled"].value<bool>();auto tune=(*row)["tuning"].as_table();
                Require(id && *id>0 && uint64_t(*id)<c.next && ids.insert(uint64_t(*id)).second && model && sig && en && tune,"Invalid car identity");
                Entry e;e.id=uint64_t(*id);e.car={*model,*sig};Require(Valid(e.car),"Invalid car signature");e.enabled=*en;e.tuning=ReadTuning(*tune);c.cars.push_back(e);
            }
            Require(loaded.emplace(*key,std::move(c)).second,"Duplicate career key");
        }
        g_careers=std::move(loaded);g_global=*mode;g_enabled=*enabled;
    } catch(const std::exception& e) {g_careers.clear();g_read_only=true;g_status=std::string("Profile file preserved: ")+e.what();}
}
bool Save() {
    std::lock_guard lock(UiLock());if(g_read_only) return false;if(!g_dirty) return true;
    std::filesystem::path temp;
    bool owned_temp=false;
    try {
        auto p=File();bool exists=std::filesystem::exists(hostpath::Native(p));
        Require(exists==g_disk_exists && (!exists || Read(p)==g_disk),"Profile file changed outside the game; preserved");
        toml::table t{{"version",1},{"global_editing",g_global},{"profiles_enabled",g_enabled}};toml::array career;
        for(auto& [key,c]:g_careers) {
            toml::table item{{"key",key},{"label",c.label},{"next_id",int64_t(c.next)}};toml::array cars;
            for(auto& e:c.cars) cars.push_back(toml::table{{"id",int64_t(e.id)},{"model",e.car.model},{"signature",e.car.signature},{"enabled",e.enabled},{"tuning",TuningTable(e.tuning)}});
            item.insert("cars",std::move(cars));career.push_back(std::move(item));
        }
        t.insert("career",std::move(career));std::ostringstream out;out<<t;std::string bytes=out.str();Require(bytes.size()<=kMaxBytes,"Profile metadata exceeds safe limit");
        std::filesystem::create_directories(hostpath::Native(p.parent_path()));static std::atomic<uint64_t> serial;
        temp=p;temp+=L".new-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(++serial);
        HANDLE f=CreateFileW(hostpath::Native(temp).c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);Require(f!=INVALID_HANDLE_VALUE,"Cannot create profile temporary");
        owned_temp=true;
        DWORD written=0;bool good=WriteFile(f,bytes.data(),DWORD(bytes.size()),&written,nullptr)&&written==bytes.size()&&FlushFileBuffers(f);CloseHandle(f);Require(good,"Cannot flush profile temporary");
        Require(std::filesystem::exists(hostpath::Native(p))==g_disk_exists && (!g_disk_exists || Read(p)==g_disk),"Profile file changed while saving; preserved");
        Require(MoveFileExW(hostpath::Native(temp).c_str(),hostpath::Native(p).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=0,"Cannot publish profile metadata");
        owned_temp=false;temp.clear();g_disk=std::move(bytes);g_disk_exists=true;g_dirty=false;return true;
    } catch(const std::exception& e) {g_status=e.what();if(g_status.find("changed")!=std::string::npos)g_read_only=true;if(owned_temp&&!temp.empty()){std::error_code ec;std::filesystem::remove(hostpath::Native(temp),ec);}return false;}
}
bool Dirty(){std::lock_guard lock(UiLock());return g_dirty;}
View Snapshot(){std::lock_guard lock(UiLock());View v;v.global_editing=g_global;v.profiles_enabled=g_enabled;v.storage_read_only=g_read_only;v.status=g_status;auto* e=Selected();if(e){v.ready=true;v.override_enabled=e->enabled;v.profile=g_careers[g_active].label;v.model=e->car.model;v.slot=g_selected;v.key=Key(g_active,*e);}return v;}
Settings::Tuning EffectiveForKey(const std::string& key){std::lock_guard lock(UiLock());if(!g_enabled)return Get().tuning;for(auto& [career,c]:g_careers)for(auto& e:c.cars)if(Key(career,e)==key)return e.enabled?e.tuning:Get().tuning;return Get().tuning;}
Settings::Tuning Effective(){std::lock_guard lock(UiLock());auto* e=Selected();return g_enabled&&e&&e->enabled?e->tuning:Get().tuning;}
Settings::Tuning& Editor(){if(g_global)return Get().tuning;auto v=Snapshot();const auto context=v.key+"/car";if(g_editor_key!=context){g_editor=Effective();g_editor_key=context;}return g_editor;}
bool CommitEditor(){std::lock_guard lock(UiLock());if(g_read_only)return false;if(g_global){Sanitize(Get().tuning);g_dirty=true;return true;}auto* e=Selected();if(!e)return false;Sanitize(g_editor);e->tuning=g_editor;e->enabled=true;g_dirty=true;return true;}
bool SetGlobalEditing(bool enabled){std::lock_guard lock(UiLock());if(g_global==enabled)return false;g_global=enabled;Changed();return true;}
bool SetProfilesEnabled(bool enabled){std::lock_guard lock(UiLock());if(g_read_only||g_enabled==enabled)return false;g_enabled=enabled;Changed();return true;}
bool CreateOverride(){std::lock_guard lock(UiLock());auto* e=Selected();if(!e||g_read_only)return false;e->tuning=Get().tuning;e->enabled=true;Changed();return true;}
bool UseGlobal(){std::lock_guard lock(UiLock());auto* e=Selected();if(!e||g_read_only)return false;e->enabled=false;Changed();return true;}
bool DeleteOverride(){std::lock_guard lock(UiLock());auto* e=Selected();if(!e||g_read_only)return false;e->enabled=false;e->tuning={};Changed();return true;}
bool ResetOverride(){std::lock_guard lock(UiLock());auto* e=Selected();if(!e||g_read_only)return false;e->tuning={};e->enabled=true;Changed();return true;}
void ClearSelection(){std::lock_guard lock(UiLock());g_active.clear();g_live=false;g_editor_key.clear();}
std::string KeyForSlot(uint32_t slot){std::lock_guard lock(UiLock());auto it=g_careers.find(g_active);return g_live&&it!=g_careers.end()&&slot<it->second.cars.size()?Key(g_active,it->second.cars[slot]):std::string{};}
void Observe(const std::string& career,const std::string& label,const std::vector<Car>& cars,uint32_t selected){
    std::lock_guard lock(UiLock());if(!Text(career,96)||!Text(label,32)||cars.empty()||cars.size()>30||selected>=cars.size()||!std::all_of(cars.begin(),cars.end(),Valid)){ClearSelection();return;}
    if(!g_careers.count(career)&&g_careers.size()>=kMaxCareers){ClearSelection();g_status="PC profile limit reached; using global tuning";return;}
    try {
        const auto old=g_careers.find(career);Career c=old==g_careers.end()?Career{}:old->second;
        bool live=g_live&&g_active==career;
        bool exact=c.cars.size()==cars.size();if(exact)for(size_t i=0;i<cars.size();++i)exact&=c.cars[i].car==cars[i];
        if((!live&&!exact)||(live&&cars.size()<c.cars.size())){c.cars.clear();g_status="Loaded roster changed outside an observed purchase/sale; using new car identities";Changed();}
        for(size_t i=0;i<cars.size();++i){if(i>=c.cars.size()){c.cars.push_back(New(c,cars[i]));Changed();}else if(c.cars[i].car.model!=cars[i].model){c.cars[i]=New(c,cars[i]);Changed();}else if(c.cars[i].car!=cars[i]){c.cars[i].car=cars[i];g_dirty=true;}}
        c.label=label;g_careers.insert_or_assign(career,std::move(c));
        if(g_active!=career||g_selected!=selected)g_editor_key.clear();g_active=career;g_selected=selected;g_live=true;
    } catch(const std::exception& e) {
        // Exhausted/corrupt metadata must not throw through a game update or
        // leave a partially assigned roster to be published on the next save.
        ClearSelection();g_read_only=true;g_status=std::string("Profile metadata preserved: ")+e.what();
    }
}
bool Removed(const std::string& career,uint32_t slot,const std::vector<Car>& before,const std::vector<Car>& after,uint32_t selected){
    std::lock_guard lock(UiLock());auto it=g_careers.find(career);if(!g_live||g_active!=career||it==g_careers.end()||slot>=before.size()||before.size()!=after.size()+1||it->second.cars.size()!=before.size())return false;
    auto expected=before;expected.erase(expected.begin()+slot);if(expected!=after)return false;for(size_t i=0;i<before.size();++i)if(it->second.cars[i].car!=before[i])return false;
    it->second.cars.erase(it->second.cars.begin()+slot);g_selected=selected;Changed();if(after.empty())g_live=false;return true;
}
}
