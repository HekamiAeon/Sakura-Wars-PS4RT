// SPDX-License-Identifier: GPL-2.0-or-later
#include "ps4rt_achievements.h"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

namespace Ps4RtAchievements {
namespace {
constexpr int32_t Error(unsigned code) { return static_cast<int32_t>(0x80551600u | code); }
constexpr auto Invalid = Error(4), InvalidHandle = Error(8), InvalidContext = Error(9),
    InvalidTrophy = Error(10), AlreadyUnlocked = Error(12), CannotUnlockPlatinum = Error(13),
    NotRegistered = Error(15), AlreadyRegistered = Error(16), BrokenData = Error(17),
    SaveFailed = Error(18), ContextExists = Error(19), MissingCatalog = Error(38);
using State = std::map<int, uint64_t>;

std::vector<std::string> Split(const std::string& s) {
    std::vector<std::string> result;
    size_t begin = 0;
    for (size_t end; (end = s.find('\t', begin)) != std::string::npos; begin = end + 1)
        result.push_back(s.substr(begin, end - begin));
    result.push_back(s.substr(begin));
    return result;
}
int Number(const std::string& s) {
    int n;
    const auto [end, error] = std::from_chars(s.data(), s.data() + s.size(), n);
    if (error != std::errc{} || end != s.data() + s.size()) throw std::runtime_error("Invalid number");
    return n;
}
bool Identity(const std::string& s) {
    if (s.size() != 12 || s.substr(0, 4) != "NPWR" || s[9] != '_') return false;
    for (size_t i = 4; i < s.size(); ++i)
        if (i != 9 && (s[i] < '0' || s[i] > '9')) return false;
    return true;
}
int Grade(char c) {
    switch (c) { case 'P': return 1; case 'G': return 2; case 'S': return 3; case 'B': return 4; }
    return 0;
}
template<size_t N> void Text(char (&out)[N], const std::string& s) {
    size_t n = std::min(N - 1, s.size());
    while (n && n < s.size() && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    std::memcpy(out, s.data(), n);
    out[n] = 0;
}
// The original Sakura runtime's stub: leave the guest's size field, clear the rest.
template<typename T> void ZeroAfterSize(T* out) {
    if (!out) return;
    const size_t size = out->size;
    if (size > sizeof(size_t) && size < 4096)
        std::memset(reinterpret_cast<char*>(out) + sizeof(size_t), 0, size - sizeof(size_t));
}
class FileLock {
public:
    explicit FileLock(const std::filesystem::path& path) {
        fd = open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0) throw std::runtime_error("Cannot open achievement lock");
        if (flock(fd, LOCK_EX) != 0) { close(fd); fd = -1; throw std::runtime_error("Cannot lock achievements"); }
    }
    ~FileLock() { if (fd >= 0) close(fd); }
    FileLock(const FileLock&) = delete;
private:
    int fd = -1;
};
State ReadState(const std::filesystem::path& path) {
    if (!std::filesystem::exists(path)) return {};
    if (std::filesystem::file_size(path) > 65536) throw std::runtime_error("Oversized achievement state");
    std::ifstream in(path);
    std::string line;
    if (!std::getline(in, line) || line != "PS4RT-UNLOCKS\t1") throw std::runtime_error("Invalid achievement state");
    State result;
    while (std::getline(in, line)) {
        auto fields = Split(line);
        if (fields.size() != 2) throw std::runtime_error("Invalid achievement state entry");
        const int id = Number(fields[0]);
        uint64_t when = 0;
        const auto& value = fields[1];
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), when);
        if (id < 0 || id >= 128 || when == 0 || error != std::errc{} || end != value.data() + value.size() ||
            !result.emplace(id, when).second) throw std::runtime_error("Invalid achievement timestamp");
    }
    if (in.bad()) throw std::runtime_error("Cannot read achievement state");
    return result;
}
void WriteState(const std::filesystem::path& path, const State& state) {
    std::ostringstream text;
    text << "PS4RT-UNLOCKS\t1\n";
    for (auto [id, when] : state) text << id << '\t' << when << '\n';
    const auto data = text.str();
    std::string temporary = path.string() + ".tmp-XXXXXX";
    int fd = mkstemp(temporary.data());
    if (fd < 0) throw std::runtime_error("Cannot create achievement state");
    bool ok = true;
    for (size_t pos = 0; pos < data.size();) {
        const auto n = write(fd, data.data() + pos, data.size() - pos);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = false; break; }
        pos += static_cast<size_t>(n);
    }
    if (ok) ok = fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok) ok = rename(temporary.c_str(), path.c_str()) == 0;
    if (!ok) { unlink(temporary.c_str()); throw std::runtime_error("Cannot save achievement state"); }
    const int dir = open(path.parent_path().c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir >= 0) { (void)fsync(dir); close(dir); }
}
} // namespace

