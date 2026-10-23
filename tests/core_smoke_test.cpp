#include "folio/core/version.hpp"

#include <gtest/gtest.h>

TEST(CoreSmokeTest, ReturnsApplicationName) {
  EXPECT_EQ(folio::core::application_name(), "FOLIO");
}
