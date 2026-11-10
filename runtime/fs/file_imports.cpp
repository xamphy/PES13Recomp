// Nt* file APIs, symbolic links and the Rtl string helpers they depend on.
// All I/O completes synchronously; completion events are signalled before returning.
#include "../debug/write_watch.h"
#include "vfs.h"
#include "../gpu/memory_watch.h"

#include <kernel/kernel.h>
#include <kernel/objects.h>
#include <guest_memory.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

#ifdef _WIN32
#   include <io.h>
#   define fseek64 _fseeki64
#   define ftell64 _ftelli64
#else
#   include <unistd.h>
#   define fseek64 fseeko
#   define ftell64 ftello
#endif

namespace fs = std::filesystem;
using namespace Kernel;

namespace
{
    constexpr uint32_t STATUS_PENDING                = 0x00000103;
    constexpr uint32_t STATUS_NO_MORE_FILES          = 0x80000006;
    constexpr uint32_t STATUS_INVALID_INFO_CLASS     = 0xC0000003;
    constexpr uint32_t STATUS_END_OF_FILE            = 0xC0000011;
    constexpr uint32_t STATUS_ACCESS_DENIED          = 0xC0000022;
    constexpr uint32_t STATUS_OBJECT_NAME_NOT_FOUND  = 0xC0000034;
    constexpr uint32_t STATUS_OBJECT_NAME_COLLISION  = 0xC0000035;
    constexpr uint32_t STATUS_OBJECT_PATH_NOT_FOUND  = 0xC000003A;
    constexpr uint32_t STATUS_NOT_A_DIRECTORY        = 0xC0000103;
    constexpr uint32_t STATUS_FILE_IS_A_DIRECTORY    = 0xC00000BA;

    constexpr uint32_t FILE_DIRECTORY_FILE     = 0x00000001;
    constexpr uint32_t FILE_NON_DIRECTORY_FILE = 0x00000040;
    constexpr uint32_t FILE_ATTRIBUTE_READONLY  = 0x01;
    constexpr uint32_t FILE_ATTRIBUTE_DIRECTORY = 0x10;
    constexpr uint32_t FILE_ATTRIBUTE_NORMAL    = 0x80;
    constexpr uint64_t FILE_USE_FILE_POINTER_POSITION = 0xFFFFFFFFFFFFFFFEull;
    constexpr uint64_t FixedFileTime = 0x01CD8B0000000000ull; // 2012 - stable timestamps

    struct FileObject : KObject
    {
        std::mutex mutex;
        std::string guestPath;
        fs::path hostPath;
        std::FILE* file = nullptr;
        bool directory = false;
        bool readOnly = true;
        // Directory enumeration state
        std::vector<fs::directory_entry> entries;
        size_t nextEntry = 0;
        bool enumerated = false;

        ~FileObject() override { if (file) std::fclose(file); }
    };

    std::string ReadAnsiString(uint32_t ansiString)
    {
        if (!ansiString) return {};
        const uint16_t length = Memory::Load16(ansiString);
        const uint32_t buffer = Memory::Load32(ansiString + 4);
        std::string s;
        for (uint32_t i = 0; buffer && i < length; ++i) s.push_back(char(Memory::Load8(buffer + i)));
        return s;
    }

    std::shared_ptr<FileObject> FileFromHandle(uint32_t handle)
    {
        return std::dynamic_pointer_cast<FileObject>(LookupHandle(handle));
    }

    // OBJECT_ATTRIBUTES: RootDirectory, ObjectName (ANSI_STRING*), Attributes
    std::string GuestPathFromAttributes(uint32_t objAttr)
    {
        std::string name = ReadAnsiString(Memory::Load32(objAttr + 4));
        if (const uint32_t root = Memory::Load32(objAttr + 0))
            if (auto dir = FileFromHandle(root))
                name = dir->guestPath + "\\" + name;
        return name;
    }

    uint32_t StackArg(const PPCContext& ctx, uint32_t index) { return Memory::Load32(ctx.r1.u32 + 0x54 + (index - 8) * 8); }

    void SetIoStatus(uint32_t ioStatus, uint32_t status, uint32_t information)
    {
        if (!ioStatus) return;
        Memory::Store32(ioStatus + 0, status);
        Memory::Store32(ioStatus + 4, information);
    }

