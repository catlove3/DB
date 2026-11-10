#include "db/common/cli.h"

#include <exception>
#include <iostream>
#include <string>

namespace {

/*
    FUNCTION : TRIM
    docs:
        Return an owning copy without leading/trailing spaces, tabs, CR or LF.
        Whitespace-only input returns an empty string; internal and other Unicode whitespace remain.
        Borrow text without modifying it. Allocation failures propagate; no session/file state changes.
*/
std::string trim(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

/*
    FUNCTION : RUN SHELL
    docs:
        Read standard input one line at a time using options already validated by parse_cli.
        Skip blank lines; exit/exit; or normal EOF returns 0. Interactive mode also prints prompts.
        Every other nonempty line emits and flushes one unsupported-SQL error block.
        The shell does not yet parse SQL, open databases or perform persistent shutdown.

    errors:
        Bad input-stream state prints a diagnostic and returns 1; string/stream exceptions reach main.
        Output cannot be rolled back, and the current shell does not explicitly inspect stdout failures.
*/
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

/*
    FUNCTION : MAIN
    docs:
        Accept runtime argc/argv and validate options with parse_cli before selecting an action.
        Help, the current no-op --init and normal shell termination return 0.
        Database selection and import are not wired up and return 1 with a diagnostic.
        This entry point does not yet connect storage or create/reset a data directory.

    errors:
        Catch std::exception, print to stderr and return 1; other exception types propagate.
        Returning 0 from this shell does not imply that SQL was executed.
*/
int main(int argc, char* argv[]) {
    try {
        const db::CliOptions options = db::parse_cli(argc, argv);
        if (options.help) {
            db::print_help(std::cout);
            return 0;
        }
        if (options.initialize) {
            // Catalog initialization exists in the library but is not wired into this shell yet.
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
