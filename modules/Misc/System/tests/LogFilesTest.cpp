// SPDX-License-Identifier: Apache-2.0
// Copyright Pionix GmbH and Contributors to EVerest

#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include <stdlib.h>

#include <everest/run_application/run_application.hpp>

#include "log_files.hpp"

namespace module::main {
namespace {

namespace fs = std::filesystem;
using std::chrono::hours;
using std::chrono::minutes;
using std::chrono::system_clock;

const auto T0 = system_clock::time_point{} + hours(24 * 365 * 50);

LogFile file(const std::string& name, minutes written, std::uintmax_t size = 100) {
    return {name, T0 + written, size};
}

std::vector<std::string> names(const std::vector<LogFile>& files) {
    std::vector<std::string> result;
    for (const auto& f : files) {
        result.push_back(f.path.filename().string());
    }
    return result;
}

// A rotated set: log.2 holds up to T0+10min, log.1 from then up to T0+20min, log from then up to T0+30min
const std::vector<LogFile> ROTATED{file("log", minutes(30)), file("log.2", minutes(10)), file("log.1", minutes(20))};

TEST(LogFiles, without_bounds_every_file_is_picked_oldest_first) {
    EXPECT_EQ(names(select_log_files(ROTATED, std::nullopt, std::nullopt, 0)),
              (std::vector<std::string>{"log.2", "log.1", "log"}));
}

TEST(LogFiles, files_last_written_before_the_oldest_timestamp_are_left_out) {
    EXPECT_EQ(names(select_log_files(ROTATED, T0 + minutes(15), std::nullopt, 0)),
              (std::vector<std::string>{"log.1", "log"}));
}

TEST(LogFiles, a_file_last_written_at_the_oldest_timestamp_is_kept) {
    EXPECT_EQ(names(select_log_files(ROTATED, T0 + minutes(20), std::nullopt, 0)),
              (std::vector<std::string>{"log.1", "log"}));
}

TEST(LogFiles, files_started_after_the_latest_timestamp_are_left_out) {
    EXPECT_EQ(names(select_log_files(ROTATED, std::nullopt, T0 + minutes(15), 0)),
              (std::vector<std::string>{"log.2", "log.1"}));
}

TEST(LogFiles, a_file_started_at_the_latest_timestamp_is_left_out) {
    // log.1 holds what was written after T0+10min, so nothing at T0+10min itself
    EXPECT_EQ(names(select_log_files(ROTATED, std::nullopt, T0 + minutes(10), 0)), (std::vector<std::string>{"log.2"}));
}

TEST(LogFiles, a_range_inside_one_file_picks_that_file) {
    EXPECT_EQ(names(select_log_files(ROTATED, T0 + minutes(12), T0 + minutes(18), 0)),
              (std::vector<std::string>{"log.1"}));
}

TEST(LogFiles, the_oldest_file_is_kept_when_the_range_ends_before_it_does) {
    // its start is not known, so it may hold entries from before the latest timestamp
    EXPECT_EQ(names(select_log_files(ROTATED, std::nullopt, T0 - hours(1), 0)), (std::vector<std::string>{"log.2"}));
}

TEST(LogFiles, a_range_after_every_file_picks_nothing) {
    EXPECT_TRUE(select_log_files(ROTATED, T0 + minutes(31), std::nullopt, 0).empty());
}

TEST(LogFiles, no_files_pick_nothing) {
    EXPECT_TRUE(select_log_files({}, std::nullopt, std::nullopt, 0).empty());
}

TEST(LogFiles, the_size_limit_keeps_the_newest_files_that_fit) {
    const std::vector<LogFile> files{file("log", minutes(30), 40), file("log.1", minutes(20), 40),
                                     file("log.2", minutes(10), 40)};
    EXPECT_EQ(names(select_log_files(files, std::nullopt, std::nullopt, 100)),
              (std::vector<std::string>{"log.1", "log"}));
    EXPECT_EQ(names(select_log_files(files, std::nullopt, std::nullopt, 120)),
              (std::vector<std::string>{"log.2", "log.1", "log"}));
}

TEST(LogFiles, the_newest_file_is_kept_even_when_larger_than_the_size_limit) {
    const std::vector<LogFile> files{file("log", minutes(30), 500), file("log.1", minutes(20), 10)};
    EXPECT_EQ(names(select_log_files(files, std::nullopt, std::nullopt, 100)), (std::vector<std::string>{"log"}));
}

TEST(LogFiles, the_size_limit_applies_after_the_time_range) {
    const std::vector<LogFile> files{file("log", minutes(30), 60), file("log.1", minutes(20), 60),
                                     file("log.2", minutes(10), 60)};
    // the range leaves out "log", so the limit keeps log.1 rather than the newest file of the directory
    EXPECT_EQ(names(select_log_files(files, std::nullopt, T0 + minutes(15), 100)), (std::vector<std::string>{"log.1"}));
}

class LogDirectoryTest : public ::testing::Test {
protected:
    fs::path dir;

