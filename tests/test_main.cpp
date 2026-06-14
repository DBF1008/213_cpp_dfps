/*
 * Test runner — provides main() and the test registry.
 */

#include "test_framework.h"

std::vector<TestEntry> &GetTests(void) {
    static std::vector<TestEntry> tests;
    return tests;
}

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    auto &tests = GetTests();
    int passed = 0;
    int failed = 0;
    std::vector<const char *> failedNames;

    printf("Running %zu tests...\n\n", tests.size());

    for (auto &t : tests) {
        printf("  [RUN ] %s\n", t.name);
        try {
            t.fn();
            printf("  [PASS] %s\n", t.name);
            passed++;
        } catch (const TestFailure &f) {
            printf("  [FAIL] %s\n", t.name);
            printf("         %s:%d: %s\n", f.file, f.line, f.msg);
            failed++;
            failedNames.push_back(t.name);
        } catch (const std::exception &e) {
            printf("  [FAIL] %s (exception: %s)\n", t.name, e.what());
            failed++;
            failedNames.push_back(t.name);
        }
    }

    printf("\n========================================\n");
    printf("Results: %d passed, %d failed, %zu total\n", passed, failed, tests.size());

    if (failed > 0) {
        printf("\nFailed tests:\n");
        for (auto *name : failedNames) {
            printf("  - %s\n", name);
        }
        return 1;
    }

    printf("\nAll tests passed!\n");
    return 0;
}
