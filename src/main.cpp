#include "db/common/cli.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

int run_shell(const db::CliOptions& options) {
    if (!options.batch) {
        std::cout << "IDBS starter: SQL execution is not implemented.\n"
                     "Type exit or send EOF to stop.\n";
    }

    std::string line;
    while (true) {
        if (!options.batch) {
            std::cout << "mydb> " << std::flush;
        }
        if (!std::getline(std::cin, line)) {
            if (std::cin.bad()) {
                std::cerr << "mydb: failed to read standard input\n";
                return 1;
            }
            break;
        }
        const std::string statement = trim(line);
        if (statement.empty()) {
            continue;
        }
        if (statement == "exit" || statement == "exit;") {
            break;
        }

        // TODO: Parser -> AST -> Engine::execute -> Formatter. Do not report
        // success before these layers exist. Keep one flushed block per line
        // so a future interactive course runner can proceed without hanging.
        std::cout << "!ERROR\nSQL execution is not implemented in this starter\n@\n"
                  << std::flush;
    }

    // TODO: Explicit Engine::shutdown() once storage and buffers are added.
    return 0;
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const db::CliOptions options = db::parse_cli(argc, argv);
        if (options.help) {
            db::print_help(std::cout);
            return 0;
        }
        if (options.initialize) {
            // No storage exists yet. Initialization is intentionally a no-op;
            // add owned-data cleanup and catalog creation with the storage layer.
            return 0;
        }
        if (!options.import_file.empty()) {
            std::cerr << "mydb: file import is not implemented\n";
            return 1;
        }
        if (!options.database.empty()) {
            std::cerr << "mydb: database selection is not implemented\n";
            return 1;
        }
        return run_shell(options);
    } catch (const std::exception& error) {
        std::cerr << "mydb: " << error.what() << '\n';
        return 1;
    }
}
