#pragma once
// Player-controlled runtime updates. Checks only notify; downloading and installation are separate actions.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace app::updates {

enum class Operation { Check, Stage, Apply };
enum class State { Disabled, Idle, Checking, Current, SourceOnly, Available, Downloading, Staged, Scheduled, Error };
struct Snapshot {
    State state=State::Disabled;
    bool busy=false,notification=false;
    uint64_t release_id=0;
    std::string tag,message,source_revision,latest_source_revision;
    std::filesystem::path stage_directory;
};
struct Result {
    std::string status,tag,message,source_revision,latest_source_revision;
    uint64_t release_id=0;
    std::string stage_directory; // UTF-8 from the helper; native conversion only at the Windows boundary.
};

// Standard CommandLineToArgvW/CRT argument quoting. Every argument is quoted; shell operators have no meaning
// because the caller supplies an absolute powershell.exe application and uses -File, never -Command/cmd.exe.
inline std::wstring QuoteArgument(std::wstring_view argument) {
    std::wstring out=L"\"";size_t slashes=0;
    for(wchar_t c:argument) {
        if(c==L'\\'){++slashes;continue;}
        if(c==L'\"'){out.append(slashes*2+1,L'\\');out+=c;slashes=0;continue;}
        out.append(slashes,L'\\');slashes=0;out+=c;
    }
    out.append(slashes*2,L'\\');out+=L'\"';return out;
}

