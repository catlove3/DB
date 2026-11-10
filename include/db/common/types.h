#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <vector>
#include <string>
#include <variant>

namespace db {
	// Runtime file ID; 0 is valid and reopening can assign a different ID.
	// These integer aliases do not prevent mixing file/page/frame IDs at compile time.
	typedef std::uint64_t file_id;
	// Zero-based page number within a file, independent of its current cache frame.
	typedef std::uint64_t page_id;
	// Bytes per page in the persistent format, not the number of cache frames.
	constexpr const std::uint64_t page_size = 8192;
	// Owned page bytes; identity and dirty state are stored separately.
	typedef std::array<std::byte, page_size> page;
	// Reusable buffer-pool slot, never a persistent record address.
	typedef std::uint32_t frame_id;
	typedef std::uint32_t slot_id;
	// Persistent catalog IDs start at 1; 0 means unassigned, unlike runtime file_id.
	typedef std::uint64_t database_id;
	typedef std::uint64_t table_id;
	typedef std::uint64_t index_id;
	typedef std::uint64_t constraint_id;
	typedef std::uint32_t column_id;
	constexpr database_id invalid_database_id = 0;
	constexpr table_id invalid_table_id = 0;
	constexpr index_id invalid_index_id = 0;
	constexpr constraint_id invalid_constraint_id = 0;
	constexpr column_id invalid_column_id = 0;

	constexpr page_id invalid_page_id = std::numeric_limits<page_id>::max();
	constexpr slot_id invalid_slot_id = std::numeric_limits<slot_id>::max();
	// File header at page 0, record metadata at page 1, record data from page 2.
	constexpr page_id record_meta_page = 1;
	constexpr page_id first_record_data_page = 2;

	typedef std::vector<std::byte> record_bytes;

	struct rid
	{
		page_id pid;
		slot_id sid;
		bool operator==(const rid & r) const noexcept
		{
			return pid == r.pid && sid == r.sid;
		}
		rid():pid(invalid_page_id),sid(invalid_slot_id){}
	};

	struct scan_item
	{
		rid id;
		record_bytes bytes;
	};

	typedef std::variant<
    std::monostate,  // SQL NULL
    std::int32_t,   // INT
    double,         // FLOAT
    std::string     // VARCHAR
	> value;
	typedef std::vector<value> row;


	enum class data_type
	{
		INT,
		FLOAT,
		VARCHAR
	};

	// Shared operation progress; callers keep this object even when a service throws.
	enum class statement_phase
	{
		preparing,
		applying_row,
		between_rows,
		metadata_published
	};

	struct statement_context
	{
		statement_phase phase = statement_phase::preparing;
		std::uint64_t completed_rows = 0;
		std::optional<std::string> input_path;
		std::optional<std::size_t> input_line;
	};

}