struct Service::Impl {
    struct Trophy { Achievement row; int platinum = -1; };
    struct Set { std::string identity, title, description; std::map<int, Trophy> trophies; };
    struct Context { int user; uint32_t label; bool registered = false; };
    std::mutex mutex;
    std::map<uint32_t, Set> sets;
    std::map<int, Context> contexts;
    std::map<int, bool> handles; // true = aborted
    std::deque<Achievement> notifications;
    std::filesystem::path root;
    std::string problem;
    int next_context = 1, next_handle = 1;
    // Sakura Wars port: without a usable catalog, keep the original runtime's
    // accept-and-log trophy contract so the game boots exactly as before.
    bool Detached() const { return sets.empty(); }

    std::filesystem::path Path(int user, const Set& set) const {
        return root / std::to_string(user) / (set.identity + ".dat");
    }
    int32_t Check(int context, int handle, bool registered = true) {
        if (!contexts.contains(context)) return InvalidContext;
        const auto h = handles.find(handle);
        if (h == handles.end()) return InvalidHandle;
        if (h->second) return Error(7);
        if (registered && !contexts.at(context).registered) return NotRegistered;
        return 0;
    }
};

Service::Service(const std::filesystem::path& catalog, const std::filesystem::path& root)
    : impl(std::make_unique<Impl>()) {
    impl->root = root;
    if (catalog.empty() || root.empty()) { impl->problem = "Achievements are unavailable for this game."; return; }
    try {
        if (std::filesystem::file_size(catalog) > 4 * 1024 * 1024) throw std::runtime_error("Oversized catalog");
        std::ifstream in(catalog);
        std::string line;
        if (!std::getline(in, line) || line != "PS4RT-TROPHIES\t1") throw std::runtime_error("Invalid catalog header");
        while (std::getline(in, line)) {
            if (line.size() > 16384) throw std::runtime_error("Oversized catalog entry");
            auto f = Split(line);
            if (f.size() == 5 && f[0] == "S") {
                const int label = Number(f[1]);
                if (label < 0 || !Identity(f[2]) || impl->sets.contains(label)) throw std::runtime_error("Invalid set");
                impl->sets.emplace(label, Impl::Set{f[2], f[3], f[4], {}});
            } else if (f.size() == 9 && f[0] == "T") {
                const int label = Number(f[1]), id = Number(f[2]);
                if (!impl->sets.contains(label) || id < 0 || id >= 128 || f[3].size() != 1 || !Grade(f[3][0]))
                    throw std::runtime_error("Invalid trophy");
                auto& set = impl->sets.at(label);
                Achievement row;
                row.label = label; row.id = id; row.group = Number(f[5]); row.grade = f[3][0];
                row.hidden = Number(f[6]) != 0; row.name = f[7]; row.description = f[8];
                row.set = set.identity; row.title = set.title;
                char icon[16]; std::snprintf(icon, sizeof(icon), "%03d.png", id);
                row.icon = (catalog.parent_path() / set.identity / icon).string();
                if (!set.trophies.emplace(id, Impl::Trophy{row, Number(f[4])}).second)
                    throw std::runtime_error("Duplicate trophy");
            } else throw std::runtime_error("Invalid catalog entry");
        }
        if (impl->sets.empty() || in.bad()) throw std::runtime_error("Empty catalog");
        for (const auto& [label, set] : impl->sets) {
            if (set.trophies.empty()) throw std::runtime_error("Empty trophy set");
            for (const auto& [id, t] : set.trophies)
                if (t.platinum >= 0 && (!set.trophies.contains(t.platinum) ||
                    set.trophies.at(t.platinum).row.grade != 'P')) throw std::runtime_error("Invalid platinum link");
        }
    } catch (const std::exception& e) {
        impl->sets.clear();
        impl->problem = "Achievements are unavailable for this game.";
        std::fprintf(stderr, "Achievements: catalog unavailable: %s\n", e.what());
    }
}
Service::~Service() = default;
const std::string& Service::Problem() const { return impl->problem; }

