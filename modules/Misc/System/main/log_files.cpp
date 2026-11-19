// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include "log_files.hpp"

#include <algorithm>
#include <system_error>

#include <everest/run_application/run_application.hpp>

namespace module::main {

namespace fs = std::filesystem;

namespace {
std::chrono::system_clock::time_point to_system_time(fs::file_time_type time) {
    // C++17 has no clock_cast: carry the distance from "now" over to the system clock
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(time - fs::file_time_type::clock::now() +
                                                                             std::chrono::system_clock::now());
}
} // namespace

std::vector<LogFile> list_log_files(const fs::path& directory, const std::string& name_filter) {
    std::vector<LogFile> files;
    std::error_code ec;
    fs::directory_iterator it(directory, ec);
    if (ec) {
        return files;
    }
    for (; it != fs::directory_iterator(); it.increment(ec)) {
        if (ec) {
            break;
        }
        const auto& entry = *it;
        std::error_code entry_ec;
        if (!entry.is_regular_file(entry_ec) || entry_ec) {
            continue;
        }
        if (entry.path().filename().string().find(name_filter) == std::string::npos) {
            continue;
        }
        const auto size = entry.file_size(entry_ec);
        if (entry_ec || size == 0) {
            continue;
        }
        const auto last_write = entry.last_write_time(entry_ec);
        if (entry_ec) {
            continue;
        }
        files.push_back({entry.path(), to_system_time(last_write), size});
    }
    return files;
}

std::vector<LogFile> select_log_files(std::vector<LogFile> files,
                                      const std::optional<std::chrono::system_clock::time_point>& oldest,
                                      const std::optional<std::chrono::system_clock::time_point>& latest,
                                      std::uintmax_t max_total_size) {
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) {
        return a.last_write < b.last_write || (a.last_write == b.last_write && a.path < b.path);
    });

    std::vector<LogFile> picked;
    for (std::size_t i = 0; i < files.size(); ++i) {
        if (oldest.has_value() && files[i].last_write < oldest.value()) {
            continue; // everything in it is older than requested
        }
        if (latest.has_value() && i > 0 && files[i - 1].last_write >= latest.value()) {
            continue; // everything in it was written after the requested range
        }
        picked.push_back(files[i]);
    }

    if (max_total_size > 0) {
        std::uintmax_t total = 0;
        auto first_kept = picked.size();
        while (first_kept > 0) {
            const auto size = picked[first_kept - 1].size;
            if (first_kept < picked.size() && total + size > max_total_size) {
                break;
            }
            total += size;
            --first_kept;
        }
        picked.erase(picked.begin(), picked.begin() + static_cast<std::ptrdiff_t>(first_kept));
    }
    return picked;
}

bool pack_log_files(const fs::path& directory, const std::vector<std::string>& file_names, const fs::path& archive,
                    const std::shared_ptr<std::atomic_bool>& stop_requested) {
    using namespace everest::run_application;

    // "--" ends the options, so that no file name is taken for one
    std::vector<std::string> args = {"-czf", archive.string(), "-C", directory.string(), "--"};
    args.insert(args.end(), file_names.begin(), file_names.end());
    RunOptions options;
    options.stop_requested = stop_requested;
    const auto result = run_application("tar", args, options);
    if (stop_requested != nullptr && stop_requested->load()) {
        return false;
    }
    // GNU tar exits with 1 when a file changed while it was read: the archive is still complete up to that point. A tar
    // that cannot be started also reports 1, but then there is no archive.
    if (result.exit_code != 0 && result.exit_code != 1) {
        return false;
    }
    std::error_code ec;
    const auto size = fs::file_size(archive, ec);
    return !ec && size > 0;
}

} // namespace module::main