    uint64_t HostFileSize(FileObject& f)
    {
        if (f.directory) return 0;
        std::error_code ec;
        const auto size = fs::file_size(f.hostPath, ec);
        return ec ? 0 : uint64_t(size);
    }

    uint32_t Attributes(bool directory, bool readOnly)
    {
        return directory ? FILE_ATTRIBUTE_DIRECTORY : (readOnly ? FILE_ATTRIBUTE_READONLY : FILE_ATTRIBUTE_NORMAL);
    }

    // FILE_NETWORK_OPEN_INFORMATION (0x38 bytes)
    void StoreNetworkOpenInfo(uint32_t info, uint64_t size, uint32_t attributes)
    {
        for (int i = 0; i < 4; ++i) Memory::Store64(info + i * 8, FixedFileTime);
        Memory::Store64(info + 0x20, (size + 0xFFF) & ~0xFFFull); // AllocationSize
        Memory::Store64(info + 0x28, size);                       // EndOfFile
        Memory::Store32(info + 0x30, attributes);
        Memory::Store32(info + 0x34, 0);
    }

    bool WildcardMatch(const std::string& pattern, const std::string& name)
    {
        // '*' and '?' only, case-insensitive
        size_t p = 0, n = 0, star = std::string::npos, mark = 0;
        auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
        while (n < name.size())
        {
            if (p < pattern.size() && (pattern[p] == '?' || eq(pattern[p], name[n]))) { ++p; ++n; }
            else if (p < pattern.size() && pattern[p] == '*') { star = p++; mark = n; }
            else if (star != std::string::npos) { p = star + 1; n = ++mark; }
            else return false;
        }
        while (p < pattern.size() && pattern[p] == '*') ++p;
        return p == pattern.size();
    }

    uint32_t OpenFile(uint32_t handlePtr, uint32_t access, uint32_t objAttr, uint32_t ioStatus,
        uint32_t disposition, uint32_t options)
    {
        const std::string guestPath = GuestPathFromAttributes(objAttr);
        bool readOnly = true;
        const fs::path host = Fs::ResolveGuestPath(guestPath, &readOnly);
        if (host.empty())
        {
            std::printf("[fs] open \"%s\" -> no such device\n", guestPath.c_str());
            SetIoStatus(ioStatus, STATUS_OBJECT_PATH_NOT_FOUND, 0);
            return STATUS_OBJECT_PATH_NOT_FOUND;
        }

        std::error_code ec;
        const bool exists = fs::exists(host, ec);
        const bool isDirectory = exists && fs::is_directory(host, ec);
        const bool wantsWrite = (access & (0x40000000 | 0x2 | 0x4 | 0x10000000)) != 0; // GENERIC_WRITE/WRITE_DATA/APPEND/GENERIC_ALL
        uint32_t information = 1; // FILE_OPENED

        auto file = std::make_shared<FileObject>();
        file->signalState = 1; // file objects are always signalled (I/O is synchronous)
        file->guestPath = guestPath;
        file->hostPath = host;
        file->readOnly = readOnly;

        if ((options & FILE_DIRECTORY_FILE) || (isDirectory && !(options & FILE_NON_DIRECTORY_FILE)))
        {
            if (!exists)
            {
                if (readOnly || disposition == 1 /*OPEN*/ || disposition == 4 /*OVERWRITE*/)
                {
                    SetIoStatus(ioStatus, STATUS_OBJECT_NAME_NOT_FOUND, 0);
                    return STATUS_OBJECT_NAME_NOT_FOUND;
                }
                fs::create_directories(host, ec);
                information = 2; // FILE_CREATED
            }
            else if (!isDirectory)
            {
                SetIoStatus(ioStatus, STATUS_NOT_A_DIRECTORY, 0);
                return STATUS_NOT_A_DIRECTORY;
            }
            file->directory = true;
        }
        else
        {
            if (isDirectory)
            {
                SetIoStatus(ioStatus, STATUS_FILE_IS_A_DIRECTORY, 0);
                return STATUS_FILE_IS_A_DIRECTORY;
            }
            const char* mode = nullptr;
            switch (disposition)
            {
            case 1: // FILE_OPEN
                if (!exists) { std::printf("[fs] open \"%s\" -> not found\n", guestPath.c_str()); SetIoStatus(ioStatus, STATUS_OBJECT_NAME_NOT_FOUND, 0); return STATUS_OBJECT_NAME_NOT_FOUND; }
                mode = (wantsWrite && !readOnly) ? "r+b" : "rb";
                break;
            case 2: // FILE_CREATE
                if (exists) { SetIoStatus(ioStatus, STATUS_OBJECT_NAME_COLLISION, 0); return STATUS_OBJECT_NAME_COLLISION; }
                mode = "w+b"; information = 2;
                break;
            case 3: // FILE_OPEN_IF
                mode = exists ? ((wantsWrite && !readOnly) ? "r+b" : "rb") : "w+b";
                information = exists ? 1 : 2;
                break;
            case 4: // FILE_OVERWRITE
                if (!exists) { std::printf("[fs] open \"%s\" -> not found\n", guestPath.c_str()); SetIoStatus(ioStatus, STATUS_OBJECT_NAME_NOT_FOUND, 0); return STATUS_OBJECT_NAME_NOT_FOUND; }
                mode = "w+b"; information = 3;
                break;
            default: // FILE_SUPERSEDE / FILE_OVERWRITE_IF
                mode = "w+b"; information = exists ? 3 : 2;
                break;
            }
            if (readOnly && mode[0] == 'w')
            {
                SetIoStatus(ioStatus, STATUS_ACCESS_DENIED, 0);
                return STATUS_ACCESS_DENIED;
            }
            if (mode[0] == 'w')
                fs::create_directories(host.parent_path(), ec);
#ifdef _WIN32
            file->file = _wfopen(host.c_str(), std::wstring(mode, mode + std::strlen(mode)).c_str());
#else
            file->file = std::fopen(host.c_str(), mode);
#endif
            if (!file->file)
            {
                SetIoStatus(ioStatus, STATUS_ACCESS_DENIED, 0);
                return STATUS_ACCESS_DENIED;
            }
        }

        std::printf("[fs] open \"%s\"%s\n", guestPath.c_str(), file->directory ? " (directory)" : "");
        if (handlePtr) Memory::Store32(handlePtr, CreateHandle(file));
        SetIoStatus(ioStatus, STATUS_SUCCESS, information);
        return STATUS_SUCCESS;
    }

