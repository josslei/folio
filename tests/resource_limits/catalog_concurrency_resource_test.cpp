#include "storage/metadata_catalog.hpp"
#include <gtest/gtest.h>
#include <atomic>
#include <future>
#include <iostream>
#ifdef __APPLE__
#include <mach/mach.h>
#elif defined(__linux__)
#include <fstream>
#include <unistd.h>
#endif

namespace {
namespace core = folio::core;
namespace storage = core::storage;
using Clock = std::chrono::steady_clock;
std::size_t resident_bytes() {
#ifdef __APPLE__
  mach_task_basic_info_data_t info{};
  mach_msg_type_number_t count = MACH_TASK_BASIC_INFO_COUNT;
  if (task_info(mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info),
                &count) == KERN_SUCCESS)
    return info.resident_size;
#elif defined(__linux__)
  std::size_t virtual_pages, resident_pages;
  if (std::ifstream("/proc/self/statm") >> virtual_pages >> resident_pages)
    return resident_pages * static_cast<std::size_t>(sysconf(_SC_PAGESIZE));
#endif
  return 0; // RSS measurement is optional; Windows runtime verification is separate.
}

TEST(CatalogConcurrencyResources, RetainedVersionsRemainStableDuringFrequentUpdates) {
  storage::MetadataCatalog catalog;
  core::TimestampContext time{core::Timestamp{std::chrono::seconds{1800000000}}, "UTC", {}};
  std::vector<storage::RecordMetadata> records;
  records.reserve(10000);
  for (unsigned i = 0; i < 10000; ++i) {
    auto snapshot = core::Memo{std::string(256, 'a'), "unused body", time}.snapshot();
    auto metadata = storage::metadata_from(snapshot, std::filesystem::path("memos") /
                                                         (snapshot.id.to_string() + ".md"));
    ASSERT_TRUE(std::holds_alternative<std::monostate>(catalog.insert_record(metadata)));
    records.push_back(std::move(metadata));
  }
  const auto baseline = resident_bytes();
  std::vector<storage::MetadataCatalogView> views;
  const auto copies_started = Clock::now();
  for (unsigned i = 0; i < 10000; ++i)
    views.push_back(catalog.read_view());
  const auto copies_us =
      std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - copies_started).count();
  const auto copies_rss = resident_bytes();
  std::atomic<bool> updated = false;
  std::promise<void> reading;
  auto reader = std::async(std::launch::async, [view = catalog.read_view(), &updated, &reading] {
    reading.set_value();
    std::size_t queries = 0;
    do {
      const auto results = view.query();
      EXPECT_EQ(results.size(), 10000u);
      for (const auto& metadata : results)
        EXPECT_EQ(std::get<storage::MemoMetadata>(metadata).metadata.title, std::string(256, 'a'));
      ++queries;
    } while (!updated.load());
    return queries;
  });
  reading.get_future().wait();
  const auto started = Clock::now();
  for (unsigned i = 0; i < 512; ++i) {
    auto value = records[i];
    std::get<storage::MemoMetadata>(value).metadata.title = std::string(512, 'b');
    EXPECT_TRUE(std::holds_alternative<std::monostate>(catalog.update_record(std::move(value))));
    if (i % 16 == 0) views.push_back(catalog.read_view());
  }
  const auto retained = resident_bytes();
  updated = true;
  const auto queries = reader.get();
  const auto elapsed =
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - started).count();
  EXPECT_EQ(views.front().query().size(), 10000u);
  EXPECT_EQ(std::get<storage::MemoMetadata>(
                *views.front().find_record(std::get<storage::MemoMetadata>(records[0]).metadata.id))
                .metadata.title,
            std::string(256, 'a'));
  views.clear();
  std::cout << "catalog_entries=10000 updates=512 retained_versions=32 view_handles=10000"
            << " capture_us=" << copies_us << " update_and_reader_ms=" << elapsed
            << " concurrent_queries=" << queries << " rss_baseline=" << baseline
            << " rss_after_handles=" << copies_rss << " rss_retained=" << retained
            << " rss_released=" << resident_bytes() << '\n';
  // Allocators may retain freed pages; RSS is observational, not a brittle limit.
}
} // namespace