int32_t Service::CreateContext(int32_t* out, int32_t user, uint32_t label, uint64_t options) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) {
        if (!out) return Invalid;
        *out = impl->next_context++;
        return 0;
    }
    if (!out || options) return Invalid;
    if (user <= 0) return Error(37);
    if (!impl->sets.contains(label)) return MissingCatalog;
    for (auto [id, c] : impl->contexts) if (c.user == user && c.label == label) return ContextExists;
    if (impl->contexts.size() >= 32) return Error(35);
    *out = impl->next_context++;
    impl->contexts.emplace(*out, Impl::Context{user, label});
    return 0;
}
int32_t Service::CreateHandle(int32_t* out) {
    std::scoped_lock lock{impl->mutex};
    if (!out) return Invalid;
    if (impl->Detached()) { *out = impl->next_handle++; return 0; }
    if (impl->handles.size() >= 128) return Error(36);
    *out = impl->next_handle++;
    impl->handles.emplace(*out, false);
    return 0;
}
int32_t Service::Register(int32_t context, int32_t handle, uint64_t options) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) {
        std::printf("Achievements: trophy context %d registered without metadata; unlocks are not recorded\n",
                    context);
        return 0;
    }
    if (options) return Invalid;
    if (auto error = impl->Check(context, handle, false)) return error;
    auto& c = impl->contexts.at(context);
    if (c.registered) return AlreadyRegistered;
    const auto& set = impl->sets.at(c.label);
    size_t earned = 0;
    try { earned = ReadState(impl->Path(c.user, set)).size(); }
    catch (const std::exception&) {
        std::fprintf(stderr, "Achievements: %s state for user %d is damaged; left unchanged\n",
                     set.identity.c_str(), c.user);
        return BrokenData;
    }
    c.registered = true;
    std::printf("Achievements: %s registered for user %d (%zu / %zu unlocked)\n", set.identity.c_str(),
                c.user, earned, set.trophies.size());
    return 0;
}
int32_t Service::Unlock(int32_t context, int32_t handle, int32_t id, int32_t* platinum) {
    std::scoped_lock lock{impl->mutex};
    if (!platinum) return Invalid;
    *platinum = -1;
    if (impl->Detached()) {
        std::printf("Runtime: trophy %d unlocked (not recorded: achievements unavailable)\n", id);
        return 0;
    }
    if (auto error = impl->Check(context, handle)) return error;
    const auto c = impl->contexts.at(context);
    const auto& set = impl->sets.at(c.label);
    if (!set.trophies.contains(id)) return InvalidTrophy;
    if (set.trophies.at(id).row.grade == 'P') return CannotUnlockPlatinum;
    try {
        const auto path = impl->Path(c.user, set);
        std::filesystem::create_directories(path.parent_path());
        FileLock file_lock(path.string() + ".lock");
        auto state = ReadState(path);
        if (state.contains(id)) return AlreadyUnlocked;
        const auto when = static_cast<uint64_t>(std::time(nullptr));
        state[id] = when;
        std::vector<int> earned{id};
        for (auto& [pid, p] : set.trophies) {
            if (p.row.grade != 'P' || state.contains(pid)) continue;
            bool required = false, complete = true;
            for (auto& [tid, t] : set.trophies) {
                if (t.row.grade != 'P' && t.platinum == pid) {
                    required = true;
                    complete &= state.contains(tid);
                }
            }
            if (required && complete) { state[pid] = when; earned.push_back(pid); }
        }
        WriteState(path, state); // Publish first. A failed save never announces an award.
        for (int trophy : earned) {
            auto row = set.trophies.at(trophy).row;
            row.unlocked_at = when;
            if (row.grade == 'P') *platinum = trophy;
            impl->notifications.push_back(std::move(row));
            std::printf("Runtime: trophy %d unlocked (%s)\n", trophy, set.identity.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "Achievements: unlock could not be saved: %s\n", e.what());
        return SaveFailed;
    }
}
int32_t Service::TrophyInfo(int32_t context, int32_t handle, int32_t id,
                           Ps4RtTrophyDetails* details, Ps4RtTrophyData* data) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) { ZeroAfterSize(details); ZeroAfterSize(data); return 0; }
    if ((!details && !data) || (details && details->size != sizeof(*details)) ||
        (data && data->size != sizeof(*data))) return Invalid;
    if (auto error = impl->Check(context, handle)) return error;
    const auto c = impl->contexts.at(context);
    const auto& set = impl->sets.at(c.label);
    if (!set.trophies.contains(id)) return InvalidTrophy;
    const auto& row = set.trophies.at(id).row;
    State state;
    try { state = ReadState(impl->Path(c.user, set)); } catch (const std::exception&) { return BrokenData; }
    if (details) {
        *details = {}; details->size = sizeof(*details); details->trophy_id = id;
        details->grade = Grade(row.grade); details->group_id = row.group; details->hidden = row.hidden;
        Text(details->name, row.name); Text(details->description, row.description);
    }
    if (data) {
        *data = {}; data->size = sizeof(*data); data->trophy_id = id;
        data->unlocked = state.contains(id);
        // OrbisRtcTick: microseconds since 0001-01-01, matching runtime_rtc.c.
        data->timestamp = state.contains(id) ? UINT64_C(0xdcbffeff2bc000) + state.at(id) * 1000000 : 0;
    }
    return 0;
}
int32_t Service::GameInfo(int32_t context, int32_t handle, Ps4RtTrophyGameDetails* details,
                         Ps4RtTrophyGameData* data) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) { ZeroAfterSize(details); ZeroAfterSize(data); return 0; }
    if ((!details && !data) || (details && details->size != sizeof(*details)) ||
        (data && data->size != sizeof(*data))) return Invalid;
    if (auto error = impl->Check(context, handle)) return error;
    const auto c = impl->contexts.at(context);
    const auto& set = impl->sets.at(c.label);
    State state;
    try { state = ReadState(impl->Path(c.user, set)); } catch (const std::exception&) { return BrokenData; }
    std::array<uint32_t, 5> total{}, unlocked{};
    std::set<int> groups;
    uint32_t count = 0;
    for (const auto& [id, t] : set.trophies) {
        const int grade = Grade(t.row.grade);
        ++total[grade];
        if (t.row.group >= 0) groups.insert(t.row.group);
        if (state.contains(id)) { ++unlocked[grade]; ++count; }
    }
    if (details) {
        *details = {}; details->size = sizeof(*details); details->num_groups = groups.size();
        details->num_trophies = set.trophies.size(); details->num_platinum = total[1];
        details->num_gold = total[2]; details->num_silver = total[3]; details->num_bronze = total[4];
        Text(details->title, set.title); Text(details->description, set.description);
    }
    if (data) {
        *data = {}; data->size = sizeof(*data); data->unlocked_trophies = count;
        data->unlocked_platinum = unlocked[1]; data->unlocked_gold = unlocked[2];
        data->unlocked_silver = unlocked[3]; data->unlocked_bronze = unlocked[4];
        data->progress_percentage = count * 100 / set.trophies.size();
    }
    return 0;
}
int32_t Service::UnlockState(int32_t context, int32_t handle, uint32_t* flags, uint32_t* count) {
    std::scoped_lock lock{impl->mutex};
    if (!flags || !count) return Invalid;
    if (impl->Detached()) {
        if (context <= 0 || handle <= 0) return Invalid;
        std::fill_n(flags, 4, 0);
        *count = 0;
        return 0;
    }
    if (auto error = impl->Check(context, handle)) return error;
    const auto c = impl->contexts.at(context);
    const auto& set = impl->sets.at(c.label);
    try {
        auto state = ReadState(impl->Path(c.user, set));
        std::fill_n(flags, 4, 0);
        for (auto [id, when] : state) if (set.trophies.contains(id)) flags[id / 32] |= 1u << (id % 32);
        *count = set.trophies.size();
        return 0;
    } catch (const std::exception&) { return BrokenData; }
}
int32_t Service::Abort(int32_t handle) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) return handle > 0 ? 0 : Invalid;
    if (!impl->handles.contains(handle)) return InvalidHandle;
    impl->handles[handle] = true;
    return 0;
}
int32_t Service::DestroyHandle(int32_t handle) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) return handle > 0 ? 0 : Invalid;
    return impl->handles.erase(handle) ? 0 : InvalidHandle;
}
int32_t Service::DestroyContext(int32_t context) {
    std::scoped_lock lock{impl->mutex};
    if (impl->Detached()) return context > 0 ? 0 : Invalid;
    return impl->contexts.erase(context) ? 0 : InvalidContext;
}
std::vector<Achievement> Service::Snapshot(int32_t user) {
    std::scoped_lock lock{impl->mutex};
    std::vector<Achievement> result;
    for (const auto& [label, set] : impl->sets) {
        State state;
        try { state = ReadState(impl->Path(user, set)); } catch (const std::exception&) { continue; }
        for (const auto& [id, trophy] : set.trophies) {
            auto row = trophy.row;
            if (state.contains(id)) row.unlocked_at = state.at(id);
            result.push_back(std::move(row));
        }
    }
    return result;
}
std::optional<Achievement> Service::PopNotification() {
    std::scoped_lock lock{impl->mutex};
    if (impl->notifications.empty()) return std::nullopt;
    auto next = std::move(impl->notifications.front());
    impl->notifications.pop_front();
    return next;
}
bool Service::NotificationsPending() {
    std::scoped_lock lock{impl->mutex};
    return !impl->notifications.empty();
}
Service& Get() {
    static Service service(std::getenv("PS4RT_TROPHY_CATALOG") ? std::getenv("PS4RT_TROPHY_CATALOG") : "",
                           std::getenv("PS4RT_ACHIEVEMENTS_DIR") ? std::getenv("PS4RT_ACHIEVEMENTS_DIR") : "");
    return service;
}
} // namespace Ps4RtAchievements