    void CompleteIo(uint32_t event, uint32_t apcRoutine)
    {
        if (event)
            if (ObjectPtr e = LookupHandle(event); e && e->kind == ObjectKind::Event)
                SetEvent(*e);
        static bool warned = false;
        if (apcRoutine && !warned)
        {
            warned = true;
            std::fprintf(stderr, "[fs] I/O completion APC requested (0x%08X) - not delivered yet\n", apcRoutine);
        }
    }
}

// NTSTATUS NtCreateFile(u32* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*,
//     u64* AllocationSize, u32 FileAttributes, u32 ShareAccess, u32 CreateDisposition, u32 CreateOptions)
GUEST_IMPORT(NtCreateFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r10.u32, StackArg(ctx, 8));
}

// NTSTATUS NtOpenFile(u32* Handle, u32 DesiredAccess, OBJECT_ATTRIBUTES*, IO_STATUS_BLOCK*, u32 ShareAccess, u32 OpenOptions)
GUEST_IMPORT(NtOpenFile)
{
    ctx.r3.u64 = OpenFile(ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, 1 /*FILE_OPEN*/, ctx.r8.u32);
}

// NTSTATUS NtReadFile(u32 Handle, u32 Event, u32 ApcRoutine, u32 ApcContext, IO_STATUS_BLOCK*,
//                     void* Buffer, u32 Length, s64* ByteOffset)
GUEST_IMPORT(NtReadFile)
{
    auto f = FileFromHandle(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!f || !f->file) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }

    size_t read;
    int64_t readOffset = -1;
    {
        std::lock_guard lock(f->mutex);
        if (offsetPtr)
        {
            const uint64_t offset = Memory::Load64(offsetPtr);
            if (offset != FILE_USE_FILE_POINTER_POSITION)
                fseek64(f->file, int64_t(offset), SEEK_SET);
        }
        readOffset = ftell64(f->file);
        // The renderer write-protects guest memory (MemoryWatch: resolve destinations, vertex pages,
        // textures). A kernel-mode ReadFile into a protected page fails instead of faulting, and
        // releasing the pages first (HostWillWrite) races with the renderer protecting them again
        // (silently short reads -> corrupted game data, 2026-11-08). So: read into a host buffer,
        // then copy with a plain user-mode memcpy, whose writes fault and are recorded normally.
        if (length)
        {
            thread_local std::vector<uint8_t> staging;
            if (staging.size() < length) staging.resize(length);
            read = std::fread(staging.data(), 1, length, f->file);
            if (read) std::memcpy(Memory::Translate(buffer), staging.data(), read);
        }
    }
    const uint32_t status = (length && read == 0) ? STATUS_END_OF_FILE : STATUS_SUCCESS;
    // PES_LOG_READS: short reads, and reads into a given physical megabyte (PES_LOG_READS=0x170).
    static const int64_t logReadsMb = [] { const char* e = std::getenv("PES_LOG_READS"); return e ? (std::strcmp(e, "all") == 0 ? int64_t(-2) : int64_t(std::strtol(e, nullptr, 0))) : -1; }();
    if (logReadsMb == -2 || (logReadsMb >= 0 && (read < length || ((buffer & 0x1FFFFFFFu) >> 20) == uint32_t(logReadsMb))))
        std::printf("[fs] read \"%s\" @0x%llX -> 0x%08X (phys 0x%08X) %u bytes requested, %zu read%s\n", f->guestPath.c_str(),
            (unsigned long long)readOffset, buffer,
            buffer & 0x1FFFFFFFu, length, read, read < length ? " (SHORT)" : "");
    // PES_WATCH_WESYS=<n>: on the n-th read that returns a WESYS block, report the first code to
    // touch it (the decompressor / parser).
    static const int64_t watchWesys = [] { const char* e = std::getenv("PES_WATCH_WESYS"); return e ? int64_t(std::atoi(e)) : -1; }();
    if (watchWesys >= 0 && read >= 4096 && (buffer & 0xFFFu) == 0)
    {
        static const uint8_t magic[8] = { 0x00, 0x01, 0x01, 'W', 'E', 'S', 'Y', 'S' };
        if (std::memcmp(Memory::Translate(buffer), magic, 8) == 0)
        {
            static int64_t seen = 0;
            if (seen++ == watchWesys)
            {
                std::printf("[fs] WESYS block #%lld from \"%s\" @0x%llX at 0x%08X (%zu bytes)\n", (long long)watchWesys,
                    f->guestPath.c_str(), (unsigned long long)readOffset, buffer, read);
                WriteWatch::WatchFirstRead(buffer);
            }
        }
    }
    SetIoStatus(ioStatus, status, uint32_t(read));
    CompleteIo(ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = status;
}

