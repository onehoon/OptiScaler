#pragma once

#include <cstddef>
#include <string_view>

namespace ReflexDxgiIdentity
{
enum class Scope
{
    Off,
    SlCommonOnly,
    GameOnly,
    SlCommonAndGame,
};

inline bool EqualsIgnoreCase(std::string_view left, std::string_view right)
{
    if (left.size() != right.size())
        return false;

    for (std::size_t i = 0; i < left.size(); ++i)
    {
        const auto lower = [](char value)
        { return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value; };
        if (lower(left[i]) != lower(right[i]))
            return false;
    }

    return true;
}

inline Scope ParseScope(std::string_view value)
{
    if (EqualsIgnoreCase(value, "SlCommonOnly"))
        return Scope::SlCommonOnly;
    if (EqualsIgnoreCase(value, "GameOnly"))
        return Scope::GameOnly;
    if (EqualsIgnoreCase(value, "SlCommonAndGame"))
        return Scope::SlCommonAndGame;
    return Scope::Off;
}

inline bool IsSlCommonCaller(std::string_view caller) { return EqualsIgnoreCase(caller, "sl.common.dll"); }

inline bool IsGameCaller(std::string_view caller)
{
    return EqualsIgnoreCase(caller, "pragmata.exe") || EqualsIgnoreCase(caller, "re9.exe");
}

inline bool ShouldApply(std::string_view caller, std::string_view configuredScope, bool targetQuirk, bool intelGpu,
                        bool streamlineSpoofing, bool globalDxgiSpoofing, bool skipSpoofing)
{
    if (!targetQuirk || !intelGpu || !streamlineSpoofing || globalDxgiSpoofing || skipSpoofing)
        return false;

    const auto scope = ParseScope(configuredScope);
    if (scope == Scope::SlCommonOnly)
        return IsSlCommonCaller(caller);
    if (scope == Scope::GameOnly)
        return IsGameCaller(caller);
    if (scope == Scope::SlCommonAndGame)
        return IsSlCommonCaller(caller) || IsGameCaller(caller);
    return false;
}
} // namespace ReflexDxgiIdentity
