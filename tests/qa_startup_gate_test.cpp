#include "app/qa_startup_gate.h"
#include "core/host_path.h"
#include <cassert>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <stdexcept>

namespace {
namespace fs = std::filesystem;
using namespace app::qa_startup;
constexpr auto kToken = "0123456789abcdef0123456789ABCDEF";

void WriteNew(const fs::path& path, std::string_view text) {
    assert(!fs::exists(path));
    std::ofstream out(path, std::ios::binary);
    out.write(text.data(), std::streamsize(text.size())); out.close(); assert(out);
}
std::string Bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

struct Fixture {
    fs::path parent = fs::weakly_canonical(fs::current_path());
    fs::path root = parent / ("qa-startup-fixture-" + std::to_string(Clock::now().time_since_epoch().count()));
    Config config{true, true, root/L"release-λ.txt", kToken};
    Clock::time_point time{};
    uint32_t sleeps = 0, waits = 0;
    std::function<void()> on_sleep = [] {};
    Fixture() { assert(fs::create_directory(root)); }
    ~Fixture() {
        assert(fs::weakly_canonical(root).parent_path() == parent && root.filename().string().starts_with("qa-startup-fixture-"));
        fs::remove_all(root);
    }
    template<class Reader> Result RunWith(Reader reader) {
        return Run(config, [&] { return time; }, [&](auto duration) {
            assert(duration > Clock::duration::zero() && duration <= kPoll);
            time += duration; ++sleeps; on_sleep();
        }, reader, [&] { ++waits; });
    }
    Result Go() { return RunWith(ReadRelease); }
    void Publish(std::string_view token = kToken) {
        const auto temporary = root/"new-token.tmp";
        WriteNew(temporary, token);
        fs::rename(temporary, config.release);  // A closed complete file appears in one rename.
    }
};

void InertAndBadConfig() {
    auto forbidden = []() -> Clock::time_point { assert(false); return {}; };
    auto never_sleep = [](auto) { assert(false); };
    auto never_read = [](const auto&, auto) { assert(false); return Probe::Error; };
    const auto inert = Run(Config{}, forbidden, never_sleep, never_read, [] { assert(false); });
    assert(inert.outcome == Outcome::Disabled && inert.Proceed());
    Fixture f;
    for (auto broken : {Config{true,false,f.config.release,kToken}, Config{false,true,{},kToken},
                         Config{true,true,{},kToken}, Config{true,true,"relative.txt",kToken},
                         Config{true,true,f.root,kToken}, Config{true,true,f.config.release,""},
                         Config{true,true,f.config.release,"0123"}, Config{true,true,f.config.release,std::string(32,'g')},
                         Config{true,true,f.config.release,std::string(kToken)+"\n"}}) {
        // An existing directory is a pre-existing path, not a bad token/config case.
        if (broken.release == f.root) { f.config = broken; assert(!f.Go().Proceed()); continue; }
        const auto result = Run(broken, forbidden, never_sleep, never_read, [] { assert(false); });
        assert(result.outcome == Outcome::Rejected && !result.Proceed());
    }
    assert(!fs::exists(f.root/"relative.txt"));
}

void NewAtomicRelease() {
    Fixture f;
    f.on_sleep = [&] { if (f.sleeps == 2) f.Publish(); };
    const auto result = f.Go();
    assert(result.outcome == Outcome::Released && result.Proceed());
    assert(f.sleeps == 2 && f.waits == 1 && Bytes(f.config.release) == kToken);
    assert(ReadRelease(f.config.release, kToken) == Probe::Match);  // Contents remain intact.
}

void PreexistingAndForeignPreserved() {
    for (auto payload : {std::string(kToken), std::string(32,'0'), std::string{}, std::string(kToken)+"\n",
                         std::string("\xEF\xBB\xBF")+kToken}) {
        Fixture f; WriteNew(f.config.release,payload);
        const auto result = f.Go();
        assert(result.outcome == Outcome::Rejected && !result.Proceed());
        assert(f.sleeps == 0 && f.waits == 0 && Bytes(f.config.release) == payload);
    }
    Fixture directory; assert(fs::create_directory(directory.config.release));
    assert(!directory.Go().Proceed() && fs::is_directory(directory.config.release));
    Fixture link;
    fs::create_symlink(link.root/"missing",link.config.release);
    assert(!link.Go().Proceed() && fs::is_symlink(link.config.release));
}

void NewlyPublishedInvalidAndError() {
    for (auto payload : {std::string(32,'0'),std::string{},std::string(kToken)+"\n",std::string("\xEF\xBB\xBF")+kToken}) {
        Fixture f; f.on_sleep = [&] { f.Publish(payload); };
        assert(!f.Go().Proceed() && f.sleeps == 1 && Bytes(f.config.release) == payload);
    }
    Fixture f;
    f.on_sleep = [&] { fs::create_symlink(f.root/"missing",f.config.release); };
    assert(!f.Go().Proceed() && fs::is_symlink(f.config.release));
    Fixture error; uint32_t reads = 0;
    auto result = error.RunWith([&](const auto&,auto) { return ++reads == 1 ? Probe::Missing : Probe::Error; });
    assert(!result.Proceed() && error.waits == 1 && error.sleeps == 0 && !fs::exists(error.config.release));
}

void Deadlines() {
    Fixture timeout;
    const auto stopped = timeout.Go();
    assert(stopped.outcome == Outcome::TimedOut && !stopped.Proceed());
    assert(timeout.time == Clock::time_point{}+kTimeout && timeout.sleeps == 480);
    assert(!fs::exists(timeout.config.release));
    for (auto delay : {kTimeout, kTimeout+std::chrono::seconds(1)}) {
        Fixture late; uint32_t reads = 0;
        const auto result = late.RunWith([&](const auto&,auto) {
            if (++reads == 1) return Probe::Missing;
            late.time += delay; return Probe::Match;
        });
        assert(result.outcome == Outcome::TimedOut && !result.Proceed());  // A slow probe cannot grant late release.
    }
    Fixture boundary;
    boundary.on_sleep = [&] { if (boundary.time == Clock::time_point{}+kTimeout) boundary.Publish(); };
    assert(boundary.Go().outcome == Outcome::TimedOut && Bytes(boundary.config.release) == kToken);
    Fixture early;
    early.on_sleep = [&] { if (early.sleeps == 479) early.Publish(); };
    assert(early.Go().outcome == Outcome::Released && early.time < Clock::time_point{}+kTimeout);
}

void ExceptionsAndClock() {
    Fixture failure; uint32_t reads = 0;
    const auto result = failure.RunWith([&](const auto&,auto) -> Probe {
        if (++reads == 1) return Probe::Missing;
        throw std::runtime_error("fixture read failure");
    });
    assert(!result.Proceed() && !fs::exists(failure.config.release));
    Fixture sleep; sleep.on_sleep = [] { throw std::runtime_error("fixture sleep failure"); };
    assert(!sleep.Go().Proceed() && !fs::exists(sleep.config.release));
    Fixture backwards; uint32_t probes = 0;
    const auto backwards_result = backwards.RunWith([&](const auto&,auto) {
        if (++probes > 1) backwards.time -= std::chrono::milliseconds(1);
        return Probe::Missing;
    });
    assert(!backwards_result.Proceed());
}

void ExplicitEmptyEnvironment() {
    constexpr auto file = L"XBE_QA_STARTUP_RELEASE_FILE", token = L"XBE_QA_STARTUP_TOKEN";
    const auto old_file = ReadEnvironment(file), old_token = ReadEnvironment(token);
    SetEnvironmentVariableW(file,nullptr); SetEnvironmentVariableW(token,nullptr);
    assert(!FromEnvironment().file_configured && !FromEnvironment().token_configured);
    SetEnvironmentVariableW(file,L""); SetEnvironmentVariableW(token,L"");
    const auto empty = FromEnvironment();
    assert(empty.file_configured && empty.token_configured && empty.release.empty() && empty.token.empty());
    SetEnvironmentVariableW(file,old_file.configured ? old_file.value.c_str() : nullptr);
    SetEnvironmentVariableW(token,old_token.configured ? old_token.value.c_str() : nullptr);
}

void RawPathBeforeNormalization() {
    const fs::path relative(std::string(260,'r')+".txt");
    assert(!relative.is_absolute() && hostpath::Native(relative).is_absolute());  // The actual old seam hazard.
    Config config{true,true,relative,kToken};
    uint32_t normalized = 0;
    PrepareRelease(config,[&](const auto& path) { ++normalized; return hostpath::Native(path); });
    assert(normalized == 0 && config.release == relative);
    const auto result = Run(config,[]() -> Clock::time_point { assert(false); return {}; },
        [](auto) { assert(false); },[](const auto&,auto) { assert(false); return Probe::Error; },[] { assert(false); });
    assert(!result.Proceed());
    Fixture absolute;
    PrepareRelease(absolute.config,[&](const auto& path) { ++normalized; return hostpath::Native(path); });
    assert(normalized == 1 && absolute.config.release.is_absolute());
}
}

int main() {
    InertAndBadConfig(); NewAtomicRelease(); PreexistingAndForeignPreserved(); NewlyPublishedInvalidAndError();
    Deadlines(); ExceptionsAndClock(); ExplicitEmptyEnvironment(); RawPathBeforeNormalization();
    std::cout << "QA startup gate: 8 CPU filesystem/clock groups passed; no game/window/device created\n";
}
