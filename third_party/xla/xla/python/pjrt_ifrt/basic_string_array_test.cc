/* Copyright 2024 The OpenXLA Authors.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#include "xla/python/pjrt_ifrt/basic_string_array.h"

#include <cstdint>
#include <memory>
#include <numeric>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "absl/strings/str_cat.h"
#include "absl/strings/string_view.h"
#include "absl/synchronization/notification.h"
#include "absl/types/span.h"
#include "llvm/Support/Casting.h"
#include "xla/pjrt/pjrt_future.h"
#include "xla/python/ifrt/array.h"
#include "xla/python/ifrt/device.h"
#include "xla/python/ifrt/dtype.h"
#include "xla/python/ifrt/future.h"
#include "xla/python/ifrt/memory.h"
#include "xla/python/ifrt/shape.h"
#include "xla/python/ifrt/sharding.h"
#include "xla/python/ifrt/test_util.h"
#include "xla/tsl/concurrency/ref_count.h"
#include "tsl/lib/core/status_test_util.h"
#include "tsl/platform/env.h"
#include "tsl/platform/statusor.h"
#include "tsl/platform/test.h"

namespace xla {
namespace ifrt {
namespace {

using ::testing::HasSubstr;
using ::tsl::testing::StatusIs;

// Makes a simple single device sharded `BasicStringArray` from the
// user-supplied buffers and on_done_with_buffer callback by means of the
// factory method: `BasicStringArray::Create`.
absl::StatusOr<tsl::RCReference<BasicStringArray>> CreateTestArray(
    Client* client, Future<BasicStringArray::Buffers> buffers,
    BasicStringArray::OnDoneWithBuffer on_done_with_buffer) {
  Shape shape({1});
  Device* device = client->addressable_devices().at(0);
  std::shared_ptr<const Sharding> sharding =
      SingleDeviceSharding::Create(device, MemoryKind());

  return BasicStringArray::Create(client, shape, sharding, std::move(buffers),
                                  std::move(on_done_with_buffer));
}

TEST(BasicStringArrayTest, CreateSuccess) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  BasicStringArray::Buffers buffers;
  buffers.push_back({"abc", "def"});

  // This test implicitly tests that the on_done_with_buffer can be a nullptr,
  // and that the destruction of the BasicStringArray object completes
  // successfully (even when the callback is a nullptr).
  TF_EXPECT_OK(CreateTestArray(client.get(),
                               Future<BasicStringArray::Buffers>(buffers),
                               /*on_done_with_buffer=*/nullptr));
}

TEST(BasicStringArrayTest, CreateFailure) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  // Create fails if with invalid future.
  EXPECT_THAT(CreateTestArray(client.get(), Future<BasicStringArray::Buffers>(),
                              /*on_done_with_buffer=*/nullptr),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(BasicStringArrayTest, Destruction) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());

  BasicStringArray::Buffers buffers;
  buffers.push_back({"abc", "def"});

  absl::Notification on_done_with_buffer_called;
  BasicStringArray::OnDoneWithBuffer on_done_with_buffer =
      [&on_done_with_buffer_called]() { on_done_with_buffer_called.Notify(); };

  auto array_creation_status_promise = PjRtFuture<>::CreatePromise();

  tsl::Env::Default()->SchedClosure(([&]() {
    auto array = CreateTestArray(client.get(),
                                 Future<BasicStringArray::Buffers>(buffers),
                                 std::move(on_done_with_buffer));

    array_creation_status_promise.Set(array.status());
    // `array` goes out of scope and gets destroyed.
  }));

  // Make sure that the array has been created successfully.
  TF_ASSERT_OK(Future<>(array_creation_status_promise).Await());

  // Destruction must release the buffer. That is, the `on_done_with_buffer`
  // callback must be called.
  on_done_with_buffer_called.WaitForNotification();
}

