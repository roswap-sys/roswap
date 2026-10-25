#include <kunit/test.h>
#include <linux/module.h>

int rs_meta_selftest(void);

static void rs_kunit_meta(struct kunit *test)
{
	KUNIT_EXPECT_EQ(test, rs_meta_selftest(), 0);
}

static struct kunit_case rs_cases[] = {
	KUNIT_CASE(rs_kunit_meta),
	{}
};

static struct kunit_suite rs_suite = {
	.name = "roswap_extent",
	.test_cases = rs_cases,
};

kunit_test_suite(rs_suite);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("RoSwap extent allocator KUnit tests");
