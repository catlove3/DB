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
	// 运行期打开文件的编号；由 file_manager 分配，0 合法，关闭重开可能变化。
	// 以下 ID 都是整数别名，C++ 不会自动阻止 file_id/page_id/frame_id 混用。
	typedef std::uint64_t file_id;
	// 文件内从 0 开始的页号；与页当前放在哪个缓存帧无关。
	typedef std::uint64_t page_id;
	// 每页固定字节数；是磁盘格式的一部分，不是缓存可容纳的页数。
	constexpr const std::uint64_t page_size = 8192;
	// 拥有 8192 字节的页缓冲区；不含文件身份、页号或是否已修改的信息。
	typedef std::array<std::byte, page_size> page;
	// 缓存池内存槽的编号；对应槽可先后装载不同文件页，不能作为持久地址。
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
	// page 0 属于文件管理器；记录元数据占 page 1，记录数据从 page 2 开始。
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