TEST(BasicStringArrayTest, Delete) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  BasicStringArray::Buffers buffers;
  buffers.push_back({"abc", "def"});
  absl::Notification on_done_with_buffer_called;
  BasicStringArray::OnDoneWithBuffer on_done_with_buffer =
      [&on_done_with_buffer_called]() { on_done_with_buffer_called.Notify(); };

  TF_ASSERT_OK_AND_ASSIGN(
      auto array,
      CreateTestArray(client.get(), Future<BasicStringArray::Buffers>(buffers),
                      std::move(on_done_with_buffer)));

  tsl::Env::Default()->SchedClosure([&]() { array->Delete(); });

  // Delete must have released the buffer by calling `on_done_with_buffer`.
  on_done_with_buffer_called.WaitForNotification();

  // IsDeleted should return true.
  EXPECT_TRUE(array->IsDeleted());
}

TEST(GetReadyFutureTest, SuccessCase) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  // Make a BasicStringArray with a future that is not ready.
  auto promise = Future<BasicStringArray::Buffers>::CreatePromise();
  auto buffers_future = Future<BasicStringArray::Buffers>(promise);
  TF_ASSERT_OK_AND_ASSIGN(auto array,
                          CreateTestArray(client.get(), buffers_future,
                                          /*on_done_with_buffer=*/nullptr));

  // Array should not be ready since the buffers future is not ready.
  auto ready_future = array->GetReadyFuture();
  EXPECT_FALSE(ready_future.IsKnownReady());

  // Make the buffers future ready asynchronously.
  BasicStringArray::Buffers buffers;
  buffers.push_back({"abc", "def"});
  tsl::Env::Default()->SchedClosure([&]() { promise.Set(buffers); });
  TF_EXPECT_OK(ready_future.Await());
}

TEST(GetReadyFutureTest, FailureCases) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  // Make a BasicStringArray with a future that is not ready.
  auto promise = Future<BasicStringArray::Buffers>::CreatePromise();
  auto buffers_future = Future<BasicStringArray::Buffers>(promise);
  TF_ASSERT_OK_AND_ASSIGN(auto array,
                          CreateTestArray(client.get(), buffers_future,
                                          /*on_done_with_buffer=*/nullptr));

  // Array should not be ready since the buffers future is not ready.
  auto ready_future = array->GetReadyFuture();
  EXPECT_FALSE(ready_future.IsKnownReady());

  // Make the buffers future ready with an error asynchronously
  tsl::Env::Default()->SchedClosure(
      [&]() { promise.Set(absl::InternalError("injected error")); });

  EXPECT_THAT(ready_future.Await(), StatusIs(absl::StatusCode::kInternal));
}

TEST(MakeArrayFromHostBufferTest, SuccessCase) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  Shape shape({1});
  Device* device = client->addressable_devices().at(0);
  std::shared_ptr<const Sharding> sharding =
      SingleDeviceSharding::Create(device, MemoryKind());

  auto string_views = std::make_shared<std::vector<absl::string_view>>();
  string_views->push_back("abc");
  string_views->push_back("def");
  const void* data = string_views->data();
  auto on_done_with_host_buffer = [string_views = std::move(string_views)]() {};

  TF_ASSERT_OK(client->MakeArrayFromHostBuffer(
      data, DType(DType::kString), shape,
      /*byte_strides=*/std::nullopt, std::move(sharding),
      Client::HostBufferSemantics::kImmutableOnlyDuringCall,
      std::move(on_done_with_host_buffer)));
}