namespace detail {
// Strict bounded JSON object reader for the helper's flat result protocol. No arrays, nested objects,
// duplicate keys or executable text. Boolean metadata is accepted as a scalar. Unicode escapes (including
// surrogate pairs) become UTF-8.
class Json {
public:
    explicit Json(std::string_view input):input_(input){}
    std::optional<std::map<std::string,std::string>> Read() {
        if(input_.empty()||input_.size()>65536)return {};
        if(input_.starts_with("\xef\xbb\xbf"))at_=3;
        Space();if(!Take('{'))return {};
        std::map<std::string,std::string> values;
        Space();if(Take('}')){Space();return at_==input_.size()?std::optional(values):std::nullopt;}
        for(size_t fields=0;fields<16;++fields){
            Space();auto key=String();if(!key||key->size()>64)return {};Space();if(!Take(':'))return {};Space();
            std::optional<std::string> value;
            if(at_<input_.size()&&input_[at_]=='"')value=String();
            else if(input_.substr(at_,4)=="true"){at_+=4;value="true";}
            else if(input_.substr(at_,5)=="false"){at_+=5;value="false";}
            else{const size_t begin=at_;while(at_<input_.size()&&input_[at_]>='0'&&input_[at_]<='9')++at_;
                if(begin==at_||at_-begin>19)return {};value=std::string(input_.substr(begin,at_-begin));}
            if(!value||value->size()>8192||!values.emplace(*key,*value).second)return {};
            Space();if(Take('}')){Space();return at_==input_.size()?std::optional(values):std::nullopt;}
            if(!Take(','))return {};
        }return {};
    }
private:
    std::string_view input_;size_t at_=0;
    void Space(){while(at_<input_.size()&&(input_[at_]==' '||input_[at_]=='\r'||input_[at_]=='\n'||input_[at_]=='\t'))++at_;}
    bool Take(char c){if(at_<input_.size()&&input_[at_]==c){++at_;return true;}return false;}
    std::optional<uint32_t> Hex(){uint32_t v=0;for(unsigned n=0;n<4;++n){if(at_==input_.size())return {};
        const char c=input_[at_++];uint32_t d=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:16;
        if(d==16)return {};v=v*16+d;}return v;}
    static void Utf8(std::string& out,uint32_t c){
        if(c<0x80)out+=char(c);
        else if(c<0x800){out+=char(0xc0|(c>>6));out+=char(0x80|(c&63));}
        else if(c<0x10000){out+=char(0xe0|(c>>12));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else{out+=char(0xf0|(c>>18));out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    std::optional<std::string> String(){
        if(!Take('"'))return {};std::string out;
        while(at_<input_.size()){
            const unsigned char c=input_[at_++];if(c=='"')return out;if(c<0x20)return {};
            if(c!='\\'){out+=char(c);continue;}
            if(at_==input_.size())return {};const char escaped=input_[at_++];
            switch(escaped){
            case '"':case '\\':case '/':out+=escaped;break;
            case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
            case 'u':{auto code=Hex();if(!code)return {};
                if(*code>=0xd800&&*code<=0xdbff){if(!Take('\\')||!Take('u'))return {};auto low=Hex();
                    if(!low||*low<0xdc00||*low>0xdfff)return {};*code=0x10000+((*code-0xd800)<<10)+(*low-0xdc00);}
                else if(*code>=0xdc00&&*code<=0xdfff)return {};
                if(*code==0)return {};Utf8(out,*code);break;}
            default:return {};
            }
        }return {};
    }
};
inline bool Revision(std::string_view value){if(value.size()<7||value.size()>40)return false;
    for(char c:value)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F')))return false;return true;}
inline bool Plain(std::string_view value,size_t maximum){if(value.size()>maximum)return false;
    for(unsigned char c:value)if(c<32||c==127)return false;return true;}
} // namespace detail

inline std::optional<Result> ParseResult(std::string_view json,Operation operation) {
    auto fields=detail::Json(json).Read();if(!fields)return {};
    const auto get=[&](const char* name)->std::string{auto it=fields->find(name);return it==fields->end()?std::string{}:it->second;};
    if(get("schema")!="1")return {};
    Result out;out.status=get("status");out.tag=get("tag");out.message=get("message");out.source_revision=get("source_revision");
    out.latest_source_revision=get("latest_source_revision");
    if(!out.latest_source_revision.empty()&&!detail::Revision(out.latest_source_revision))return {};
    if(out.message.size()>2048)return {};
    if(out.status=="error"){if(out.message.empty())out.message="The update helper reported an error.";return out;}
    if(operation==Operation::Check&&out.status=="source_only"){
        if(!detail::Plain(out.tag,128)||(!out.source_revision.empty()&&!detail::Revision(out.source_revision)))return {};
        // A source repository can advance before it has ANY runnable release. Never manufacture a Stage ID.
        return out;
    }
    const std::string number=get("release_id");
    if(number.empty())return {};
    auto [end,error]=std::from_chars(number.data(),number.data()+number.size(),out.release_id);
    if(error!=std::errc{}||end!=number.data()+number.size()||!out.release_id||out.release_id>INT64_MAX)return {};
    if(!detail::Plain(out.tag,128)||out.tag.empty()||!detail::Revision(out.source_revision))return {};
    if(operation==Operation::Check){if(out.status!="current"&&out.status!="available"&&out.status!="source_only")return {};}
    else if(operation==Operation::Stage){
        if(out.status!="staged")return {};out.stage_directory=get("stage_directory");
        if(!detail::Plain(out.stage_directory,32700)||out.stage_directory.empty())return {};
    }else if(out.status!="applied"&&out.status!="already_applied"&&out.status!="rolled_back"&&out.status!="scheduled")return {};
    return out;
}

// Pure transition policy. Only explicit Stage/Apply calls authorize their actions; Check cannot progress into a
// download/install. Generation tokens reject stale completion. Integration serializes this model under its OWN lock.
class Model {
public:
    explicit Model(bool packaged=false){snapshot_.state=packaged?State::Idle:State::Disabled;}
    const Snapshot& snapshot()const{return snapshot_;}
    std::optional<uint64_t> Begin(Operation operation){
        if(snapshot_.state==State::Disabled||snapshot_.busy||snapshot_.state==State::Scheduled)return {};
        if(operation==Operation::Stage&&(snapshot_.state!=State::Available||!snapshot_.release_id))return {};
        if(operation==Operation::Apply&&snapshot_.stage_directory.empty())return {};
        snapshot_.busy=true;snapshot_.notification=false;operation_=operation;++generation_;
        snapshot_.state=operation==Operation::Check?State::Checking:operation==Operation::Stage?State::Downloading:State::Scheduled;
        snapshot_.message=operation==Operation::Check?"Checking published releases...":operation==Operation::Stage?
            "Downloading and verifying the selected release...":"Scheduling installation after this game closes...";
        return generation_;
    }
    bool Finish(uint64_t token,Operation operation,const Result& result,const std::filesystem::path& stage={}){
        if(!snapshot_.busy||token!=generation_||operation_!=operation)return false;
        if(result.status!="error"){
            if(operation==Operation::Check&&result.status!="current"&&result.status!="available"&&result.status!="source_only")return false;
            if(operation==Operation::Stage&&(result.status!="staged"||stage.empty()||result.release_id!=snapshot_.release_id))return false;
            if(operation==Operation::Apply&&result.status!="scheduled")return false;
        }
        snapshot_.busy=false;
        if(result.status=="error"){snapshot_.state=State::Error;snapshot_.message=result.message;return true;}
        if(operation==Operation::Apply){snapshot_.state=State::Scheduled;snapshot_.message="Update scheduled. Close the game when you are ready; it will then install.";return true;}
        snapshot_.release_id=result.release_id;snapshot_.tag=result.tag;snapshot_.source_revision=result.source_revision;
        snapshot_.latest_source_revision=result.latest_source_revision;
        snapshot_.message=result.message;
        if(operation==Operation::Stage){snapshot_.state=State::Staged;snapshot_.stage_directory=stage;staged_release_=result.release_id;snapshot_.notification=false;}
        else{
            const bool available=result.status=="available";
            snapshot_.state=available?State::Available:result.status=="source_only"?State::SourceOnly:State::Current;
            if(available&&staged_release_==result.release_id&&!snapshot_.stage_directory.empty())snapshot_.state=State::Staged;
            else{snapshot_.stage_directory.clear();staged_release_=0;}
            snapshot_.notification=available||result.status=="source_only";
        }
        return true;
    }
    void Progress(uint64_t token,Operation operation,std::string message){
        if(snapshot_.busy&&token==generation_&&operation_==operation)snapshot_.message=message.substr(0,2048);
    }
    void DismissNotification(){snapshot_.notification=false;}
    void RecoverStaged(const Result& result,const std::filesystem::path& stage){
        if(snapshot_.state==State::Disabled||snapshot_.busy||result.status!="staged"||stage.empty())return;
        snapshot_.release_id=result.release_id;snapshot_.tag=result.tag;snapshot_.source_revision=result.source_revision;
        snapshot_.stage_directory=stage;staged_release_=result.release_id;snapshot_.state=State::Staged;
        snapshot_.message="A verified download is staged. Installation still requires your choice.";
    }
private:
    Snapshot snapshot_;Operation operation_=Operation::Check;uint64_t generation_=0,staged_release_=0;
};

// Startup is once per process and auto-checks only packaged ordinary local installations. No global settings or
// original saves are read/written. DrawMenu/DrawNotification are the optional ImGui adapters; both are nonmodal.
void Startup(const std::filesystem::path& install_directory);
void Shutdown(); // Signal only: never waits for network/helper, kills a child or interrupts gameplay.
Snapshot GetSnapshot();
bool CheckNow();
bool Download();
bool InstallAfterExit();
void DrawMenu();
void DrawNotification(); // No input capture/focus; informs the player to open F1 > Info.

} // namespace app::updates
