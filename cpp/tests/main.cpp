// nanobook — test runner.
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "framework.hpp"

int main(int argc, char** argv) {
    const char* filter = (argc > 1) ? argv[1] : nullptr;

    // Group by suite so the output reads as a checklist of what is covered.
    std::map<std::string, std::vector<nbtest::TestCase>> by_suite;
    for (const auto& tc : nbtest::registry()) by_suite[tc.suite].push_back(tc);

    std::uint64_t run = 0, suites = 0;
    for (const auto& [suite, cases] : by_suite) {
        if (filter != nullptr && suite.find(filter) == std::string::npos) continue;
        std::printf("%s\n", suite.c_str());
        ++suites;
        for (const auto& tc : cases) {
            const std::uint64_t before = nbtest::counters().failures;
            nbtest::counters().current = std::string(suite) + "." + tc.name;
            tc.fn();
            ++run;
            const bool ok = nbtest::counters().failures == before;
            std::printf("  %s %s\n", ok ? "ok  " : "FAIL", tc.name);
        }
    }

    const auto& c = nbtest::counters();
    std::printf("\n%llu tests in %llu suites, %llu assertions, %llu failures\n",
                static_cast<unsigned long long>(run),
                static_cast<unsigned long long>(suites),
                static_cast<unsigned long long>(c.checks),
                static_cast<unsigned long long>(c.failures));
    if (c.failures == 0) std::printf("PASS\n");
    else std::printf("FAILED\n");
    return c.failures == 0 ? 0 : 1;
}