TEST(MakeArrayFromHostBufferTest, FailureCases) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  Shape shape({1});
  Device* device = client->addressable_devices().at(0);
  std::shared_ptr<const Sharding> single_device_sharding =
      SingleDeviceSharding::Create(device, MemoryKind());
  auto string_views = std::make_shared<std::vector<absl::string_view>>();
  string_views->push_back("abc");
  string_views->push_back("def");
  const void* data = string_views->data();
  auto on_done_with_host_buffer = [string_views = std::move(string_views)]() {};

  // MakeArrayFromHostBuffer should check and fail if `byte_strides` in not
  // nullopt.
  EXPECT_THAT(
      client->MakeArrayFromHostBuffer(
          data, DType(DType::kString), shape,
          /*byte_strides=*/std::optional<absl::Span<const int64_t>>({8}),
          single_device_sharding,
          Client::HostBufferSemantics::kImmutableOnlyDuringCall,
          on_done_with_host_buffer),
      StatusIs(absl::StatusCode::kInvalidArgument));

  // MakeArrayFromHostBuffer should check and fail if the sharding is not a
  // SingleDeviceSharding.
  std::shared_ptr<const Sharding> opaque_sharding =
      OpaqueSharding::Create(DeviceList({device}), MemoryKind());
  EXPECT_THAT(client->MakeArrayFromHostBuffer(
                  data, DType(DType::kString), shape,
                  /*byte_strides=*/std::nullopt, opaque_sharding,
                  Client::HostBufferSemantics::kImmutableOnlyDuringCall,
                  on_done_with_host_buffer),
              StatusIs(absl::StatusCode::kInvalidArgument));

  // MakeArrayFromHostBuffer should check and fail if the requested
  // HostBufferSemantics is not supported.
  for (Client::HostBufferSemantics host_buffer_semantics :
       {Client::HostBufferSemantics::kImmutableUntilTransferCompletes,
        Client::HostBufferSemantics::kImmutableZeroCopy,
        Client::HostBufferSemantics::kMutableZeroCopy}) {
    SCOPED_TRACE(
        absl::StrCat("host_buffer_semantics: ", host_buffer_semantics));
    EXPECT_THAT(client->MakeArrayFromHostBuffer(
                    data, DType(DType::kString), shape,
                    /*byte_strides=*/std::nullopt, single_device_sharding,
                    host_buffer_semantics, on_done_with_host_buffer),
                StatusIs(absl::StatusCode::kInvalidArgument));
  }
}

// Makes a single device sharded string ifrt::Array. Makes the necessary host
// string buffers.
absl::StatusOr<tsl::RCReference<Array>> MakeSingleDeviceStringTestArray(
    absl::Span<const std::string> contents, Client* client,
    Device* const device) {
  Shape shape({1});
  std::shared_ptr<const Sharding> sharding =
      SingleDeviceSharding::Create(device, MemoryKind());

  auto string_views = std::make_shared<std::vector<absl::string_view>>();
  for (const auto& content : contents) {
    string_views->push_back(content);
  }
  const void* data = string_views->data();
  auto on_done_with_host_buffer = [string_views = std::move(string_views)]() {};

  return client->MakeArrayFromHostBuffer(
      data, DType(DType::kString), shape,
      /*byte_strides=*/std::nullopt, std::move(sharding),
      Client::HostBufferSemantics::kImmutableOnlyDuringCall,
      std::move(on_done_with_host_buffer));
}

// Makes a single device sharded test array containing floats on the given
// Device.
absl::StatusOr<tsl::RCReference<Array>> MakeSingleDeviceFloatTestArray(
    Client* client, Device* const device) {
  DType dtype(DType::kF32);
  Shape shape({2, 3});
  auto data = std::make_unique<std::vector<float>>(6);
  std::iota(data->begin(), data->end(), 0);
  std::shared_ptr<const Sharding> sharding =
      SingleDeviceSharding::Create(device, MemoryKind());

  return client->MakeArrayFromHostBuffer(
      data->data(), dtype, shape,
      /*byte_strides=*/std::nullopt, sharding,
      Client::HostBufferSemantics::kImmutableOnlyDuringCall,
      /*on_done_with_host_buffer=*/nullptr);
}

