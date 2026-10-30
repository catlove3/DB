#include "db/storage/file_manager.h"
#include "db/common/error.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <system_error>

#include <unistd.h>

namespace fs = std::filesystem;
using namespace db;

namespace {

int failures = 0;

#define CHECK(condition) \
    do { \
        if(!(condition)) { \
            std::cerr << __FILE__ << ":" << __LINE__ \
                      << ": CHECK failed: " #condition << '\n'; \
            ++failures; \
        } \
    } while(false)

template<class F>
bool throws(error_code expected, F&& f)
{
    try {
        f();
    }
    catch(const db_error& error) {
        return error.code() == expected;
    }
    return false;
}

struct temp_dir
{
    fs::path path;

    temp_dir()
    {
        path = fs::temp_directory_path() /
               ("idbs_file_manager_test_" + std::to_string(::getpid()));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path, ec);
        if(ec)
            throw std::runtime_error("cannot create temp dir: " + ec.message());
    }

    ~temp_dir()
    {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

template<class F>
void mutate_header(const fs::path& path, F&& mutator)
{
    page p{};
    {
        std::ifstream in(path, std::ios::binary);
        if(!in)
            throw std::runtime_error("cannot open header for reading");
        in.read((char*)p.data(), (std::streamsize)p.size());
        if(!in)
            throw std::runtime_error("cannot read header page");
    }

    mutator(p);

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        if(!out)
            throw std::runtime_error("cannot open header for writing");
        out.write((const char*)p.data(), (std::streamsize)p.size());
        if(!out)
            throw std::runtime_error("cannot write header page");
    }
}

void test_normalize_path(temp_dir& tmp)
{
    const fs::path absolute = tmp.path / "norm.db";
    const fs::path dotted = tmp.path / "." / "norm.db";

    CHECK(!normalize_path(absolute.string()).empty());
    CHECK(fs::path(normalize_path(absolute.string())).is_absolute());
    CHECK(normalize_path(absolute.string()) == normalize_path(dotted.string()));

    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp.path);
    CHECK(normalize_path("norm.db") == normalize_path(absolute.string()));
    CHECK(normalize_path("./norm.db") == normalize_path(absolute.string()));
    fs::current_path(old_cwd);
}

void test_create_open_close_remove(temp_dir& tmp)
{
    file_manager fm;
    const fs::path path = tmp.path / "basic.db";

    fm.create_file(path.string());
    CHECK(fs::exists(path));
    CHECK(throws(error_code::object_exists, [&] { fm.create_file(path.string()); }));

    const file_id id = fm.open_file(path.string());
    file_handle& handle = fm.get_file(id);
    CHECK(handle.is_open());
    CHECK(throws(error_code::resource_busy, [&] { fm.open_file(path.string()); }));
    CHECK(throws(error_code::resource_busy, [&] { fm.remove_file(path.string()); }));

    page data{};
    for(std::size_t i = 0; i < data.size(); ++i)
        data[i] = (std::byte)(i & 0xff);

    CHECK(handle.allocate_page() == 1);
    handle.write_page(1, data);

    page out{};
    handle.read_page(1, out);
    CHECK(out == data);
    CHECK(throws(error_code::invalid_page, [&] { handle.write_page(0, data); }));
    CHECK(throws(error_code::invalid_page, [&] { handle.read_page(2, out); }));
    CHECK(throws(error_code::invalid_page, [&] { handle.write_page(2, data); }));

    page appended{};
    appended.fill(std::byte{0x5a});
    handle.append_page(appended);

    fm.close_file(id);
    CHECK(throws(error_code::invalid_file_handle, [&] { fm.close_file(id); }));
    CHECK(throws(error_code::invalid_file_handle, [&] { fm.get_file(id); }));

    const file_id reopened_id = fm.open_file(path.string());
    CHECK(reopened_id != id);
    file_handle& reopened = fm.get_file(reopened_id);
    CHECK(reopened.allocate_page() == 3);

    reopened.read_page(1, out);
    CHECK(out == data);
    reopened.read_page(2, out);
    CHECK(out == appended);

    fm.close_file(reopened_id);

    fm.remove_file(path.string());
    CHECK(!fs::exists(path));
    CHECK(throws(error_code::object_not_found, [&] { fm.open_file(path.string()); }));
    CHECK(throws(error_code::object_not_found, [&] { fm.remove_file(path.string()); }));
}

