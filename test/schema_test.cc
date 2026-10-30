#include <gtest/gtest.h>
#include <rime/schema.h>

TEST(RimeSchemaTest, RuntimePageSizeRejectsInvalidValues) {
  rime::Schema schema(".config_test");
  EXPECT_EQ(5, schema.page_size());
  ASSERT_TRUE(schema.set_page_size(100));
  EXPECT_EQ(100, schema.page_size());
  EXPECT_FALSE(schema.set_page_size(0));
  EXPECT_FALSE(schema.set_page_size(-1));
  EXPECT_EQ(100, schema.page_size());
}
