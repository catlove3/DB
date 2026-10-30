#pragma once

#include <ostream>
#include <string>

namespace db {

// Keep process arguments separate from the eventual Engine/Catalog interfaces.
struct CliOptions {
    bool help = false;
    bool batch = false;
    bool initialize = false;
    std::string data_directory = "./data";
    std::string database;
    std::string import_file;
    std::string import_table;
};

/**
 * @brief 解析进程参数；只生成配置，不打开数据库、创建目录或导入数据。
 * @param argc 参数数量；argv[0] 是程序名，解析从 argv[1] 开始。
 * @param argv 借用的参数数组；被访问的元素必须是有效的 NUL 结尾字符串。
 * @return 按值返回配置；重复的值选项采用最后一次赋值，布尔选项置 true。
 * @throws db_error(invalid_argument) 未知选项、缺值、值为空或以 '-' 开头，
 *         或 --file/--table 不配对、--init 与选库/导入组合。
 * @note --help 仅跳过最后的组合校验，不能掩盖解析途中遇到的未知选项或缺值。
 *       不校验路径存在性、数据库名或 SQL；失败不修改 argv，也无持久化副作用。
 *       字符串分配失败等标准异常原样传播。
 */
CliOptions parse_cli(int argc, char* argv[]);

/**
 * @brief 将当前程序实际支持的用法文本写入借用的输出流。
 * @param output 必须存活至调用结束；函数不关闭流、不主动 flush。
 * @note 流错误遵循 output 的 exceptions() 设置：可能只置错误位，也可能抛出
 *       std::ios_base::failure；失败时部分帮助文本可能已输出，不回滚输出。
 */
void print_help(std::ostream& output);

}  // namespace db
