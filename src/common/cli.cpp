#include "db/common/cli.h"

#include <stdexcept>

namespace db {

CliOptions parse_cli(int argc, char* argv[]) {
    CliOptions options;
    for (int i = 1; i < argc; ++i) {
        const std::string argument = argv[i];
        const auto read_value = [&]() -> std::string {
            if (i + 1 >= argc || argv[i + 1][0] == '\0' || argv[i + 1][0] == '-') {
                throw std::invalid_argument("missing value for " + argument);
            }
            return argv[++i];
        };

        if (argument == "-h" || argument == "--help") {
            options.help = true;
        } else if (argument == "-b" || argument == "--batch") {
            options.batch = true;
        } else if (argument == "--init") {
            options.initialize = true;
        } else if (argument == "--data_dir") {
            options.data_directory = read_value();
        } else if (argument == "-d" || argument == "--database") {
            options.database = read_value();
        } else if (argument == "-f" || argument == "--file") {
            options.import_file = read_value();
        } else if (argument == "-t" || argument == "--table") {
            options.import_table = read_value();
        } else {
            throw std::invalid_argument("unknown argument: " + argument);
        }
    }

    if (options.help) {
        return options;
    }
    if (options.import_file.empty() != options.import_table.empty()) {
        throw std::invalid_argument("--file and --table must be supplied together");
    }
    if (options.initialize && (!options.database.empty() || !options.import_file.empty())) {
        throw std::invalid_argument("--init cannot be combined with database selection or import");
    }
    return options;
}

void print_help(std::ostream& output) {
    output << "Usage: mydb [options]\n"
              "IDBS C++17 starter; SQL execution and storage are not implemented.\n\n"
              "  -h, --help              Show this help and exit\n"
              "  -b, --batch             Read one SQL statement per line, without prompts\n"
              "      --init              Exit successfully; no persistent state exists yet\n"
              "      --data_dir PATH     Reserve the data path (default: ./data; unused yet)\n"
              "  -d, --database NAME     Select a database (not implemented)\n"
              "  -f, --file PATH         Import a file (not implemented; requires --table)\n"
              "  -t, --table NAME        Import target (requires --file)\n\n"
              "Type exit or send EOF to stop. Unsupported SQL emits !ERROR and @.\n";
}

}  // namespace db
