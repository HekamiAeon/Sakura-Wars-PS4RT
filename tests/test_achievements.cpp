// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_achievements.h"
#include <array>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <unistd.h>

using Ps4RtAchievements::Service;
namespace fs = std::filesystem;
constexpr int32_t E(unsigned n) { return static_cast<int32_t>(0x80551600u | n); }
static int Context(Service& s, int h, int user = 1, unsigned label = 0) {
    int c = 0;
    assert(s.CreateContext(&c, user, label, 0) == 0);
    assert(s.Register(c, h, 0) == 0);
    return c;
}
int main() {
    char name[] = "/tmp/ps4rt-achievements-XXXXXX";
    const fs::path root = mkdtemp(name);
    const auto catalog = root / "catalog.tsv", state = root / "state";
    {
        std::ofstream out(catalog);
        out << "PS4RT-TROPHIES\t1\n"
            "S\t0\tNPWR00001_00\tFixture\tA synthetic trophy set\n"
            "T\t0\t0\tP\t-1\t-1\t0\tAll base awards\tBase game only\n"
            "T\t0\t2\tB\t0\t-1\t1\tFirst\tFirst hidden award\n"
            "T\t0\t5\tS\t0\t-1\t0\tSecond\tSecond base award\n"
            "T\t0\t9\tG\t-1\t1\t0\tExtra\tOptional DLC award\n"
            "S\t1\tNPWR00002_00\tNo platinum\tAnother set\n"
            "T\t1\t2\tB\t-1\t1\t0\tSeparate\tSeparate set and state\n";
    }
    Service service(catalog, state);
    assert(service.Problem().empty());
    int handle = 0, other = 0, platinum = 99;
    assert(service.CreateHandle(&handle) == 0);
    assert(service.CreateContext(&other, 1, 0, 1) == E(4));
    assert(service.CreateContext(&other, 1, 12, 0) == E(38));
    int context = Context(service, handle);
    assert(service.Register(context, handle, 0) == E(16));
    assert(service.CreateContext(&other, 1, 0, 0) == E(19));
    assert(service.Unlock(context, handle, 0, &platinum) == E(13) && platinum == -1);
    assert(service.Unlock(context, handle, 1, &platinum) == E(10));
    assert(service.Unlock(context, 9999, 2, &platinum) == E(8));
    assert(service.Unlock(context, handle, 2, nullptr) == E(4));

    // Many callers requesting the same award must produce one save and one event.
    std::atomic<int> first{0};
    std::vector<std::thread> threads;
    for (int i = 0; i < 16; ++i) threads.emplace_back([&] {
        int p;
        const auto result = service.Unlock(context, handle, 2, &p);
        assert(p == -1);
        if (result == 0) ++first;
        else assert(result == E(12));
    });
    for (auto& t : threads) t.join();
    assert(first == 1);
    auto event = service.PopNotification();
    assert(event && event->id == 2 && event->hidden && event->unlocked_at);
    assert(!service.PopNotification());
    std::array<uint32_t, 4> flags{};
    uint32_t count;
    assert(service.UnlockState(context, handle, flags.data(), &count) == 0);
    assert(count == 4 && flags[0] == (1u << 2));
    Ps4RtTrophyGameDetails details{.size = sizeof(details)};
    Ps4RtTrophyGameData data{.size = sizeof(data)};
    assert(service.GameInfo(context, handle, &details, &data) == 0);
    assert(details.num_trophies == 4 && details.num_groups == 1 && details.num_platinum == 1);
    assert(data.unlocked_trophies == 1 && data.unlocked_bronze == 1 && data.progress_percentage == 25);
    Ps4RtTrophyDetails td{.size = sizeof(td)};
    Ps4RtTrophyData ta{.size = sizeof(ta)};
    assert(service.TrophyInfo(context, handle, 2, &td, &ta) == 0);
    assert(td.trophy_id == 2 && td.grade == 4 && td.hidden && ta.unlocked);
    assert(ta.timestamp == UINT64_C(0xdcbffeff2bc000) + event->unlocked_at * 1000000);

    // A new service after restart has consistent queries, without replaying popups.
    Service restarted(catalog, state);
    int h2;
    assert(restarted.CreateHandle(&h2) == 0);
    const int c2 = Context(restarted, h2);
    assert(restarted.Unlock(c2, h2, 2, &platinum) == E(12));
    assert(!restarted.NotificationsPending());
    auto snapshot = restarted.Snapshot();
    assert(snapshot.size() == 5 && snapshot[1].id == 2 && snapshot[1].unlocked_at);

    // Independent processes race different awards in the same set. Atomic
    // replacement and an interprocess lock must merge rather than lose a write.
    std::array<pid_t, 2> children{};
    for (size_t i = 0; i < children.size(); ++i) {
        children[i] = fork();
        assert(children[i] >= 0);
        if (!children[i]) {
            Service child(catalog, state);
            int h, p;
            assert(child.CreateHandle(&h) == 0);
            int c = Context(child, h);
            assert(child.Unlock(c, h, i == 0 ? 5 : 9, &p) == 0);
            if (i == 0) assert(p == 0); // base awards complete; DLC is irrelevant
            else assert(p == -1);
            _exit(0);
        }
    }
    for (pid_t child : children) { int status; assert(waitpid(child, &status, 0) == child && status == 0); }
    assert(restarted.UnlockState(c2, h2, flags.data(), &count) == 0);
    assert(flags[0] == ((1u << 0) | (1u << 2) | (1u << 5) | (1u << 9)));
    assert(restarted.GameInfo(c2, h2, &details, &data) == 0);
    assert(data.unlocked_platinum == 1 && data.progress_percentage == 100);

    // User and trophy-set identity both isolate earned state. No platinum set
    // must never invent award zero or another set's linked platinum.
    int user2 = Context(service, handle, 2);
    assert(service.UnlockState(user2, handle, flags.data(), &count) == 0 && flags[0] == 0);
    int set2 = Context(service, handle, 1, 1);
    assert(service.Unlock(set2, handle, 2, &platinum) == 0 && platinum == -1);
    assert(service.UnlockState(set2, handle, flags.data(), &count) == 0 && flags[0] == (1u << 2) && count == 1);
    assert(service.Abort(handle) == 0);
    assert(service.Unlock(set2, handle, 2, &platinum) == E(7));
    assert(service.DestroyHandle(handle) == 0 && service.DestroyHandle(handle) == E(8));
    assert(service.DestroyContext(context) == 0 && service.DestroyContext(context) == E(9));

    // Failed writes are not announced. Damaged state is preserved for recovery.
    const auto blocked = root / "blocked";
    fs::create_directory(blocked);
    Service unwritable(catalog, blocked);
    assert(unwritable.CreateHandle(&handle) == 0);
    const int cannot_save = Context(unwritable, handle);
    fs::remove(blocked);
    { std::ofstream out(blocked); out << "not a directory"; }
    assert(unwritable.Unlock(cannot_save, handle, 2, &platinum) == E(18));
    assert(!unwritable.NotificationsPending());
    const auto damaged = state / "1" / "NPWR00002_00.dat";
    { std::ofstream out(damaged); out << "keep damaged evidence"; }
    assert(restarted.CreateContext(&other, 1, 1, 0) == 0);
    assert(restarted.Register(other, h2, 0) == E(17));
    std::ifstream in(damaged); std::string text; std::getline(in, text);
    assert(text == "keep damaged evidence");
    assert(!unwritable.NotificationsPending());
    // Sakura Wars: missing or corrupt metadata keeps the original accept-and-log
    // trophy contract (the game must boot as before) and never writes state.
    {
        const auto corrupt = root / "corrupt.tsv";
        { std::ofstream out(corrupt); out << "PS4RT-TROPHIES\t1\nT\tbroken\n"; }
        const auto detached_state = root / "detached-state";
        for (const auto& source : {root / "missing.tsv", corrupt, fs::path{}}) {
            Service detached(source, detached_state);
            assert(!detached.Problem().empty());
            int c = 0, h = 0, p = 7;
            assert(detached.CreateContext(&c, 1, 0, 0) == 0 && c > 0);
            assert(detached.CreateContext(&c, 1, 0, 0) == 0); // the old stub allowed repeats
            assert(detached.CreateContext(nullptr, 1, 0, 0) == E(4));
            assert(detached.CreateHandle(&h) == 0 && h > 0);
            assert(detached.Register(c, h, 0) == 0);
            assert(detached.Unlock(c, h, 3, &p) == 0 && p == -1);
            assert(!detached.NotificationsPending());
            std::array<uint32_t, 4> f{1, 1, 1, 1};
            uint32_t n = 9;
            assert(detached.UnlockState(c, h, f.data(), &n) == 0 && n == 0 &&
                   f[0] == 0 && f[1] == 0 && f[2] == 0 && f[3] == 0);
            assert(detached.UnlockState(0, h, f.data(), &n) == E(4));
            Ps4RtTrophyGameData gd{.size = sizeof(gd), .unlocked_trophies = 5};
            assert(detached.GameInfo(c, h, nullptr, &gd) == 0 && gd.size == sizeof(gd) &&
                   gd.unlocked_trophies == 0);
            assert(detached.Snapshot().empty());
            assert(detached.Abort(h) == 0 && detached.DestroyHandle(h) == 0 &&
                   detached.DestroyContext(c) == 0 && detached.DestroyHandle(0) == E(4));
        }
        assert(!fs::exists(detached_state));
    }
    // Exercise the exported C ABI used by runtime_services.c, not just the C++ API.
    setenv("PS4RT_TROPHY_CATALOG", catalog.c_str(), 1);
    setenv("PS4RT_ACHIEVEMENTS_DIR", state.c_str(), 1);
    int guest_context = 0, guest_handle = 0;
    assert(ps4rt_trophy_create_context(&guest_context, 1, 0, 0) == 0);
    assert(ps4rt_trophy_create_handle(&guest_handle) == 0);
    assert(ps4rt_trophy_register(guest_context, guest_handle, 0) == 0);
    assert(ps4rt_trophy_game_info(guest_context, guest_handle, &details, &data) == 0);
    assert(data.unlocked_trophies == 4 && details.num_trophies == 4);
    assert(ps4rt_trophy_unlock_state(guest_context, guest_handle, flags.data(), &count) == 0);
    assert(flags[0] == ((1u << 0) | (1u << 2) | (1u << 5) | (1u << 9)) && count == 4);
    assert(ps4rt_trophy_info(guest_context, guest_handle, 5, &td, &ta) == 0 && ta.unlocked);
    assert(ps4rt_trophy_unlock(guest_context, guest_handle, 5, &platinum) == E(12));
    assert(ps4rt_trophy_abort(guest_handle) == 0);
    assert(ps4rt_trophy_destroy_handle(guest_handle) == 0);
    assert(ps4rt_trophy_destroy_context(guest_context) == 0);
    fs::remove_all(root);
    std::puts("PASS: achievement API, sparse IDs, duplicates, concurrent writers, restart, users/sets, platinum/DLC, damaged state and metadata-less boot contract");
}