// Makes a sharded string array with two shards.
absl::StatusOr<tsl::RCReference<Array>> MakeShardedStringTestArrray(
    Client* client, const std::string shard1_contents,
    const std::string shard2_contents) {
  auto devices = client->addressable_devices();
  if (devices.size() < 2) {
    return absl::InvalidArgumentError(absl::StrCat(
        "Test client has too few devices. Need 2, got:", devices.size()));
  }

  std::shared_ptr<const Sharding> opaque_sharding = OpaqueSharding::Create(
      DeviceList({devices[0], devices[1]}), MemoryKind());

  std::vector<tsl::RCReference<Array>> arrays;
  for (int i = 0; i < 2; ++i) {
    TF_ASSIGN_OR_RETURN(
        auto array, MakeSingleDeviceStringTestArray({absl::StrCat("shard ", i)},
                                                    client, devices[i]));
    arrays.push_back(std::move(array));
  }

  return client->AssembleArrayFromSingleDeviceArrays(
      Shape({2}), std::move(opaque_sharding), absl::MakeSpan(arrays),
      ArrayCopySemantics::kAlwaysCopy);
}

TEST(AssembleArrayFromSingleDeviceArraysTest,
     SuccessWithReadySingleDeviceArrays) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());

  // Make a BasicStringArray with two underlying basic string arrays.
  const std::vector<std::string> per_shard_contents({"shard 0", "shard 1"});
  TF_ASSERT_OK_AND_ASSIGN(auto array, MakeShardedStringTestArrray(
                                          client.get(), per_shard_contents[0],
                                          per_shard_contents[1]));
  auto basic_string_array = llvm::dyn_cast<BasicStringArray>(array.get());
  ASSERT_NE(basic_string_array, nullptr);
  TF_ASSERT_OK_AND_ASSIGN(auto buffers, basic_string_array->buffers().Await());
  EXPECT_EQ(buffers.size(), 2);

  for (int i = 0; i < buffers.size(); ++i) {
    SCOPED_TRACE(absl::StrCat("buffer #", i));
    auto buffer = buffers[i];
    EXPECT_THAT(buffer, testing::ElementsAre(per_shard_contents[i]));
  }
}

TEST(AssembleArrayFromSingleDeviceArraysTest, FailsWithNonStringArrays) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  auto devices = client->addressable_devices();
  ASSERT_GE(devices.size(), 2);
  std::shared_ptr<const Sharding> opaque_sharding = OpaqueSharding::Create(
      DeviceList({devices[0], devices[1]}), MemoryKind());

  std::vector<tsl::RCReference<Array>> arrays(2);
  TF_ASSERT_OK_AND_ASSIGN(
      arrays[0], MakeSingleDeviceFloatTestArray(client.get(), devices[0]));
  TF_ASSERT_OK_AND_ASSIGN(
      arrays[1], MakeSingleDeviceStringTestArray({"string_array_contents"},
                                                 client.get(), devices[1]));

  EXPECT_THAT(client->AssembleArrayFromSingleDeviceArrays(
                  Shape({2}), std::move(opaque_sharding),
                  absl::MakeSpan(arrays), ArrayCopySemantics::kAlwaysCopy),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

TEST(AssembleArrayFromSingleDeviceArraysTest,
     FailsWithNonSingleDeviceStringArrays) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  auto devices = client->addressable_devices();
  ASSERT_GE(devices.size(), 2);
  std::shared_ptr<const Sharding> opaque_sharding = OpaqueSharding::Create(
      DeviceList({devices[0], devices[1]}), MemoryKind());

  std::vector<tsl::RCReference<Array>> arrays(2);
  const std::vector<std::string> per_shard_contents({"abc", "def"});
  TF_ASSERT_OK_AND_ASSIGN(arrays[0], MakeShardedStringTestArrray(
                                         client.get(), per_shard_contents[0],
                                         per_shard_contents[1]));
  TF_ASSERT_OK_AND_ASSIGN(
      arrays[1], MakeSingleDeviceStringTestArray({"string_array_contents"},
                                                 client.get(), devices[1]));

  EXPECT_THAT(client->AssembleArrayFromSingleDeviceArrays(
                  Shape({2}), std::move(opaque_sharding),
                  absl::MakeSpan(arrays), ArrayCopySemantics::kAlwaysCopy),
              StatusIs(absl::StatusCode::kInvalidArgument));
}

