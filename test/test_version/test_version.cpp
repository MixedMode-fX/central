/*
 * Guards the version stamping the release process depends on.
 *
 * A release artifact is only traceable if the build actually carried the
 * version into the binary, so a silent regression in scripts/version.py — a
 * rename, a dropped define, a fallback quietly winning — would otherwise
 * surface as an unidentifiable .hex on a release page. This is the first
 * suite in the native environment; issue #2 adds the algorithm tests here
 * once the hardware seam exists.
 */

#include <unity.h>

#include <cstring>

#include "../../src/version.h"

void setUp(void) {}
void tearDown(void) {}

static void version_is_stamped_by_the_build(void)
{
    // The header's own fallback, which means version.py did not run.
    TEST_ASSERT_NOT_EQUAL_MESSAGE(
        0, strcmp(MMMC_VERSION, "0.0.0"),
        "MMMC_VERSION fell back to its default: scripts/version.py did not run");
    TEST_ASSERT_GREATER_THAN(0, (int)strlen(MMMC_VERSION));
}

static void version_looks_like_a_release_number(void)
{
    // Not a full semver parse: enough to catch a VERSION file holding a tag
    // name, a path, or leftover whitespace.
    int dots = 0;
    for (const char *c = MMMC_VERSION; *c; c++) {
        if (*c == '.') {
            dots++;
            continue;
        }
        TEST_ASSERT_TRUE_MESSAGE((*c >= '0' && *c <= '9') || *c == '-' ||
                                     (*c >= 'a' && *c <= 'z'),
                                 "VERSION holds an unexpected character");
    }
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, dots, "VERSION is not MAJOR.MINOR.PATCH");
}

static void build_string_carries_version_and_revision(void)
{
    TEST_ASSERT_NOT_NULL(strstr(MMMC_BUILD, MMMC_VERSION));
    TEST_ASSERT_NOT_NULL(strstr(MMMC_BUILD, MMMC_GIT_REV));
    TEST_ASSERT_NOT_NULL_MESSAGE(strchr(MMMC_BUILD, '+'),
                                 "MMMC_BUILD should read VERSION+REVISION");
}

int main(int, char **)
{
    UNITY_BEGIN();
    RUN_TEST(version_is_stamped_by_the_build);
    RUN_TEST(version_looks_like_a_release_number);
    RUN_TEST(build_string_carries_version_and_revision);
    return UNITY_END();
}