void test_path_aliases(temp_dir& tmp)
{
    file_manager fm;
    const fs::path path = tmp.path / "alias.db";
    const fs::path dot_alias = tmp.path / "." / "alias.db";

    fm.create_file(path.string());
    const file_id id = fm.open_file(path.string());

    CHECK(normalize_path(path.string()) == normalize_path(dot_alias.string()));
    CHECK(throws(error_code::resource_busy, [&] { fm.open_file(dot_alias.string()); }));
    CHECK(throws(error_code::resource_busy, [&] { fm.remove_file(dot_alias.string()); }));

    const fs::path old_cwd = fs::current_path();
    fs::current_path(tmp.path);
    CHECK(normalize_path("alias.db") == normalize_path(path.string()));
    CHECK(throws(error_code::resource_busy, [&] { fm.open_file("alias.db"); }));
    CHECK(throws(error_code::resource_busy, [&] { fm.remove_file("alias.db"); }));
    fs::current_path(old_cwd);

    fm.close_file(id);
    fm.remove_file(dot_alias.string());
    CHECK(!fs::exists(path));
}

void test_multiple_files(temp_dir& tmp)
{
    file_manager fm;
    const fs::path path_a = tmp.path / "a.db";
    const fs::path path_b = tmp.path / "b.db";

    fm.create_file(path_a.string());
    fm.create_file(path_b.string());

    const file_id id_a = fm.open_file(path_a.string());
    const file_id id_b = fm.open_file(path_b.string());
    CHECK(id_a != id_b);

    file_handle& handle_a = fm.get_file(id_a);
    file_handle& handle_b = fm.get_file(id_b);
    CHECK(handle_a.allocate_page() == 1);
    CHECK(handle_b.allocate_page() == 1);

    page data_a{};
    data_a.fill(std::byte{0x11});
    page data_b{};
    data_b.fill(std::byte{0x22});
    handle_a.write_page(1, data_a);
    handle_b.write_page(1, data_b);

    fm.close_file(id_a);
    fm.close_file(id_b);

    const file_id reopened_a_id = fm.open_file(path_a.string());
    const file_id reopened_b_id = fm.open_file(path_b.string());
    file_handle& reopened_a = fm.get_file(reopened_a_id);
    file_handle& reopened_b = fm.get_file(reopened_b_id);

    page out{};
    reopened_a.read_page(1, out);
    CHECK(out == data_a);
    reopened_b.read_page(1, out);
    CHECK(out == data_b);

    fm.close_file(reopened_a_id);
    fm.close_file(reopened_b_id);
}

void test_invalid_headers(temp_dir& tmp)
{
    file_manager fm;

    const fs::path bad_magic = tmp.path / "bad_magic.db";
    fm.create_file(bad_magic.string());
    mutate_header(bad_magic, [](page& p) { write_u32(p.data() + 0, 0); });
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(bad_magic.string()); }));
    fm.remove_file(bad_magic.string());

    const fs::path bad_version = tmp.path / "bad_version.db";
    fm.create_file(bad_version.string());
    mutate_header(bad_version, [](page& p) {
        write_u32(p.data() + 4, read_u32(p.data() + 4) + 1);
    });
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(bad_version.string()); }));
    fm.remove_file(bad_version.string());

    const fs::path bad_page_size = tmp.path / "bad_page_size.db";
    fm.create_file(bad_page_size.string());
    mutate_header(bad_page_size, [](page& p) {
        write_u64(p.data() + 8, page_size * 2);
    });
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(bad_page_size.string()); }));
    fm.remove_file(bad_page_size.string());

    const fs::path zero_pages = tmp.path / "zero_pages.db";
    fm.create_file(zero_pages.string());
    mutate_header(zero_pages, [](page& p) {
        write_u64(p.data() + 16, 0);
    });
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(zero_pages.string()); }));
    fm.remove_file(zero_pages.string());

    const fs::path huge_pages = tmp.path / "huge_pages.db";
    fm.create_file(huge_pages.string());
    mutate_header(huge_pages, [](page& p) {
        write_u64(p.data() + 16, (uint64_t)-1);
    });
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(huge_pages.string()); }));
    fm.remove_file(huge_pages.string());
}