// Makes a `BasicStringArray::Buffers` and its associated
// `BasicStringArray::OnDoneWithBuffer` from the given span of strings.
std::pair<BasicStringArray::Buffers, BasicStringArray::OnDoneWithBuffer>
MakeBuffersAndOnDoneWithBuffer(
    absl::Span<const absl::string_view> input_strings) {
  BasicStringArray::Buffers buffers;
  auto string_holder = std::make_shared<std::vector<std::string>>();
  string_holder->reserve(input_strings.size());
  auto string_view_holder = std::make_shared<std::vector<absl::string_view>>();
  string_view_holder->reserve(input_strings.size());
  for (const auto str : input_strings) {
    string_holder->push_back(std::string(str));
  }
  for (const auto& str : *string_holder) {
    string_view_holder->push_back(absl::string_view(str));
  }
  buffers.push_back(*string_view_holder);

  BasicStringArray::OnDoneWithBuffer on_done_with_buffer =
      [string_holder = std::move(string_holder),
       string_view_holder = std::move(string_view_holder)]() {};

  return std::make_pair(std::move(buffers), std::move(on_done_with_buffer));
}

// Makes a simple single device sharded `BasicStringArray` that is not ready at
// the time of creation. Returns a promise that can be set to make the array
// ready. If the callers set this promise with buffers (i.e., not an error),
// then they must ensure that the underlying strings live until the
// `on-host-buffer-done` callback they provided is run.
absl::StatusOr<std::pair<tsl::RCReference<BasicStringArray>,
                         Promise<BasicStringArray::Buffers>>>
CreateNonReadyTestArray(
    Client* client, Device* const device,
    BasicStringArray::OnDoneWithBuffer on_done_with_buffer) {
  auto buffers_promise = Future<BasicStringArray::Buffers>::CreatePromise();
  auto buffers_future = Future<BasicStringArray::Buffers>(buffers_promise);
  Shape shape({1});
  std::shared_ptr<const Sharding> sharding =
      SingleDeviceSharding::Create(device, MemoryKind());

  TF_ASSIGN_OR_RETURN(auto array,
                      BasicStringArray::Create(client, shape, sharding,
                                               std::move(buffers_future),
                                               std::move(on_done_with_buffer)));

  return std::make_pair(std::move(array), std::move(buffers_promise));
}

