#include "DiceApp.h"

#include <charconv>
#include <cstdint>
#include <exception>
#include <iostream>
#include <string>

namespace {

void printUsage(const char *program) {
    std::cout << "Usage: " << program << " [options]\n"
              << "  A tiny dice CLI demonstrating dependency injection.\n\n"
              << "Options:\n"
              << "  --seed <n>    Fixed seed (deterministic). Omit for real randomness.\n"
              << "  --rolls <n>   Number of dice rolls (default: 5).\n"
              << "  --parallel    Also roll <rolls> dice concurrently.\n"
              << "  --help, -h    Show this help.\n";
}

std::uint64_t parseUnsigned(const std::string &arg, const std::string &option) {
    std::uint64_t value = 0;
    const char *begin = arg.data();
    const char *end = arg.data() + arg.size();
    const auto [ptr, ec] = std::from_chars(begin, end, value);
    if (ec != std::errc{} || ptr != end) {
        throw std::invalid_argument("invalid value for " + option + ": '" + arg + "'");
    }
    return value;
}

struct Options {
    std::uint64_t seed = 0; // 0 == no seed -> live (nondeterministic) wiring
    int rolls = 5;
    bool parallel = false;
};

Options parseArguments(int argc, char **argv) {
    Options opts;
    for (int i = 1; i < argc; ++i) {
        const std::string arg{argv[i]};
        if (arg == "--seed") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--seed requires a value");
            }
            opts.seed = parseUnsigned(argv[++i], "--seed");
        } else if (arg == "--rolls") {
            if (i + 1 >= argc) {
                throw std::invalid_argument("--rolls requires a value");
            }
            opts.rolls = static_cast<int>(parseUnsigned(argv[++i], "--rolls"));
        } else if (arg == "--parallel") {
            opts.parallel = true;
        } else if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            std::exit(0);
        } else {
            throw std::invalid_argument("unknown argument '" + arg + "'");
        }
    }
    return opts;
}

} // namespace

int main(int argc, char **argv) {
    Options opts;
    try {
        opts = parseArguments(argc, argv);
    } catch (const std::exception &e) {
        std::cerr << "error: " << e.what() << "\n\n";
        printUsage(argv[0]);
        return 1;
    }

    const cppdi::AppContext context =
        opts.seed == 0 ? cppdi::AppContext::live() : cppdi::AppContext::test(opts.seed);

    cppdi::app::DiceRoller roller{context};
    std::cout << "Rolling a d6 " << opts.rolls << " time(s)...\n";
    for (int i = 0; i < opts.rolls; ++i) {
        std::cout << "  " << i + 1 << ". " << roller.roll() << '\n';
    }

    cppdi::app::RandomStringGenerator strings{context};
    std::cout << "Random string: " << strings.generate(12) << '\n';

    if (opts.parallel) {
        cppdi::app::AsyncDiceRoller asyncRoller{context};
        const auto values = asyncRoller.rollParallel(static_cast<std::size_t>(opts.rolls));
        std::cout << "Parallel rolls:";
        for (const int value : values) {
            std::cout << ' ' << value;
        }
        std::cout << '\n';
    }

    return 0;
}