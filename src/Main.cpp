#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include "svfmt/FormatStyle.h"
#include "svfmt/Formatter.h"

using namespace svfmt;

static void printHelp() {
    std::cout << "Usage: svfmt [options] <file>...\n\n"
              << "SystemVerilog code formatter re-implementing Verible rules with slang.\n\n"
              << "Options:\n"
              << "  -i, --inplace                          Overwrite input file(s) directly.\n"
              << "  --indentation_spaces <n>               Spaces per indent level (default: 2).\n"
              << "  --wrap_spaces <n>                      Extra spaces applied when wrapping (default: 4).\n"
              << "  --column_limit <n>                     Target line length limit (default: 100).\n"
              << "  --assignment_statement_alignment <m>   Alignment of assignments (align, flush-left, preserve).\n"
              << "  --named_port_alignment <m>             Alignment of named port connections.\n"
              << "  --named_parameter_alignment <m>        Alignment of named parameter assignments.\n"
              << "  --port_declarations_alignment <m>      Alignment of port declarations.\n"
              << "  --parameter_declaration_alignment <m>  Alignment of parameter declarations.\n"
              << "  --case_items_alignment <m>             Alignment of case items.\n"
              << "  --struct_union_members_alignment <m>   Alignment of struct/union members.\n"
              << "  --compact_indexing_and_selections <b>  Compact binary expressions in slices (true/false).\n"
              << "  --wrap_end_else_clauses <b>            Split end and else with newline (true/false).\n"
              << "  --flagfile <path>                      Read options from a flag file.\n"
              << "  --verify                               Check if files are formatted, exit non-zero if not.\n"
              << "  -h, --help                             Display this help message.\n";
}

static bool parseFlag(std::string_view arg, FormatStyle& style, bool& inPlace, bool& verify, std::vector<std::string>& files);

static void loadFlagfile(const std::string& path, FormatStyle& style, bool& inPlace, bool& verify, std::vector<std::string>& files) {
    std::ifstream file(path);
    if (!file.is_open()) {
        std::cerr << "Warning: Could not open flagfile: " << path << "\n";
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        parseFlag(line, style, inPlace, verify, files);
    }
}

static bool parseFlag(std::string_view arg, FormatStyle& style, bool& inPlace, bool& verify, std::vector<std::string>& files) {
    if (arg == "-i" || arg == "--inplace") {
        inPlace = true;
        return true;
    }
    if (arg == "--verify") {
        verify = true;
        return true;
    }
    if (arg == "-h" || arg == "--help") {
        printHelp();
        std::exit(0);
    }

    auto split = arg.find('=');
    std::string_view key = (split != std::string_view::npos) ? arg.substr(0, split) : arg;
    std::string_view val = (split != std::string_view::npos) ? arg.substr(split + 1) : "";

    if (key == "--indentation_spaces" && !val.empty()) {
        style.indentation_spaces = std::stoi(std::string(val));
        return true;
    }
    if (key == "--wrap_spaces" && !val.empty()) {
        style.wrap_spaces = std::stoi(std::string(val));
        return true;
    }
    if (key == "--column_limit" && !val.empty()) {
        style.column_limit = std::stoi(std::string(val));
        return true;
    }
    if (key == "--assignment_statement_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.assignment_statement_alignment);
        return true;
    }
    if (key == "--named_port_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.named_port_alignment);
        return true;
    }
    if (key == "--named_parameter_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.named_parameter_alignment);
        return true;
    }
    if (key == "--port_declarations_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.port_declarations_alignment);
        return true;
    }
    if (key == "--parameter_declaration_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.parameter_declaration_alignment);
        return true;
    }
    if (key == "--case_items_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.case_items_alignment);
        return true;
    }
    if (key == "--struct_union_members_alignment" && !val.empty()) {
        parseAlignmentPolicy(val, style.struct_union_members_alignment);
        return true;
    }
    if (key == "--compact_indexing_and_selections" && !val.empty()) {
        style.compact_indexing_and_selections = (val == "true" || val == "1");
        return true;
    }
    if (key == "--wrap_end_else_clauses" && !val.empty()) {
        style.wrap_end_else_clauses = (val == "true" || val == "1");
        return true;
    }
    if (key == "--flagfile" && !val.empty()) {
        loadFlagfile(std::string(val), style, inPlace, verify, files);
        return true;
    }

    if (!arg.empty() && arg[0] != '-') {
        files.push_back(std::string(arg));
        return true;
    }

    return false;
}

int main(int argc, char* argv[]) {
    FormatStyle style;
    bool inPlace = false;
    bool verify = false;
    std::vector<std::string> files;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "--indentation_spaces" && i + 1 < argc) {
            style.indentation_spaces = std::stoi(argv[++i]);
        } else if (arg == "--wrap_spaces" && i + 1 < argc) {
            style.wrap_spaces = std::stoi(argv[++i]);
        } else if (arg == "--column_limit" && i + 1 < argc) {
            style.column_limit = std::stoi(argv[++i]);
        } else if (arg == "--flagfile" && i + 1 < argc) {
            loadFlagfile(argv[++i], style, inPlace, verify, files);
        } else {
            parseFlag(arg, style, inPlace, verify, files);
        }
    }

    // If no input files specified, read from stdin
    if (files.empty()) {
        std::string content((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        Formatter fmt(style);
        std::string formatted = fmt.format(content);
        if (verify) {
            if (formatted != content) {
                std::cerr << "stdin requires formatting.\n";
                return 1;
            }
            return 0;
        }
        std::cout << formatted;
        return 0;
    }

    int exitCode = 0;
    Formatter fmt(style);

    for (const auto& file : files) {
        std::ifstream in(file);
        if (!in.is_open()) {
            std::cerr << "Error: Cannot open file: " << file << "\n";
            exitCode = 1;
            continue;
        }
        std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();

        std::string formatted = fmt.format(content);

        if (verify) {
            if (formatted != content) {
                std::cerr << "File requires formatting: " << file << "\n";
                exitCode = 1;
            }
        } else if (inPlace) {
            if (formatted != content) {
                std::ofstream out(file, std::ios::trunc);
                if (!out.is_open()) {
                    std::cerr << "Error: Cannot write to file: " << file << "\n";
                    exitCode = 1;
                    continue;
                }
                out << formatted;
                out.close();
            }
        } else {
            std::cout << formatted;
        }
    }

    return exitCode;
}
