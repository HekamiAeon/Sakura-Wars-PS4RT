// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "ps4rt_trophy.h"
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace Ps4RtAchievements {
struct Achievement {
    uint32_t label = 0;
    int id = -1, group = -1;
    char grade = 'B';
    bool hidden = false;
    uint64_t unlocked_at = 0; // Unix seconds; zero means locked
    std::string set, title, name, description, icon;
};

// No Vulkan, SDL, guest memory or UI dependency. The guest API and overlay use
// this same store. A process lock plus atomic replacement protects concurrent
// instances; all in-process operations and notification delivery are serialized.
class Service {
public:
    Service(const std::filesystem::path& catalog, const std::filesystem::path& state_root);
    ~Service();
    int32_t CreateContext(int32_t*, int32_t user, uint32_t label, uint64_t options);
    int32_t CreateHandle(int32_t*);
    int32_t Register(int32_t context, int32_t handle, uint64_t options);
    int32_t Unlock(int32_t context, int32_t handle, int32_t id, int32_t* platinum);
    int32_t GameInfo(int32_t, int32_t, Ps4RtTrophyGameDetails*, Ps4RtTrophyGameData*);
    int32_t TrophyInfo(int32_t, int32_t, int32_t, Ps4RtTrophyDetails*, Ps4RtTrophyData*);
    int32_t UnlockState(int32_t, int32_t, uint32_t* flags, uint32_t* count);
    int32_t Abort(int32_t handle);
    int32_t DestroyHandle(int32_t handle);
    int32_t DestroyContext(int32_t context);
    std::vector<Achievement> Snapshot(int32_t user = 1);
    std::optional<Achievement> PopNotification();
    bool NotificationsPending();
    const std::string& Problem() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

Service& Get();
} // namespace Ps4RtAchievements
