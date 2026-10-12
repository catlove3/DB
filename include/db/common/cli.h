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

// Throws std::invalid_argument for unknown flags or invalid combinations.
CliOptions parse_cli(int argc, char* argv[]);
void print_help(std::ostream& output);

}  // namespace db
