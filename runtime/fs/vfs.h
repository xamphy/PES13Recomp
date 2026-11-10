#pragma once
// Virtual file system: Xbox 360 object paths -> host paths.
//
//   \Device\Cdrom0\...  (game:\, d:\)      -> <game root>          (the dumped disc, read-only)
//   \Device\Harddisk0\Partition1\...        -> <user root>\hdd
//   cache:\  (title cache partition)        -> <user root>\cache
// Titles can add their own links with ObCreateSymbolicLink.
#include <filesystem>
#include <string>

namespace Fs
{
    void Init(const std::filesystem::path& gameRoot, const std::filesystem::path& userRoot);

    // Normalizes a guest path (case, separators, \??\ prefix) and resolves symbolic
    // links and devices. Returns an empty path if no device matches.
    std::filesystem::path ResolveGuestPath(const std::string& guestPath, bool* readOnly = nullptr);

    void CreateSymbolicLink(const std::string& link, const std::string& target);
    void DeleteSymbolicLink(const std::string& link);

    // Content packages (saves): mounts a host directory as the drive "<root>:" (e.g. "save:").
    void MountContent(const std::string& root, const std::filesystem::path& hostDirectory);
    void UnmountContent(const std::string& root);
    std::filesystem::path UserRoot();
}
