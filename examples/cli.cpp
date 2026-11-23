#include "kv/store.hpp"
#include <cctype>
#include <iostream>
#include <stdexcept>
#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
bool interactive() {
#ifdef _WIN32
    return _isatty(0) && _isatty(1);
#else
    return isatty(0) && isatty(1);
#endif
}

// Quotes preserve whitespace; escapes work both inside and outside quotes.
std::vector<std::string> tokenize(const std::string& line) {
    std::vector<std::string> tokens;
    std::string token;
    char quote = 0;
    bool started = false;
    for (std::size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '\\') {
            if (++i == line.size()) throw std::invalid_argument("trailing escape");
            const char escaped = line[i];
            switch (escaped) {
            case 'n': token += '\n'; break;
            case 'r': token += '\r'; break;
            case 't': token += '\t'; break;
            case '0': token += '\0'; break;
            case '\\': case '\"': case '\'': case ' ': token += escaped; break;
            default: throw std::invalid_argument("unsupported escape");
            }
            started = true;
        } else if (quote) {
            if (c == quote) quote = 0;
            else token += c;
        } else if (c == '\"' || c == '\'') {
            quote = c;
            started = true;
        } else if (std::isspace(static_cast<unsigned char>(c))) {
            if (started) { tokens.push_back(token); token.clear(); started = false; }
        } else {
            token += c;
            started = true;
        }
    }
    if (quote) throw std::invalid_argument("unterminated quote");
    if (started) tokens.push_back(token);
    return tokens;
}

void print_value(const std::string& value) {
    constexpr char hex[] = "0123456789abcdef";
    std::cout << '"';
    for (unsigned char c : value) {
        switch (c) {
        case '\\': std::cout << "\\\\"; break;
        case '"': std::cout << "\\\""; break;
        case '\n': std::cout << "\\n"; break;
        case '\r': std::cout << "\\r"; break;
        case '\t': std::cout << "\\t"; break;
        case '\0': std::cout << "\\0"; break;
        default:
            if (c < 32 || c == 127) std::cout << "\\x" << hex[c >> 4] << hex[c & 15];
            else std::cout << static_cast<char>(c);
        }
    }
    std::cout << '"';
}

void help() {
    std::cout << "SET key value          Store or replace a value\n"
                 "GET key                Read a value; (nil) if absent\n"
                 "DEL key                Delete a key; return 1 if present, else 0\n"
                 "EXISTS key             Return 1 if present, else 0\n"
                 "SCAN [begin [end]]     List sorted keys/values in [begin, end)\n"
                 "FLUSH                  Flush memory to disk (keeps data)\n"
                 "COMPACT                Merge disk tables (keeps live data)\n"
                 "HELP                   Show commands\n"
                 "EXIT / QUIT            Close the database\n"
                 "Use single/double quotes for spaces or empty strings.\n"
                 "Escapes: \\n \\r \\t \\0 \\\\ \\\" \\' and escaped space.\n"
                 "SCAN is a materialized range scan, not Redis cursor SCAN.\n";
}
}

int main(int argc, char** argv) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: kv_cli [database-directory]\nDefault: kv-data\n";
        help();
        return 0;
    }
    if (argc > 2) { std::cerr << "Usage: kv_cli [database-directory]\n"; return 1; }
    try {
        kv::Store db(argc == 2 ? argv[1] : "kv-data");
        const bool terminal = interactive();
        if (terminal) std::cout << "Persistent KV CLI. Type HELP for commands.\n";
        std::string line;
        while (true) {
            if (terminal) std::cout << "kv> " << std::flush;
            if (!std::getline(std::cin, line)) break;
            std::vector<std::string> args;
            try { args = tokenize(line); }
            catch (const std::invalid_argument& e) { std::cout << "ERR " << e.what() << '\n'; continue; }
            if (args.empty()) continue;
            auto command = args[0];
            for (char& c : command) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
            const auto size = args.size();
            const bool known = command == "SET" || command == "GET" || command == "DEL" ||
                command == "EXISTS" || command == "SCAN" || command == "FLUSH" ||
                command == "COMPACT" || command == "HELP" || command == "EXIT" || command == "QUIT";
            if (!known) { std::cout << "ERR unknown command; type HELP\n"; continue; }
            const bool valid = command == "SET" ? size == 3 :
                (command == "GET" || command == "DEL" || command == "EXISTS") ? size == 2 :
                command == "SCAN" ? size <= 3 : size == 1;
            if (!valid) { std::cout << "ERR wrong number of arguments; type HELP\n"; continue; }
            if (command == "EXIT" || command == "QUIT") break;
            if (command == "HELP") help();
            else if (command == "SET") { db.put(args[1], args[2]); std::cout << "OK\n"; }
            else if (command == "GET") {
                const auto value = db.get(args[1]);
                if (value) { print_value(*value); std::cout << '\n'; }
                else std::cout << "(nil)\n";
            } else if (command == "DEL" || command == "EXISTS") {
                const bool exists = db.get(args[1]).has_value();
                if (command == "DEL" && exists) db.erase(args[1]);
                std::cout << (exists ? 1 : 0) << '\n';
            } else if (command == "SCAN") {
                if (size == 3 && args[1] > args[2]) { std::cout << "ERR reversed scan bounds\n"; continue; }
                const auto rows = db.scan(size > 1 ? args[1] : "",
                    size > 2 ? std::optional<std::string_view>(args[2]) : std::nullopt);
                if (rows.empty()) std::cout << "(empty)\n";
                for (const auto& row : rows) {
                    print_value(row.first); std::cout << " => "; print_value(row.second); std::cout << '\n';
                }
            } else {
                if (command == "FLUSH") db.flush();
                else db.compact();
                std::cout << "OK\n";
            }
        }
        if (std::cin.bad()) throw std::runtime_error("failed to read input");
    } catch (const std::exception& e) {
        std::cerr << "ERR " << e.what() << '\n';
        return 1;
    }
}
