// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest
#ifndef MAIN_LOG_FILES_HPP
#define MAIN_LOG_FILES_HPP

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace module::main {

/// \brief A log file found in a log directory.
struct LogFile {
    std::filesystem::path path;
    std::chrono::system_clock::time_point last_write; ///< the time of the newest entry the file can hold
    std::uintmax_t size{0};
};

/// \brief Lists the regular, non-empty files directly in \p directory whose name contains \p name_filter.
///
/// An empty \p name_filter takes every file. A missing or unreadable directory yields no files.
std::vector<LogFile> list_log_files(const std::filesystem::path& directory, const std::string& name_filter);

/// \brief Picks the files of a rotated log set that hold entries between \p oldest and \p latest, within a size limit.
///
/// Ordered by last write, each file holds the entries written after the next older file's last write, up to its own.
/// A file is picked when that span overlaps [\p oldest, \p latest]; an unset bound does not limit. The start of the
/// oldest file is not known, so only its last write is compared.
///
/// With \p max_total_size above zero, the newest picked files are kept while their sizes add up to no more than it,
/// and older ones are left out. The newest picked file is always kept, so a file larger than the limit is still
/// returned on its own.
///
/// \returns the picked files, oldest first
std::vector<LogFile> select_log_files(std::vector<LogFile> files,
                                      const std::optional<std::chrono::system_clock::time_point>& oldest,
                                      const std::optional<std::chrono::system_clock::time_point>& latest,
                                      std::uintmax_t max_total_size);

/// \brief Packs the files \p file_names in \p directory into the gzip-compressed tar archive \p archive.
///
/// A file that grows while it is read, like the log being written, is packed up to the size it had when packing
/// reached it. Setting \p stop_requested stops the packing.
/// \returns true when the archive was written, false on failure or when stopped
bool pack_log_files(const std::filesystem::path& directory, const std::vector<std::string>& file_names,
                    const std::filesystem::path& archive, const std::shared_ptr<std::atomic_bool>& stop_requested);

} // namespace module::main

#endif // MAIN_LOG_FILES_HPP