// NTSTATUS NtWriteFile(same arguments as NtReadFile)
GUEST_IMPORT(NtWriteFile)
{
    auto f = FileFromHandle(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r7.u32, buffer = ctx.r8.u32, length = ctx.r9.u32, offsetPtr = ctx.r10.u32;
    if (!f || !f->file) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }
    if (f->readOnly) { SetIoStatus(ioStatus, STATUS_ACCESS_DENIED, 0); ctx.r3.u64 = STATUS_ACCESS_DENIED; return; }

    size_t written;
    {
        std::lock_guard lock(f->mutex);
        if (offsetPtr)
        {
            const uint64_t offset = Memory::Load64(offsetPtr);
            if (offset != FILE_USE_FILE_POINTER_POSITION)
                fseek64(f->file, int64_t(offset), SEEK_SET);
        }
        written = std::fwrite(Memory::Translate(buffer), 1, length, f->file);
    }
    SetIoStatus(ioStatus, STATUS_SUCCESS, uint32_t(written));
    CompleteIo(ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

GUEST_IMPORT(NtFlushBuffersFile)
{
    if (auto f = FileFromHandle(ctx.r3.u32); f && f->file) std::fflush(f->file);
    SetIoStatus(ctx.r4.u32, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryInformationFile(u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 InfoClass)
GUEST_IMPORT(NtQueryInformationFile)
{
    auto f = FileFromHandle(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }

    const uint64_t size = HostFileSize(*f);
    uint32_t written = 0;
    switch (infoClass)
    {
    case 4: // FileBasicInformation
        for (int i = 0; i < 4; ++i) Memory::Store64(info + i * 8, FixedFileTime);
        Memory::Store32(info + 0x20, Attributes(f->directory, f->readOnly));
        written = 0x28;
        break;
    case 5: // FileStandardInformation
        Memory::Store64(info + 0x00, (size + 0xFFF) & ~0xFFFull);
        Memory::Store64(info + 0x08, size);
        Memory::Store32(info + 0x10, 1);
        Memory::Store8(info + 0x14, 0);
        Memory::Store8(info + 0x15, f->directory ? 1 : 0);
        written = 0x18;
        break;
    case 14: // FilePositionInformation
    {
        std::lock_guard lock(f->mutex);
        Memory::Store64(info, f->file ? uint64_t(ftell64(f->file)) : 0);
        written = 8;
        break;
    }
    case 34: // FileNetworkOpenInformation
        StoreNetworkOpenInfo(info, size, Attributes(f->directory, f->readOnly));
        written = 0x38;
        break;
    default:
        std::fprintf(stderr, "[fs] NtQueryInformationFile class %u not supported\n", infoClass);
        SetIoStatus(ioStatus, STATUS_INVALID_INFO_CLASS, 0);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    SetIoStatus(ioStatus, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtSetInformationFile(u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 InfoClass)
GUEST_IMPORT(NtSetInformationFile)
{
    auto f = FileFromHandle(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    if (!f) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }

    switch (infoClass)
    {
    case 14: // FilePositionInformation
        if (f->file) { std::lock_guard lock(f->mutex); fseek64(f->file, int64_t(Memory::Load64(info)), SEEK_SET); }
        break;
    case 20: // FileEndOfFileInformation
        if (f->file && !f->readOnly)
        {
            std::lock_guard lock(f->mutex);
            std::fflush(f->file);
#ifdef _WIN32
            _chsize_s(_fileno(f->file), int64_t(Memory::Load64(info)));
#else
            ftruncate(fileno(f->file), off_t(Memory::Load64(info)));
#endif
        }
        break;
    case 13: // FileDispositionInformation (delete on close)
        if (!f->readOnly && Memory::Load8(info))
        {
            std::error_code ec;
            if (f->file) { std::fclose(f->file); f->file = nullptr; }
            fs::remove(f->hostPath, ec);
        }
        break;
    default:
        std::fprintf(stderr, "[fs] NtSetInformationFile class %u ignored\n", infoClass);
        break;
    }
    SetIoStatus(ioStatus, STATUS_SUCCESS, 0);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryFullAttributesFile(OBJECT_ATTRIBUTES*, FILE_NETWORK_OPEN_INFORMATION*)
GUEST_IMPORT(NtQueryFullAttributesFile)
{
    bool readOnly = true;
    const fs::path host = Fs::ResolveGuestPath(GuestPathFromAttributes(ctx.r3.u32), &readOnly);
    std::error_code ec;
    if (host.empty() || !fs::exists(host, ec))
    {
        std::printf("[fs] query attributes \"%s\" -> not found\n",GuestPathFromAttributes(ctx.r3.u32).c_str());
        ctx.r3.u64 = STATUS_OBJECT_NAME_NOT_FOUND;
        return;
    }
    const bool dir = fs::is_directory(host, ec);
    StoreNetworkOpenInfo(ctx.r4.u32, dir ? 0 : uint64_t(fs::file_size(host, ec)), Attributes(dir, readOnly));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryVolumeInformationFile(u32 Handle, IO_STATUS_BLOCK*, void* Info, u32 Length, u32 InfoClass)
GUEST_IMPORT(NtQueryVolumeInformationFile)
{
    const uint32_t ioStatus = ctx.r4.u32, info = ctx.r5.u32, infoClass = ctx.r7.u32;
    uint32_t written = 0;
    switch (infoClass)
    {
    case 1: // FileFsVolumeInformation: CreationTime, SerialNumber, LabelLength, SupportsObjects, Label
        Memory::Store64(info + 0x00, FixedFileTime);
        Memory::Store32(info + 0x08, 0x12345678);
        Memory::Store32(info + 0x0C, 0);
        Memory::Store8(info + 0x10, 0);
        written = 0x14;
        break;
    case 3: // FileFsSizeInformation: TotalAllocationUnits, AvailableAllocationUnits, SectorsPerUnit, BytesPerSector
        Memory::Store64(info + 0x00, 0x00100000); // 16 GB in 16 KB clusters
        Memory::Store64(info + 0x08, 0x00080000); // 8 GB free
        Memory::Store32(info + 0x10, 32);
        Memory::Store32(info + 0x14, 512);
        written = 0x18;
        break;
    case 5: // FileFsAttributeInformation: Attributes, MaxComponentNameLength, NameLength, Name
        Memory::Store32(info + 0x00, 0);
        Memory::Store32(info + 0x04, 42); // FATX limit
        Memory::Store32(info + 0x08, 4);
        for (int i = 0; i < 4; ++i) Memory::Store8(info + 0x0C + i, "FATX"[i]);
        written = 0x10;
        break;
    default:
        std::fprintf(stderr, "[fs] NtQueryVolumeInformationFile class %u not supported\n", infoClass);
        SetIoStatus(ioStatus, STATUS_INVALID_INFO_CLASS, 0);
        ctx.r3.u64 = STATUS_INVALID_INFO_CLASS;
        return;
    }
    SetIoStatus(ioStatus, STATUS_SUCCESS, written);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS NtQueryDirectoryFile(u32 Handle, u32 Event, u32 ApcRoutine, u32 ApcContext, IO_STATUS_BLOCK*,
//     FILE_DIRECTORY_INFORMATION* Info, u32 Length, ANSI_STRING* FileName, u32 RestartScan)
// Returns one entry per call.
GUEST_IMPORT(NtQueryDirectoryFile)
{
    auto f = FileFromHandle(ctx.r3.u32);
    const uint32_t ioStatus = ctx.r7.u32, info = ctx.r8.u32, length = ctx.r9.u32;
    const std::string pattern = ReadAnsiString(ctx.r10.u32);
    const bool restart = StackArg(ctx, 8) != 0;
    if (!f || !f->directory) { ctx.r3.u64 = STATUS_INVALID_HANDLE; return; }

    std::lock_guard lock(f->mutex);
    if (!f->enumerated || restart)
    {
        f->entries.clear();
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(f->hostPath, ec))
            if (pattern.empty() || WildcardMatch(pattern, e.path().filename().string()))
                f->entries.push_back(e);
        f->nextEntry = 0;
        f->enumerated = true;
    }
    if (f->nextEntry >= f->entries.size())
    {
        SetIoStatus(ioStatus, STATUS_NO_MORE_FILES, 0);
        ctx.r3.u64 = STATUS_NO_MORE_FILES;
        return;
    }

    const auto& e = f->entries[f->nextEntry];
    const std::string name = e.path().filename().string();
    const uint32_t needed = 0x40 + uint32_t(name.size());
    if (length < needed) { ctx.r3.u64 = 0x80000005; return; } // STATUS_BUFFER_OVERFLOW
    ++f->nextEntry;

    std::error_code ec;
    const bool dir = e.is_directory(ec);
    const uint64_t size = dir ? 0 : uint64_t(e.file_size(ec));
    // FILE_DIRECTORY_INFORMATION
    Memory::Store32(info + 0x00, 0); // NextEntryOffset
    Memory::Store32(info + 0x04, 0); // FileIndex
    for (int i = 0; i < 4; ++i) Memory::Store64(info + 0x08 + i * 8, FixedFileTime);
    Memory::Store64(info + 0x28, size);
    Memory::Store64(info + 0x30, (size + 0xFFF) & ~0xFFFull);
    Memory::Store32(info + 0x38, Attributes(dir, f->readOnly));
    Memory::Store32(info + 0x3C, uint32_t(name.size()));
    for (size_t i = 0; i < name.size(); ++i) Memory::Store8(info + 0x40 + uint32_t(i), uint8_t(name[i]));
    SetIoStatus(ioStatus, STATUS_SUCCESS, needed);
    CompleteIo(ctx.r4.u32, ctx.r5.u32);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ------------------------------------------------------------ symbolic links
// NTSTATUS ObCreateSymbolicLink(ANSI_STRING* Link, ANSI_STRING* Target)
GUEST_IMPORT(ObCreateSymbolicLink)
{
    Fs::CreateSymbolicLink(ReadAnsiString(ctx.r3.u32), ReadAnsiString(ctx.r4.u32));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS ObDeleteSymbolicLink(ANSI_STRING* Link)
GUEST_IMPORT(ObDeleteSymbolicLink)
{
    Fs::DeleteSymbolicLink(ReadAnsiString(ctx.r3.u32));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// ------------------------------------------------------------ string helpers
// void RtlInitAnsiString(ANSI_STRING* Dest, const char* Source)
GUEST_IMPORT(RtlInitAnsiString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t len = 0;
    if (src) while (Memory::Load8(src + len)) ++len;
    Memory::Store16(dest + 0, uint16_t(len));
    Memory::Store16(dest + 2, uint16_t(src ? len + 1 : 0));
    Memory::Store32(dest + 4, src);
}

// void RtlInitUnicodeString(UNICODE_STRING* Dest, const wchar16* Source) - lengths in bytes
GUEST_IMPORT(RtlInitUnicodeString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    uint32_t chars = 0;
    if (src) while (Memory::Load16(src + chars * 2)) ++chars;
    Memory::Store16(dest + 0, uint16_t(chars * 2));
    Memory::Store16(dest + 2, uint16_t(src ? chars * 2 + 2 : 0));
    Memory::Store32(dest + 4, src);
}

GUEST_IMPORT(RtlFreeAnsiString) {}

// NTSTATUS RtlUnicodeStringToAnsiString(ANSI_STRING* Dest, UNICODE_STRING* Source, BOOLEAN Allocate)
GUEST_IMPORT(RtlUnicodeStringToAnsiString)
{
    const uint32_t dest = ctx.r3.u32, src = ctx.r4.u32;
    const uint32_t chars = Memory::Load16(src) / 2, srcBuf = Memory::Load32(src + 4);
    uint32_t destBuf = Memory::Load32(dest + 4);
    if (ctx.r5.u32 & 0xFF)
    {
        destBuf = Memory::AllocKernel(chars + 1);
        Memory::Store16(dest + 2, uint16_t(chars + 1));
        Memory::Store32(dest + 4, destBuf);
    }
    const uint32_t capacity = Memory::Load16(dest + 2);
    const uint32_t n = std::min(chars, capacity ? capacity - 1 : 0);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint16_t c = Memory::Load16(srcBuf + i * 2);
        Memory::Store8(destBuf + i, uint8_t(c < 0x80 ? c : '?'));
    }
    if (capacity) Memory::Store8(destBuf + n, 0);
    Memory::Store16(dest, uint16_t(n));
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlMultiByteToUnicodeN(wchar16* Dest, u32 DestBytes, u32* ResultBytes, const char* Src, u32 SrcBytes)
GUEST_IMPORT(RtlMultiByteToUnicodeN)
{
    const uint32_t n = std::min(ctx.r4.u32 / 2, ctx.r7.u32);
    for (uint32_t i = 0; i < n; ++i)
        Memory::Store16(ctx.r3.u32 + i * 2, Memory::Load8(ctx.r6.u32 + i));
    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, n * 2);
    ctx.r3.u64 = STATUS_SUCCESS;
}

// NTSTATUS RtlUnicodeToMultiByteN(char* Dest, u32 DestBytes, u32* ResultBytes, const wchar16* Src, u32 SrcBytes)
GUEST_IMPORT(RtlUnicodeToMultiByteN)
{
    const uint32_t n = std::min(ctx.r4.u32, ctx.r7.u32 / 2);
    for (uint32_t i = 0; i < n; ++i)
    {
        const uint16_t c = Memory::Load16(ctx.r6.u32 + i * 2);
        Memory::Store8(ctx.r3.u32 + i, uint8_t(c < 0x80 ? c : '?'));
    }
    if (ctx.r5.u32) Memory::Store32(ctx.r5.u32, n);
    ctx.r3.u64 = STATUS_SUCCESS;
}
