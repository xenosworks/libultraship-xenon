#include "O2rArchive.h"

#include "Context.h"
#include "window/Window.h"
#include "spdlog/spdlog.h"

#ifdef __XSDK__
#include <chrono>
#include <cstdlib>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace Ship {
#ifdef __XSDK__
// On the console, each asset a level loads from an archive on the disk stalled the thread that asked for it, drawing or
// audio, by up to 95 ms; an archive up to this size is read into memory once when opened, and its assets load from
// there (xsdk:docs/HARDWARE.md, "Starship's assets from memory, milestone 61").
// shortcut: a bigger one, such as a large texture pack, stays on the disk; read it in parts if one stalls.
static constexpr off_t kInMemoryBytes = 64 * 1024 * 1024;

// The archive opened from a copy in memory, which libzip frees with the archive; nullptr if it's too big or can't be
// read, to open it from the disk instead. Read only: nothing in the copy goes back to the disk.
static zip_t* OpenInMemory(const std::string& path) {
    const auto start = std::chrono::steady_clock::now();
    const int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) {
        return nullptr;
    }
    struct stat info;
    char* data = nullptr;
    size_t read_bytes = 0;
    if (fstat(fd, &info) == 0 && info.st_size > 0 && info.st_size <= kInMemoryBytes) {
        data = static_cast<char*>(malloc(info.st_size));
        while (data != nullptr && read_bytes < static_cast<size_t>(info.st_size)) {
            const ssize_t n = read(fd, data + read_bytes, info.st_size - read_bytes);
            if (n <= 0) {
                break;
            }
            read_bytes += n;
        }
    }
    close(fd);
    if (data == nullptr || read_bytes != static_cast<size_t>(info.st_size)) {
        free(data);
        return nullptr;
    }
    zip_error_t error;
    zip_error_init(&error);
    zip_source_t* source = zip_source_buffer_create(data, read_bytes, 1, &error);
    zip_t* archive = nullptr;
    if (source == nullptr) {
        free(data);
    } else if ((archive = zip_open_from_source(source, ZIP_RDONLY, &error)) == nullptr) {
        zip_source_free(source);
    }
    zip_error_fini(&error);
    if (archive != nullptr) {
        SPDLOG_INFO(
            "Read {} into memory, {} bytes in {} ms", path, read_bytes,
            std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count());
    }
    return archive;
}
#endif

O2rArchive::O2rArchive(const std::string& archivePath) : Archive(archivePath) {
}

O2rArchive::~O2rArchive() {
    SPDLOG_TRACE("destruct o2rarchive: {}", GetPath());
    Close();
}

std::shared_ptr<File> O2rArchive::LoadFile(uint64_t hash) {
    const std::string& filePath =
        *Context::GetInstance()->GetResourceManager()->GetArchiveManager()->HashToString(hash);
    return LoadFile(filePath);
}

std::shared_ptr<File> O2rArchive::LoadFile(const std::string& filePath) {
    if (mZipArchive == nullptr) {
        SPDLOG_TRACE("Failed to open file {} from zip archive {}. Archive not open.", filePath, GetPath());
        return nullptr;
    }

    auto zipEntryIndex = zip_name_locate(mZipArchive, filePath.c_str(), 0);
    if (zipEntryIndex < 0) {
        SPDLOG_TRACE("Failed to find file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    struct zip_stat zipEntryStat;
    zip_stat_init(&zipEntryStat);
    if (zip_stat_index(mZipArchive, zipEntryIndex, 0, &zipEntryStat) != 0) {
        SPDLOG_TRACE("Failed to get entry information for file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    // Filesize 0, no logging needed
    if (zipEntryStat.size == 0) {
        SPDLOG_TRACE("Failed to load file {}; filesize 0", filePath, GetPath());
        return nullptr;
    }

    struct zip_file* zipEntryFile = zip_fopen_index(mZipArchive, zipEntryIndex, 0);
    if (!zipEntryFile) {
        SPDLOG_TRACE("Failed to open file {} in zip archive  {}.", filePath, GetPath());
        return nullptr;
    }

    auto fileToLoad = std::make_shared<File>();
    fileToLoad->Buffer = std::make_shared<std::vector<char>>(zipEntryStat.size);

    if (zip_fread(zipEntryFile, fileToLoad->Buffer->data(), zipEntryStat.size) < 0) {
        SPDLOG_TRACE("Error reading file {} in zip archive  {}.", filePath, GetPath());
    }

    if (zip_fclose(zipEntryFile) != 0) {
        SPDLOG_TRACE("Error closing file {} in zip archive  {}.", filePath, GetPath());
    }

    fileToLoad->IsLoaded = true;

    return fileToLoad;
}

bool O2rArchive::Open() {
#ifdef __XSDK__
    mZipArchive = OpenInMemory(GetPath());
    if (mZipArchive == nullptr)
#endif
        mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to load zip file \"{}\"", GetPath());
        return false;
    }

    auto zipNumEntries = zip_get_num_entries(mZipArchive, 0);
    for (auto i = 0; i < zipNumEntries; i++) {
        auto zipEntryName = zip_get_name(mZipArchive, i, 0);

        // It is possible for directories to have entries in a zip
        // file, we don't want those indexed as files in the archive
        if (zipEntryName[strlen(zipEntryName) - 1] == '/') {
            continue;
        }

        IndexFile(zipEntryName);
    }

    return true;
}

bool O2rArchive::Close() {
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Cannot close zip file. Zip file not loaded. \"{}\"", GetPath());
        return false;
    }

    if (zip_close(mZipArchive) == -1) {
        SPDLOG_ERROR("Failed to close zip file \"{}\"", GetPath());
        return false;
    }

    return true;
}

bool O2rArchive::WriteFile(const std::string& filePath, const std::vector<uint8_t>& data) {
    if (!mZipArchive) {
        SPDLOG_ERROR("Cannot write to zip: Archive is not open.");
        return false;
    }

    // Create a new zip source from the data buffer
    zip_source_t* source = zip_source_buffer(mZipArchive, data.data(), data.size(), 0);
    if (!source) {
        SPDLOG_ERROR("Failed to create zip source for file \"{}\"", filePath);
        return false;
    }

    // Add or replace the file in the zip archive
    if (zip_file_add(mZipArchive, filePath.c_str(), source, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE) < 0) {
        SPDLOG_ERROR("Failed to add file \"{}\" to ZIP", filePath);
        zip_source_free(source);
        return false;
    }

    // Save changes to disk
    if (zip_close(mZipArchive) < 0) {
        zip_error_t* error = zip_get_error(mZipArchive);
        SPDLOG_ERROR("Failed to save changes to zip archive: {} ({})", zip_error_strerror(error),
                     zip_error_code_zip(error));
        zip_discard(mZipArchive); // Close zip and discard changes
        return false;
    }

    SPDLOG_INFO("Successfully wrote file: {}", filePath);

    // Reopen the zip file so that it may continued to be used by libultraship
    mZipArchive = zip_open(GetPath().c_str(), ZIP_CREATE, nullptr);
    if (mZipArchive == nullptr) {
        SPDLOG_ERROR("Failed to reopen zip file after writing.");
        return false;
    }

    IndexFile(filePath);

    // Success
    return true;
}

} // namespace Ship