// Keep host exceptions out of original game code (including allocation failure).
#define TROPHY_CALL(expr) try { return (expr); } catch (...) { return static_cast<int32_t>(0x80551603u); }
extern "C" {
int32_t ps4rt_trophy_create_context(int32_t* c, int32_t u, uint32_t l, uint64_t o) { TROPHY_CALL(Ps4RtAchievements::Get().CreateContext(c,u,l,o)); }
int32_t ps4rt_trophy_create_handle(int32_t* h) { TROPHY_CALL(Ps4RtAchievements::Get().CreateHandle(h)); }
int32_t ps4rt_trophy_register(int32_t c, int32_t h, uint64_t o) { TROPHY_CALL(Ps4RtAchievements::Get().Register(c,h,o)); }
int32_t ps4rt_trophy_unlock(int32_t c, int32_t h, int32_t id, int32_t* p) { TROPHY_CALL(Ps4RtAchievements::Get().Unlock(c,h,id,p)); }
int32_t ps4rt_trophy_game_info(int32_t c, int32_t h, Ps4RtTrophyGameDetails* d, Ps4RtTrophyGameData* a) { TROPHY_CALL(Ps4RtAchievements::Get().GameInfo(c,h,d,a)); }
int32_t ps4rt_trophy_info(int32_t c, int32_t h, int32_t id, Ps4RtTrophyDetails* d, Ps4RtTrophyData* a) { TROPHY_CALL(Ps4RtAchievements::Get().TrophyInfo(c,h,id,d,a)); }
int32_t ps4rt_trophy_unlock_state(int32_t c, int32_t h, uint32_t* f, uint32_t* n) { TROPHY_CALL(Ps4RtAchievements::Get().UnlockState(c,h,f,n)); }
int32_t ps4rt_trophy_abort(int32_t h) { TROPHY_CALL(Ps4RtAchievements::Get().Abort(h)); }
int32_t ps4rt_trophy_destroy_handle(int32_t h) { TROPHY_CALL(Ps4RtAchievements::Get().DestroyHandle(h)); }
int32_t ps4rt_trophy_destroy_context(int32_t c) { TROPHY_CALL(Ps4RtAchievements::Get().DestroyContext(c)); }
}
static_assert(sizeof(Ps4RtTrophyData) == 24 && sizeof(Ps4RtTrophyDetails) == 1176 &&
              sizeof(Ps4RtTrophyGameDetails) == 1184 && sizeof(Ps4RtTrophyGameData) == 32);
