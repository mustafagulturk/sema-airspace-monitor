#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <unistd.h>

#include "sema/static_files.hpp"

using namespace sema;
namespace fs = std::filesystem;

namespace {

class StaticFiles : public ::testing::Test {
protected:
    void SetUp() override {
        base_ = fs::temp_directory_path() / ("sema_static_" + std::to_string(::getpid()) + "_" +
                                             ::testing::UnitTest::GetInstance()->current_test_info()->name());
        fs::remove_all(base_);
        root_ = base_ / "web";
        fs::create_directories(root_ / "assets");
        write(root_ / "index.html", "<html>");
        write(root_ / "assets" / "app-1234.js", "js");
        write(base_ / "secret.txt", "secret");
        fs::create_directories(base_ / "web2");
        write(base_ / "web2" / "x.txt", "x");
    }
    void TearDown() override { fs::remove_all(base_); }

    static void write(const fs::path& p, const std::string& s) { std::ofstream(p) << s; }

    fs::path base_;
    fs::path root_;
};

}  // namespace

TEST_F(StaticFiles, ServesFilesUnderTheRoot) {
    auto p = resolve_static_path(root_, "/");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->filename(), "index.html");
    p = resolve_static_path(root_, "/assets/app-1234.js");
    ASSERT_TRUE(p);
    EXPECT_EQ(p->filename(), "app-1234.js");
    p = resolve_static_path(root_, "/assets/");
    EXPECT_FALSE(p);  // no index.html there
}

TEST_F(StaticFiles, RejectsTraversalAndOddPaths) {
    EXPECT_FALSE(resolve_static_path(root_, "/../secret.txt"));
    EXPECT_FALSE(resolve_static_path(root_, "/assets/../../secret.txt"));
    EXPECT_FALSE(resolve_static_path(root_, "/..\\secret.txt"));
    EXPECT_FALSE(resolve_static_path(root_, "/../web2/x.txt"));
    EXPECT_FALSE(resolve_static_path(root_, std::string_view("/index.html\0x", 13)));
    EXPECT_FALSE(resolve_static_path(root_, "index.html"));
    EXPECT_FALSE(resolve_static_path(root_, ""));
    EXPECT_FALSE(resolve_static_path(root_, "/missing.js"));
    EXPECT_FALSE(resolve_static_path(root_, "/assets"));  // a directory, not a file
}

TEST_F(StaticFiles, RejectsSymlinksLeavingTheRoot) {
    fs::create_symlink(base_ / "secret.txt", root_ / "link.txt");
    EXPECT_FALSE(resolve_static_path(root_, "/link.txt"));
}

TEST(MimeType, KnownAndUnknownExtensions) {
    EXPECT_EQ(mime_type("index.html"), "text/html; charset=utf-8");
    EXPECT_EQ(mime_type("a/b/app.js"), "text/javascript; charset=utf-8");
    EXPECT_EQ(mime_type("x.geojson"), "application/geo+json");
    EXPECT_EQ(mime_type("x.bin"), "application/octet-stream");
}
