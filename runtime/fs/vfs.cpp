#include "vfs.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <map>
#include <mutex>

namespace
{
    std::mutex g_mutex;
    // Lowercase guest prefix (no trailing backslash) -> lowercase guest target.
    std::map<std::string, std::string> g_links;
    // Lowercase device prefix -> host directory.
    struct Device { std::filesystem::path host; bool readOnly; };
    std::map<std::string, Device> g_devices;
    std::filesystem::path g_userRoot;

    std::string Normalize(std::string p)
    {
        std::replace(p.begin(), p.end(), '/', '\\');
        std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        if (p.rfind("\\??\\", 0) == 0) p.erase(0, 4);
        while (p.size() > 1 && p.back() == '\\') p.pop_back();
        return p;
    }

    // Appends one (lowercased) guest path component to a host path. Guest paths are matched
    // case-insensitively (Normalize lowercases them), which Windows does for free; on Apple /
    // Linux file systems that are case-sensitive (iPadOS), look up the real spelling in the
    // directory - e.g. the menu movies "pes_topBG_E_1.sfd" were "not found" on the iPad.
    // A component that does not exist yet (a file being created) keeps the guest spelling.
    std::filesystem::path AppendComponent(const std::filesystem::path& dir, const std::string& part)
    {
        std::filesystem::path direct = dir / part;
#ifndef _WIN32
        std::error_code ec;
        if (std::filesystem::exists(direct, ec))
            return direct;
        for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        {
            const std::string name = it->path().filename().string();
            if (name.size() == part.size() &&
                std::equal(name.begin(), name.end(), part.begin(),
                    [](unsigned char a, unsigned char b) { return std::tolower(a) == std::tolower(b); }))
                return it->path();
        }
#endif
        return direct;
    }

    // Longest entry in `map` that is a prefix of `path` at a path-component boundary.
    template<typename M>
    typename M::const_iterator LongestPrefix(const M& map, const std::string& path)
    {
        auto best = map.end();
        for (auto it = map.begin(); it != map.end(); ++it)
        {
            const std::string& prefix = it->first;
            if (path.rfind(prefix, 0) == 0 && (path.size() == prefix.size() || path[prefix.size()] == '\\'))
                if (best == map.end() || prefix.size() > best->first.size())
                    best = it;
        }
        return best;
    }
}

namespace Fs
{
    void Init(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot)
    {
        std::lock_guard lock(g_mutex);
        g_userRoot = userRoot;
        g_devices["\\device\\cdrom0"] = { gameRoot, true };
        g_devices["\\device\\harddisk0\\partition1"] = { userRoot / "hdd", false };
        g_devices["\\device\\harddisk0\\cache0"] = { userRoot / "cache", false };
        g_links["game:"] = "\\device\\cdrom0";
        g_links["d:"] = "\\device\\cdrom0";
        g_links["cache:"] = "\\device\\harddisk0\\cache0";
        g_links["hdd:"] = "\\device\\harddisk0\\partition1";
        std::filesystem::create_directories(userRoot / "hdd");
        std::filesystem::create_directories(userRoot / "cache");
        std::printf("[fs] game:\\ -> %s, user data -> %s\n", gameRoot.string().c_str(), userRoot.string().c_str());
    }

    void MountContent(const std::string& root, const std::filesystem::path& hostDirectory)
    {
        std::lock_guard lock(g_mutex);
        const std::string name = Normalize(root);
        g_devices["\\device\\content\\" + name] = { hostDirectory, false };
        g_links[name + ":"] = "\\device\\content\\" + name;
        std::printf("[fs] content %s: -> %s\n", name.c_str(), hostDirectory.string().c_str());
    }

    void UnmountContent(const std::string& root)
    {
        std::lock_guard lock(g_mutex);
        const std::string name = Normalize(root);
        g_devices.erase("\\device\\content\\" + name);
        g_links.erase(name + ":");
    }

    std::filesystem::path UserRoot() { return g_userRoot; }

    void CreateSymbolicLink(const std::string& link, const std::string& target)
    {
        std::lock_guard lock(g_mutex);
        g_links[Normalize(link)] = Normalize(target);
        std::printf("[fs] link %s -> %s\n", Normalize(link).c_str(), Normalize(target).c_str());
    }

    void DeleteSymbolicLink(const std::string& link)
    {
        std::lock_guard lock(g_mutex);
        g_links.erase(Normalize(link));
    }

    std::filesystem::path ResolveGuestPath(const std::string& guestPath, bool* readOnly)
    {
        std::lock_guard lock(g_mutex);
        std::string p = Normalize(guestPath);

        for (int depth = 0; depth < 8; ++depth) // follow links (bounded)
        {
            const auto link = LongestPrefix(g_links, p);
            if (link == g_links.end())
                break;
            p = link->second + p.substr(link->first.size());
        }

        const auto device = LongestPrefix(g_devices, p);
        if (device == g_devices.end())
            return {};
        if (readOnly) *readOnly = device->second.readOnly;

        std::string rest = p.substr(device->first.size());
        while (!rest.empty() && rest.front() == '\\') rest.erase(0, 1);
        std::filesystem::path host = device->second.host;
        size_t start = 0;
        while (start < rest.size())
        {
            const size_t end = rest.find('\\', start);
            const std::string part = rest.substr(start, end == std::string::npos ? std::string::npos : end - start);
            if (!part.empty() && part != ".")
                host = AppendComponent(host, part);
            if (end == std::string::npos) break;
            start = end + 1;
        }
        return host;
    }
}