void test_truncated_file(temp_dir& tmp)
{
    file_manager fm;
    const fs::path path = tmp.path / "truncated.db";

    fm.create_file(path.string());
    const file_id id = fm.open_file(path.string());
    file_handle& handle = fm.get_file(id);
    CHECK(handle.allocate_page() == 1);
    CHECK(handle.allocate_page() == 2);
    fm.close_file(id);

    fs::resize_file(path, page_size);
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(path.string()); }));

    fs::resize_file(path, 0);
    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(path.string()); }));

    fm.remove_file(path.string());
}

void test_short_file(temp_dir& tmp)
{
    file_manager fm;
    const fs::path path = tmp.path / "short.db";

    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write("abc", 3);
    }

    CHECK(throws(error_code::invalid_format, [&] { fm.open_file(path.string()); }));
    fm.remove_file(path.string());
}

void test_hardlink_identity(temp_dir& tmp)
{
    file_manager fm;
    const fs::path original = tmp.path / "hardlink.db";
    const fs::path alias = tmp.path / "hardlink_alias.db";
    fm.create_file(original.string());
    fs::create_hard_link(original, alias);
    const file_id id = fm.open_file(original.string());
    CHECK(throws(error_code::resource_busy, [&] { fm.open_file(alias.string()); }));
    CHECK(throws(error_code::resource_busy, [&] { fm.remove_file(alias.string()); }));
    CHECK(fs::exists(original) && fs::exists(alias));
    CHECK(fm.get_file(id).allocate_page() == 1);
    fm.close_file(id);
    const file_id reopened = fm.open_file(alias.string());
    CHECK(fm.get_file(reopened).get_num_pages() == 2);
    fm.close_file(reopened);
    fm.remove_file(alias.string());
    CHECK(fs::exists(original) && !fs::exists(alias));
    fm.remove_file(original.string());
}

void test_error_categories(temp_dir& tmp)
{
    file_manager fm;
    const auto missing = (tmp.path / "missing_parent" / "new.db").string();
    CHECK(throws(error_code::object_not_found, [&] { fm.create_file(missing); }));
    CHECK(throws(error_code::object_not_found, [&] { fm.open_file(missing); }));
    CHECK(throws(error_code::object_not_found, [&] { fm.remove_file(missing); }));
    file_handle closed;
    page bytes{};
    CHECK(throws(error_code::invalid_file_handle, [&] { closed.read_page(0, bytes); }));
    CHECK(throws(error_code::invalid_file_handle, [&] { closed.allocate_page(); }));
}

}

int main()
{
    try {
        temp_dir tmp;

        test_normalize_path(tmp);
        test_create_open_close_remove(tmp);
        test_path_aliases(tmp);
        test_multiple_files(tmp);
        test_invalid_headers(tmp);
        test_truncated_file(tmp);
        test_short_file(tmp);
        test_hardlink_identity(tmp);
        test_error_categories(tmp);
    }
    catch(const std::exception& e) {
        std::cerr << "unexpected exception: " << e.what() << '\n';
        return 1;
    }

    if(failures != 0) {
        std::cerr << failures << " check(s) failed\n";
        return 1;
    }

    std::cout << "file_manager tests passed\n";
    return 0;
}