TEST(AssembleArrayFromSingleDeviceArraysTest,
     FromNonReadySingleDeviceArraysSuccess) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  auto devices = client->addressable_devices();
  ASSERT_GE(devices.size(), 2);
  std::shared_ptr<const Sharding> opaque_sharding = OpaqueSharding::Create(
      DeviceList({devices[0], devices[1]}), MemoryKind());

  // Make two non-ready single device sharded arrays.
  std::vector<tsl::RCReference<Array>> arrays;
  std::vector<Promise<BasicStringArray::Buffers>> promises;
  arrays.reserve(2);
  auto buf_and_on_done_with_buffer = MakeBuffersAndOnDoneWithBuffer({"abc"});
  auto buffers0 = buf_and_on_done_with_buffer.first;
  auto on_done_with_buffer0 = buf_and_on_done_with_buffer.second;
  TF_ASSERT_OK_AND_ASSIGN(
      auto ret, CreateNonReadyTestArray(client.get(), devices[0],
                                        std::move(on_done_with_buffer0)));
  arrays.push_back(std::move(ret.first));
  promises.push_back(std::move(ret.second));

  buf_and_on_done_with_buffer = MakeBuffersAndOnDoneWithBuffer({"def"});
  auto buffers1 = buf_and_on_done_with_buffer.first;
  auto on_done_with_buffer1 = buf_and_on_done_with_buffer.second;
  TF_ASSERT_OK_AND_ASSIGN(
      ret, CreateNonReadyTestArray(client.get(), devices[1],
                                   std::move(on_done_with_buffer1)));
  arrays.push_back(std::move(ret.first));
  promises.push_back(std::move(ret.second));

  TF_ASSERT_OK_AND_ASSIGN(
      auto array, client->AssembleArrayFromSingleDeviceArrays(
                      Shape({1}), std::move(opaque_sharding),
                      absl::MakeSpan(arrays), ArrayCopySemantics::kAlwaysCopy));

  tsl::Env::Default()->SchedClosure(([&]() mutable {
    promises[0].Set(buffers0);
    promises[1].Set(buffers1);
  }));

  auto basic_string_array = llvm::dyn_cast<BasicStringArray>(array.get());
  ASSERT_NE(basic_string_array, nullptr);

  auto buffers_future = basic_string_array->buffers();
  TF_ASSERT_OK_AND_ASSIGN(auto buffers, buffers_future.Await());
  EXPECT_EQ(buffers.size(), 2);
  EXPECT_THAT(buffers[0], testing::ElementsAre("abc"));
  EXPECT_THAT(buffers[1], testing::ElementsAre("def"));
}

TEST(AssembleArrayFromSingleDeviceArraysTest,
     FromNonReadySingleDeviceArraysFailure) {
  TF_ASSERT_OK_AND_ASSIGN(auto client, test_util::GetClient());
  auto devices = client->addressable_devices();
  ASSERT_GE(devices.size(), 2);
  std::shared_ptr<const Sharding> opaque_sharding = OpaqueSharding::Create(
      DeviceList({devices[0], devices[1]}), MemoryKind());

  // Make two non-ready single device sharded arrays.
  std::vector<tsl::RCReference<Array>> arrays;
  std::vector<Promise<BasicStringArray::Buffers>> promises;
  arrays.reserve(2);

  TF_ASSERT_OK_AND_ASSIGN(
      auto ret, CreateNonReadyTestArray(client.get(), devices[0],
                                        /*on_done_with_buffer=*/nullptr));
  arrays.push_back(std::move(ret.first));
  promises.push_back(std::move(ret.second));

  TF_ASSERT_OK_AND_ASSIGN(
      ret, CreateNonReadyTestArray(client.get(), devices[1],
                                   /*on_done_with_buffer=*/nullptr));
  arrays.push_back(std::move(ret.first));
  promises.push_back(std::move(ret.second));

  // Make a sharded BasicStringArray out of the single device arrays.
  TF_ASSERT_OK_AND_ASSIGN(
      auto array, client->AssembleArrayFromSingleDeviceArrays(
                      Shape({1}), std::move(opaque_sharding),
                      absl::MakeSpan(arrays), ArrayCopySemantics::kAlwaysCopy));

  // Make the single device arrays become ready with an error.
  absl::Notification done_readying_single_device_arrays;
  tsl::Env::Default()->SchedClosure(([&]() mutable {
    promises[0].Set(absl::InternalError("injected from the test"));
    promises[1].Set(absl::InternalError("injected from the test"));
    done_readying_single_device_arrays.Notify();
  }));

  auto basic_string_array = llvm::dyn_cast<BasicStringArray>(array.get());
  ASSERT_NE(basic_string_array, nullptr);

  auto buffers_future = basic_string_array->buffers();
  EXPECT_THAT(buffers_future.Await(),
              StatusIs(absl::StatusCode::kInternal,
                       HasSubstr("injected from the test")));

  // Make sure to wait for the Closure to complete its work and set both
  // promises before returning from the test. The consequent destruction of the
  // promises can race with the Closure.
  done_readying_single_device_arrays.WaitForNotification();
}

}  // namespace
}  // namespace ifrt
}  // namespace xla
