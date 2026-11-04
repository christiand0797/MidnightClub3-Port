// Pure updater policy/protocol and Windows command-line parsing. No helper, download, installation or game launch.
#include "app/updates.h"
#include <cassert>
#include <iostream>
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#endif

int main(){
    using namespace app::updates;
    assert(QuoteArgument(L"")==L"\"\"");
    assert(QuoteArgument(L"C:\\game path\\")==L"\"C:\\game path\\\\\"");
#ifdef _WIN32
    const std::wstring samples[]={L"",L"C:\\Games\\MC3 & Other's\\",L"a\"b",L"\\\\server\\quoted\\",L"; $(Get-Item *) & | >",L"C:\\unicode é中\\file.ps1",L"trailing\\\\",L"x\\\\\" y"};
    for(const auto& value:samples){
        const std::wstring command=QuoteArgument(L"powershell.exe")+L" "+QuoteArgument(value);
        int count=0;LPWSTR* parsed=CommandLineToArgvW(command.c_str(),&count);
        assert(parsed&&count==2&&std::wstring(parsed[1])==value);LocalFree(parsed);
    }
#endif
    constexpr std::string_view available=R"({"schema":1,"status":"available","release_id":42,"tag":"v0.4","source_revision":"abcdef0","latest_source_revision":"abcdef1","source_ahead":true,"message":"A release is ready."})";
    auto result=ParseResult(available,Operation::Check);assert(result&&result->release_id==42&&result->status=="available");
    assert(!ParseResult(available,Operation::Stage));
    assert(!ParseResult(R"({"schema":1,"status":"available","release_id":0,"tag":"v1","source_revision":"abcdef0"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"available","release_id":9223372036854775808,"tag":"v1","source_revision":"abcdef0"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"schema":1,"status":"error","message":"duplicate"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":"bad\u0000tail"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":"bad\ud800"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":"bad\udc00"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":{},"tag":"nested"})",Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":["nested"]})",Operation::Check));
    assert(!ParseResult(R"({"schema":2,"status":"error","message":"unknown protocol"})",Operation::Check));
    assert(!ParseResult(std::string(65537,' '),Operation::Check));
    assert(!ParseResult(R"({"schema":1,"status":"error","message":"ok"} trailing)",Operation::Check));
    auto unicode=ParseResult(R"({"schema":1,"status":"error","message":"Saved \u00e9 \ud83d\ude97"})",Operation::Check);
    assert(unicode&&unicode->message=="Saved é 🚗");
    auto source=ParseResult(R"({"schema":1,"status":"source_only","release_id":0,"tag":"","source_revision":"","latest_source_revision":"abcdef123456","source_ahead":true,"message":"Awaiting a runnable build."})",Operation::Check);
    assert(source&&source->release_id==0);
    assert(!ParseResult(R"({"schema":1,"status":"source_only","latest_source_revision":"not-a-revision"})",Operation::Check));
    auto staged=ParseResult(R"({"schema":1,"status":"staged","release_id":42,"tag":"v0.4","source_revision":"abcdef0","stage_directory":"C:\\Updates\\stage","message":"Nothing installed."})",Operation::Stage);
    assert(staged&&staged->stage_directory=="C:\\Updates\\stage");
    assert(!ParseResult(R"({"schema":1,"status":"staged","release_id":42,"tag":"v0.4","source_revision":"abcdef0"})",Operation::Stage));

    Model disabled;assert(!disabled.Begin(Operation::Check)&&!disabled.Begin(Operation::Stage)&&!disabled.Begin(Operation::Apply));
    Model model(true);
    assert(!model.Begin(Operation::Stage)&&!model.Begin(Operation::Apply));
    const auto check=model.Begin(Operation::Check);assert(check&&model.snapshot().busy);
    assert(!model.Begin(Operation::Check)&&!model.Begin(Operation::Stage));
    assert(!model.Finish(*check+1,Operation::Check,*result));
    assert(!model.Finish(*check,Operation::Stage,*result));
    assert(model.Finish(*check,Operation::Check,*result));
    assert(model.snapshot().state==State::Available&&model.snapshot().notification&&!model.snapshot().busy);
    assert(model.snapshot().stage_directory.empty()); // Check cannot stage or install.
    model.DismissNotification();assert(!model.snapshot().notification);
    const auto download=model.Begin(Operation::Stage);assert(download&&model.snapshot().state==State::Downloading);
    assert(!model.Begin(Operation::Apply)&&!model.Begin(Operation::Check));
    assert(!model.Finish(*download,Operation::Stage,*staged)); // missing stage cannot authorize Apply
    auto wrong=*staged;wrong.release_id=43;
    assert(!model.Finish(*download,Operation::Stage,wrong,L"C:\\Updates\\stage"));
    assert(model.Finish(*download,Operation::Stage,*staged,L"C:\\Updates\\stage"));
    assert(model.snapshot().state==State::Staged&&!model.snapshot().busy);
    auto again=model.Begin(Operation::Check);assert(again);
    assert(model.Finish(*again,Operation::Check,*result));assert(model.snapshot().state==State::Staged); // reuse same verified stage
    const auto apply=model.Begin(Operation::Apply);assert(apply);
    assert(!model.Begin(Operation::Check));
    Result scheduled;scheduled.status="scheduled";
    assert(model.Finish(*apply,Operation::Apply,scheduled));
    assert(model.snapshot().state==State::Scheduled&&!model.snapshot().busy);
    assert(!model.Begin(Operation::Apply)&&!model.Begin(Operation::Stage)&&!model.Begin(Operation::Check));

    Model commits(true);auto token=commits.Begin(Operation::Check);assert(token);
    assert(commits.Finish(*token,Operation::Check,*source));
    assert(commits.snapshot().state==State::SourceOnly&&commits.snapshot().notification);
    assert(!commits.Begin(Operation::Stage)&&!commits.Begin(Operation::Apply));
    Model recovery(true);recovery.RecoverStaged(*staged,L"C:\\Updates\\stage");
    auto retry=recovery.Begin(Operation::Apply);assert(retry);
    Result error;error.status="error";error.message="Helper could not start; stage retained.";
    assert(recovery.Finish(*retry,Operation::Apply,error));
    assert(recovery.snapshot().state==State::Error&&!recovery.snapshot().stage_directory.empty());
    retry=recovery.Begin(Operation::Check);assert(retry);auto current=*result;current.status="current";
    assert(recovery.Finish(*retry,Operation::Check,current));assert(recovery.snapshot().stage_directory.empty());
    assert(!recovery.Begin(Operation::Apply));
    std::cout<<"updates: quoting, bounded helper protocol, explicit check/download/install and stale-result policy passed\n";
}
