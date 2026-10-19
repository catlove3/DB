#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
namespace db {
	typedef std::uint64_t file_id;
	typedef std::uint64_t page_id;
	const std::uint64_t page_size = 8192;
	typedef std::array<std::byte, page_size> page;
}