    void SetUp() override {
        std::string dir_template = (fs::temp_directory_path() / "system-log-files-test-XXXXXX").string();
        ASSERT_NE(mkdtemp(dir_template.data()), nullptr);
        dir = dir_template;
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void write(const std::string& name, const std::string& content) {
        std::ofstream(dir / name) << content;
    }
};

TEST_F(LogDirectoryTest, lists_the_non_empty_regular_files_whose_name_contains_the_filter) {
    write("libocpp_201.security.log", "event 3");
    write("libocpp_201.security.log.0", "event 1\nevent 2");
    write("libocpp_201.log", "message");
    write("empty.security.log", "");
    fs::create_directory(dir / "sub.security.log");

    auto listed = names(list_log_files(dir, ".security.log"));
    std::sort(listed.begin(), listed.end());
    EXPECT_EQ(listed, (std::vector<std::string>{"libocpp_201.security.log", "libocpp_201.security.log.0"}));
}

TEST_F(LogDirectoryTest, an_empty_filter_lists_every_file) {
    write("everest.log", "a");
    write("everest.log.1", "b");
    EXPECT_EQ(list_log_files(dir, "").size(), 2);
}

TEST_F(LogDirectoryTest, a_missing_directory_lists_nothing) {
    EXPECT_TRUE(list_log_files(dir / "missing", "").empty());
}

TEST_F(LogDirectoryTest, the_last_write_and_size_come_from_the_file) {
    write("everest.log", "12345");
    fs::last_write_time(dir / "everest.log", fs::file_time_type::clock::now() - hours(1));

    const auto listed = list_log_files(dir, "");
    ASSERT_EQ(listed.size(), 1);
    EXPECT_EQ(listed[0].size, 5);
    const auto expected = system_clock::now() - hours(1);
    EXPECT_LT(std::chrono::abs(listed[0].last_write - expected), std::chrono::seconds(5));
}

TEST_F(LogDirectoryTest, packs_the_files_into_one_compressed_archive) {
    write("everest.log", "new");
    write("everest.log.1", "old");
    // a name that tar would take for an option without the "--" before the names
    write("--version", "odd");
    const auto archive = dir / "out.tar.gz";

    ASSERT_TRUE(pack_log_files(dir, {"everest.log.1", "everest.log", "--version"}, archive, nullptr));

    everest::run_application::RunOptions options;
    const auto listing =
        everest::run_application::run_application("tar", {"-tzf", archive.string(), "--"}, options).split_output;
    EXPECT_EQ(listing, (std::vector<std::string>{"everest.log.1", "everest.log", "--version"}));
}

TEST_F(LogDirectoryTest, packing_a_missing_file_fails) {
    write("everest.log", "new");
    EXPECT_FALSE(pack_log_files(dir, {"everest.log", "gone.log"}, dir / "out.tar.gz", nullptr));
}

TEST_F(LogDirectoryTest, stopped_packing_fails) {
    write("everest.log", "new");
    const auto stop = std::make_shared<std::atomic_bool>(true);
    EXPECT_FALSE(pack_log_files(dir, {"everest.log"}, dir / "out.tar.gz", stop));
}

} // namespace
} // namespace module::main
