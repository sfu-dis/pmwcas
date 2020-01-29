// Copyright (c) Xiangpeng Hao (haoxiangpeng@hotmail.com). All rights reserved.
// Licensed under the MIT license.

#include <gtest/gtest.h>
#include <stdlib.h>

#include <random>
#include <thread>

#include "common/allocator_internal.h"
#include "include/allocator.h"
#include "include/environment.h"
#include "include/pmwcas.h"
#include "include/status.h"
#include "mwcas/mwcas.h"
#include "util/auto_ptr.h"
#include "util/random_number_generator.h"
#ifdef WIN32
#include "environment/environment_windows.h"
#else
#include "environment/environment_linux.h"
#endif

static const uint64_t ARRAY_SIZE = 1024;
static const uint8_t ARRAY_INIT_VALUE = 0;
static const uint32_t UPDATE_ROUND = 8192;
static const uint32_t WORKLOAD_THREAD_CNT = 4;

void ArrayScan(uint64_t* array) {
  uint64_t dirty_cnt{0}, concas_cnt{0}, mwcas_cnt{0};
  for (uint32_t i = 0; i < ARRAY_SIZE; i += 1) {
    uint64_t val = array[i];
    if ((val & pmwcas::Descriptor::kDirtyFlag) != 0) {
      dirty_cnt += 1;
      val = val & ~pmwcas::Descriptor::kDirtyFlag;
    }
    if ((val & pmwcas::Descriptor::kCondCASFlag) != 0) {
      concas_cnt += 1;
      val = val & ~pmwcas::Descriptor::kCondCASFlag;
    }
    if ((val & pmwcas::Descriptor::kMwCASFlag) != 0) {
      mwcas_cnt += 1;
      val = val & ~pmwcas::Descriptor::kMwCASFlag;
    }
  }
  LOG(INFO) << "=======================\nDirty count: " << dirty_cnt
            << "\tCondition CAS count: " << concas_cnt
            << "\tMwCAS count: " << mwcas_cnt << std::endl;
}

namespace pmwcas {
GTEST_TEST(PMwCASTest, SingleThreadedRecovery) {
  auto* descriptor_pool =
      new pmwcas::DescriptorPool(10000, WORKLOAD_THREAD_CNT + 1, false);
  auto* allocator_ = (PMDKAllocator*)Allocator::Get();

  uint64_t** root_obj = (uint64_t**)allocator_->GetRoot(sizeof(uint64_t));
  allocator_->Allocate((void**)root_obj, sizeof(uint64_t) * ARRAY_SIZE);
  uint64_t* array = *root_obj;
  memset(array, ARRAY_INIT_VALUE, sizeof(uint64_t) * ARRAY_SIZE);

  auto thread_workload = [&]() {
    std::random_device rd;
    std::mt19937 eng(rd());
    std::uniform_int_distribution<> distr(0, ARRAY_SIZE);

    for (uint32_t i = 0; i < UPDATE_ROUND; i += 1) {
      pmwcas::EpochGuard guard(descriptor_pool->GetEpoch());
      auto desc = descriptor_pool->AllocateDescriptor();

      /// generate unique positions
      std::set<uint32_t> positions;
      while (positions.size() < DESC_CAP) {
        positions.insert(distr(eng));
      }

      /// randomly select array items to perform MwCAS
      for (const auto& it : positions) {
        auto item = &array[it];
        auto old_val = *item;
        if (pmwcas::Descriptor::IsCleanPtr(old_val)) {
          desc->AddEntry(item, old_val, old_val + 1);
        }
      }
      desc->MwCAS();
    }
    LOG(INFO) << "finsihed all jobs" << std::endl;
  };

  /// Step 1: start the workload on multiple threads;
  std::thread workers[WORKLOAD_THREAD_CNT];
  for (uint32_t t = 0; t < WORKLOAD_THREAD_CNT; t += 1) {
    workers[t] = std::thread(thread_workload);
  }

  /// Step 2: wait for some time;
  // std::this_thread::sleep_for(std::chrono::milliseconds(1));

  /// Step 3: force kill all running threads without noticing them
  for (uint32_t t = 0; t < WORKLOAD_THREAD_CNT; t += 1) {
    /// TODO(hao): this only works on Linux
    pthread_t thread_handler = workers[t].native_handle();
    workers[t].detach();
    pthread_cancel(thread_handler);
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  ArrayScan(array);

  /// Step 4: perform the recovery
  descriptor_pool->Recovery(false);

  /// Step 5: check every item in the array
  std::map<uint64_t, uint32_t> histogram;
  for (uint32_t i = 0; i < ARRAY_SIZE; i += 1) {
    auto value = array[i];

    ASSERT_TRUE(pmwcas::Descriptor::IsCleanPtr(value));

    if (histogram.find(value) == histogram.end()) {
      histogram[value] = 1;
    } else {
      histogram[value] += 1;
    }
  }
  LOG(INFO) << "=============================\nArray histogram\nvalue\tcount"
            << std::endl;
  for (const auto& item : histogram) {
    LOG(INFO) << item.first << "\t" << item.second << std::endl;
  }

  /// Step 6: perform random work over the pool again, there should not be any
  /// error.
  for (uint32_t t = 0; t < WORKLOAD_THREAD_CNT; t += 1) {
    workers[t] = std::thread(thread_workload);
  }
  for (uint32_t t = 0; t < WORKLOAD_THREAD_CNT; t += 1) {
    workers[t].join();
  }
}
}  // namespace pmwcas

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

#ifndef PMDK
  static_assert(false, "PMDK is currently required for recovery");
#endif

  pmwcas::InitLibrary(pmwcas::PMDKAllocator::Create(
                          "mwcas_test_pool", "mwcas_linked_layout",
                          static_cast<uint64_t>(1024) * 1024 * 1204 * 1),
                      pmwcas::PMDKAllocator::Destroy,
                      pmwcas::LinuxEnvironment::Create,
                      pmwcas::LinuxEnvironment::Destroy);
  return RUN_ALL_TESTS();
}
