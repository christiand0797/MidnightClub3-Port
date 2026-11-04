#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "app/updates.h"
#include "core/host_path.h"
#include "core/log.h"
#include <imgui.h>

#include <atomic>
#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace app::updates {
namespace {
namespace fs=std::filesystem;
struct Handle {
    HANDLE value=nullptr;
    ~Handle(){if(value&&value!=INVALID_HANDLE_VALUE)CloseHandle(value);}
    Handle()=default;Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
};
struct Runtime {
    std::mutex mutex;
    std::atomic<bool> stopping=false;
    Model model;
    fs::path install,helper,powershell,result_root;
    explicit Runtime(bool enabled):model(enabled){}
};
std::mutex g_mutex;
std::shared_ptr<Runtime> g_runtime;
bool g_started=false;

bool Ordinary(const fs::path& path,bool directory) {
    const auto text=path.native();
    if(text.size()<3||text.size()>32700||text[1]!=L':'||(text[2]!=L'\\'&&text[2]!=L'/')||
       !((text[0]>=L'A'&&text[0]<=L'Z')||(text[0]>=L'a'&&text[0]<=L'z')))return false;
    for(wchar_t c:text)if(c<32||c==127)return false;
    auto current=path;
    while(!current.empty()) {
        const DWORD attributes=GetFileAttributesW(current.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT))return false;
        const auto parent=current.parent_path();if(parent==current)break;current=parent;
    }
    const DWORD attributes=GetFileAttributesW(path.c_str());
    return attributes!=INVALID_FILE_ATTRIBUTES&&!(attributes&FILE_ATTRIBUTE_REPARSE_POINT)&&
        bool(attributes&FILE_ATTRIBUTE_DIRECTORY)==directory;
}
bool Packaged(const fs::path& install) {
    if(!Ordinary(install,true)||!Ordinary(install/L"mc3.exe",false)||
       !Ordinary(install/L"release-manifest.json",false)||!Ordinary(install/L"update"/L"Update-MC3.ps1",false))return false;
    // Source checkouts, including out/native and git worktrees, never auto-run a release updater.
    for(auto path=install;!path.empty();) {
        std::error_code ec;if(fs::exists(path/L".git",ec)||ec)return false;
        const auto parent=path.parent_path();if(parent==path)break;path=parent;
    }
    return true;
}
std::optional<std::string> ReadResult(const fs::path& path) {
    if(!Ordinary(path,false))return {};
    std::error_code ec;const auto size=fs::file_size(path,ec);
    if(ec||!size||size>65536)return {};
    std::ifstream input(path,std::ios::binary);std::string data(size,'\0');
    if(!input.read(data.data(),std::streamsize(size)))return {};
    if(input.peek()!=EOF)return {};return data;
}
std::string Unique() {
    GUID guid{};if(FAILED(CoCreateGuid(&guid)))throw std::runtime_error("Cannot create an update operation identity.");
    wchar_t text[40]{};if(!StringFromGUID2(guid,text,40))throw std::runtime_error("Cannot format update identity.");
    std::string out;for(wchar_t c:text)if((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f')||(c>=L'A'&&c<=L'F'))out+=char(c);
    if(out.size()!=32)throw std::runtime_error("Invalid update operation identity.");return out;
}
fs::path LocalResults(const fs::path& install) {
    const DWORD size=GetEnvironmentVariableW(L"LOCALAPPDATA",nullptr,0);
    if(size<4||size>32700)throw std::runtime_error("Local application-data directory is unavailable.");
    std::wstring text(size,L'\0');const DWORD read=GetEnvironmentVariableW(L"LOCALAPPDATA",text.data(),size);
    if(!read||read>=size)throw std::runtime_error("Local application-data directory changed.");text.resize(read);
    uint64_t identity=1469598103934665603ull;
    for(wchar_t c:install.native()){if(c>=L'A'&&c<=L'Z')c+=L'a'-L'A';identity=(identity^uint16_t(c))*1099511628211ull;}
    const auto root=fs::path(text)/L"MidnightClub3-Port"/L"updates"/L"ui"/std::to_wstring(identity);
    // Reject an existing reparse point before creating children. A nonexistent path is checked through its parents.
    auto parent=root;
    while(!parent.empty()) {const DWORD a=GetFileAttributesW(parent.c_str());
        if(a!=INVALID_FILE_ATTRIBUTES&&(a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("Update metadata path is a link.");
        const auto next=parent.parent_path();if(next==parent)break;parent=next;}
    std::error_code ec;fs::create_directories(root,ec);
    if(ec||!Ordinary(root,true))throw std::runtime_error("Cannot prepare the local update status directory.");return root;
}
fs::path Powershell() {
    std::wstring root(32768,L'\0');const UINT n=GetWindowsDirectoryW(root.data(),UINT(root.size()));
    if(!n||n>=root.size())return {};root.resize(n);
    const auto path=fs::path(root)/L"System32"/L"WindowsPowerShell"/L"v1.0"/L"powershell.exe";
    return Ordinary(path,false)?path:fs::path{};
}
void PersistStage(const Runtime& runtime,std::string_view json) {
    const fs::path target=runtime.result_root/L"last-stage.json";
    const fs::path temporary=runtime.result_root/(L"stage-"+hostpath::FromUtf8(Unique()).native()+L".tmp");
    Handle file;file.value=CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file.value==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot preserve staged-update status.");
    DWORD wrote=0;
    if(!WriteFile(file.value,json.data(),DWORD(json.size()),&wrote,nullptr)||wrote!=json.size()||!FlushFileBuffers(file.value))
        throw std::runtime_error("Cannot flush staged-update status.");
    CloseHandle(file.value);file.value=nullptr;
    if(!MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("Cannot publish staged-update status; the downloaded stage was preserved.");
}
std::shared_ptr<Runtime> RuntimeCopy(){std::lock_guard lock(g_mutex);return g_runtime;}
void Finish(const std::shared_ptr<Runtime>& runtime,uint64_t token,Operation operation,Result result,const fs::path& stage={}) {
    std::lock_guard lock(runtime->mutex);
    runtime->model.Finish(token,operation,result,stage);
}

void Work(const std::shared_ptr<Runtime>& runtime,uint64_t token,Operation operation,Snapshot selected) noexcept {
    try {
        if(runtime->stopping.load())return;
        // Recheck the fixed local package/helper immediately before launching; no URL/command from JSON is run.
        if(!Packaged(runtime->install)||!Ordinary(runtime->powershell,false))throw std::runtime_error("Packaged updater is unavailable; development installs are not updated automatically.");
        if(runtime->result_root.empty())runtime->result_root=LocalResults(runtime->install); // only this one worker writes
        const fs::path result=runtime->result_root/(hostpath::FromUtf8(Unique()).native()+L".json");
        std::vector<std::wstring> args={runtime->powershell.native(),L"-NoLogo",L"-NoProfile",L"-NonInteractive",
            L"-ExecutionPolicy",L"Bypass",L"-File",runtime->helper.native(),L"-Mode",
            operation==Operation::Check?L"Check":operation==Operation::Stage?L"Stage":L"Apply",
            L"-InstallDirectory",runtime->install.native(),L"-ResultPath",result.native()};
        if(operation==Operation::Stage){args.emplace_back(L"-ReleaseId");args.push_back(std::to_wstring(selected.release_id));}
        if(operation==Operation::Apply){
            if(!Ordinary(selected.stage_directory,true))throw std::runtime_error("The selected staged directory is unavailable.");
            args.emplace_back(L"-StageDirectory");args.push_back(selected.stage_directory.native());
            args.emplace_back(L"-ParentProcessId");args.push_back(std::to_wstring(GetCurrentProcessId()));
        }
        std::wstring command;
        for(const auto& argument:args){if(argument.find(L'\0')!=argument.npos)throw std::runtime_error("Invalid updater argument.");
            if(!command.empty())command+=L' ';command+=QuoteArgument(argument);}
        if(command.size()>32760)throw std::runtime_error("Updater command exceeds Windows' argument bound.");
        STARTUPINFOW startup{sizeof(startup)};startup.dwFlags=STARTF_USESHOWWINDOW;startup.wShowWindow=SW_HIDE;
        PROCESS_INFORMATION child{};
        if(!CreateProcessW(runtime->powershell.c_str(),command.data(),nullptr,nullptr,FALSE,
            CREATE_NO_WINDOW|CREATE_UNICODE_ENVIRONMENT|BELOW_NORMAL_PRIORITY_CLASS,nullptr,runtime->install.c_str(),&startup,&child))
            throw std::runtime_error("Cannot launch the packaged updater.");
        Handle process;process.value=child.hProcess;CloseHandle(child.hThread);
        LOGR("updates: %s helper started; result %s",operation==Operation::Check?"check":operation==Operation::Stage?"stage":"apply",hostpath::Utf8(result).c_str());
        if(operation==Operation::Apply){
            // Child waits for this actual PID. Closing our HANDLE does not terminate it; no quit/kill is requested.
            Result scheduled;scheduled.status="scheduled";Finish(runtime,token,operation,std::move(scheduled));return;
        }
        const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(operation==Operation::Check?180:1800);
        bool slow_reported=false;
        for(;;){
            if(runtime->stopping.load())return; // release handle, leave in-flight child/download untouched
            const DWORD wait=WaitForSingleObject(process.value,250);
            if(wait==WAIT_OBJECT_0)break;
            if(wait!=WAIT_TIMEOUT)throw std::runtime_error("Cannot observe updater completion; the helper was left running.");
            if(!slow_reported&&std::chrono::steady_clock::now()>=deadline){
                std::lock_guard lock(runtime->mutex);
                runtime->model.Progress(token,operation,"Updater is taking longer than expected. Its work is preserved; you can keep playing.");
                slow_reported=true;
                // Keep this ONE worker observing its child. Starting another operation while it still runs would
                // duplicate work. Shutdown still returns immediately; this wait polls its stop flag every 250 ms.
            }
        }
        DWORD exit=1;if(!GetExitCodeProcess(process.value,&exit))throw std::runtime_error("Cannot read updater exit status.");
        auto json=ReadResult(result);auto parsed=json?ParseResult(*json,operation):std::nullopt;
        if(!parsed)throw std::runtime_error("Updater returned no valid bounded status. Its files were preserved.");
        if(exit!=0&&parsed->status!="error")throw std::runtime_error("Updater did not complete successfully.");
        fs::path staged;
        if(operation==Operation::Stage&&parsed->status=="staged"){
            if(parsed->release_id!=selected.release_id)throw std::runtime_error("Downloaded release differs from the selected update.");
            staged=hostpath::FromUtf8(parsed->stage_directory);
            if(!Ordinary(staged,true)||!Ordinary(staged/L"stage.json",false))throw std::runtime_error("Staged-update receipt is unavailable.");
            PersistStage(*runtime,*json);
        }
        Finish(runtime,token,operation,std::move(*parsed),staged);
    }catch(const std::exception& error){
        Result failed;failed.status="error";failed.message=std::string(error.what()).substr(0,2048);
        Finish(runtime,token,operation,std::move(failed));
    }catch(...){Result failed;failed.status="error";failed.message="Updater could not finish. Existing files were preserved.";Finish(runtime,token,operation,std::move(failed));}
}
bool Begin(Operation operation) {
    auto runtime=RuntimeCopy();if(!runtime||runtime->stopping.load())return false;
    std::optional<uint64_t> token;Snapshot selected;
    {std::lock_guard lock(runtime->mutex);selected=runtime->model.snapshot();token=runtime->model.Begin(operation);}
    if(!token)return false;
    try{std::thread([runtime,operation,selected=std::move(selected),token=*token]{Work(runtime,token,operation,selected);}).detach();return true;}
    catch(...){Result failed;failed.status="error";failed.message="Cannot start the update worker.";Finish(runtime,*token,operation,std::move(failed));return false;}
}
} // namespace

void Startup(const fs::path& install_directory) {
    std::shared_ptr<Runtime> runtime;
    {std::lock_guard lock(g_mutex);if(g_started)return;g_started=true;
        std::error_code ec;auto install=fs::absolute(install_directory,ec).lexically_normal();
        const bool packaged=!ec&&Packaged(install);
        runtime=std::make_shared<Runtime>(packaged);runtime->install=install;
        runtime->helper=install/L"update"/L"Update-MC3.ps1";runtime->powershell=Powershell();g_runtime=runtime;}
    if(runtime->model.snapshot().state==State::Disabled)return;
    // Local bounded recovery is independent of preferences/saves. The helper revalidates the stage before Apply.
    try{runtime->result_root=LocalResults(runtime->install);
        if(auto saved=ReadResult(runtime->result_root/L"last-stage.json"))if(auto parsed=ParseResult(*saved,Operation::Stage)){
            const auto staged=hostpath::FromUtf8(parsed->stage_directory);
            if(Ordinary(staged,true)&&Ordinary(staged/L"stage.json",false))runtime->model.RecoverStaged(*parsed,staged);
        }
    }catch(...){ /* A check can report the precise local failure on its background worker. */ }
    CheckNow();
}
void Shutdown(){if(auto runtime=RuntimeCopy())runtime->stopping.store(true);}
Snapshot GetSnapshot(){auto runtime=RuntimeCopy();if(!runtime)return {};std::lock_guard lock(runtime->mutex);return runtime->model.snapshot();}
bool CheckNow(){return Begin(Operation::Check);}
bool Download(){return Begin(Operation::Stage);}
bool InstallAfterExit(){return Begin(Operation::Apply);}

void DrawMenu() {
    auto runtime=RuntimeCopy();if(!runtime)return;
    Snapshot snapshot;
    {std::lock_guard lock(runtime->mutex);runtime->model.DismissNotification();snapshot=runtime->model.snapshot();}
    ImGui::Separator();ImGui::TextUnformatted("Game updates");
    if(snapshot.state==State::Disabled){ImGui::TextDisabled("Automatic updates are available in packaged releases.");return;}
    if(!snapshot.tag.empty())ImGui::Text("Published release: %s",snapshot.tag.c_str());
    if(snapshot.state==State::SourceOnly&&!snapshot.latest_source_revision.empty())
        ImGui::Text("New source revision: %.12s",snapshot.latest_source_revision.c_str());
    if(!snapshot.message.empty())ImGui::TextWrapped("%s",snapshot.message.c_str());
    ImGui::BeginDisabled(snapshot.busy||snapshot.state==State::Scheduled);
    if(ImGui::Button("Check now"))CheckNow();
    ImGui::EndDisabled();
    if(snapshot.state==State::Available){ImGui::SameLine();if(ImGui::Button("Download update"))Download();}
    if(!snapshot.busy&&!snapshot.stage_directory.empty()&&snapshot.state!=State::Scheduled){
        if(ImGui::Button("Install after game closes"))InstallAfterExit();
        ImGui::TextWrapped("The updater waits for the game and this installation's Make HD jobs. Close the game normally when you are ready.");
    }
    if(snapshot.state==State::SourceOnly)ImGui::TextDisabled("New source changes are not an installable runtime update.");
    ImGui::TextWrapped("Checking never downloads or installs. Your saves, preferences and generated game content are kept.");
}
void DrawNotification() {
    const auto snapshot=GetSnapshot();if(!snapshot.notification)return;
    const ImGuiViewport* viewport=ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x+viewport->WorkSize.x-16,viewport->WorkPos.y+16),ImGuiCond_Always,ImVec2(1,0));
    ImGui::SetNextWindowBgAlpha(0.85f);
    constexpr ImGuiWindowFlags flags=ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoDecoration|
        ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoInputs|ImGuiWindowFlags_NoNav;
    if(ImGui::Begin("##mc3-update-notice",nullptr,flags)){
        if(snapshot.state==State::SourceOnly){
            ImGui::TextUnformatted("New MC3 source changes are available.");
            ImGui::TextUnformatted("Awaiting a published build. See F1 > Info.");
        }else{
            ImGui::Text("MC3 update available: %s",snapshot.tag.c_str());
            ImGui::TextUnformatted("Open F1 > Info to choose whether to update.");
        }
    }ImGui::End();
}

} // namespace app::updates
