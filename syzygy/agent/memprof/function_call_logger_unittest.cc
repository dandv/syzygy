// Copyright 2014 Google Inc. All Rights Reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// Function call logger unittests.

#include "syzygy/agent/memprof/function_call_logger.h"

#include "base/bind.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace agent {
namespace memprof {

namespace {

using trace::client::TraceFileSegment;

// TODO(chrisha): Centralize this for use elsewhere.
class TestRpcSession : public trace::client::RpcSession {
 public:
  TestRpcSession() : closed_(false) {
  }

  virtual bool CreateSession(TraceFileSegment* segment) {
    DCHECK_NE(static_cast<TraceFileSegment*>(nullptr), segment);
    return AllocateBuffer(segment);
  }

  virtual bool AllocateBuffer(TraceFileSegment* segment) {
    DCHECK_NE(static_cast<TraceFileSegment*>(nullptr), segment);
    return AllocateBuffer(2 * 1024 * 1024, segment);
  }

  virtual bool AllocateBuffer(size_t min_size, TraceFileSegment* segment) {
    DCHECK_NE(static_cast<TraceFileSegment*>(nullptr), segment);
    if (closed_)
      return false;
    buffer_.resize(min_size);
    segment->base_ptr = buffer_.data();
    segment->buffer_info.buffer_offset = 0;
    segment->buffer_info.buffer_size = min_size;
    segment->buffer_info.shared_memory_handle = 0;
    segment->end_ptr = segment->base_ptr + min_size;
    segment->header = reinterpret_cast<TraceFileSegmentHeader*>(
        segment->base_ptr);
    segment->write_ptr = reinterpret_cast<uint8*>(segment->header + 1);
    segment->header->thread_id = ::GetCurrentThreadId();
    segment->header->segment_length = 0;
    return true;
  }

  virtual bool ExchangeBuffer(TraceFileSegment* segment) {
    DCHECK_NE(static_cast<TraceFileSegment*>(nullptr), segment);
    return AllocateBuffer(segment);
  }

  virtual bool ReturnBuffer(TraceFileSegment* segment) {
    DCHECK_NE(static_cast<TraceFileSegment*>(nullptr), segment);
    if (closed_)
      return false;
    segment->base_ptr = nullptr;
    segment->buffer_info.buffer_offset = 0;
    segment->buffer_info.buffer_size = 0;
    segment->buffer_info.shared_memory_handle = 0;
    segment->end_ptr = nullptr;
    segment->header = nullptr;
    segment->write_ptr = nullptr;
    return true;
  }

  virtual bool CloseSession() {
    if (closed_)
      return false;
    closed_ = true;
    return true;
  }

  virtual void FreeSharedMemory() {
    return;
  }

 private:
  std::vector<uint8> buffer_;
  bool closed_;
};

class TestFunctionCallLogger : public FunctionCallLogger {
 public:
  TestFunctionCallLogger()
      : FunctionCallLogger(&test_session_, &test_segment_) {
    test_segment_.allocate_callback =
        base::Bind(&TestFunctionCallLogger::AllocateCallback,
                   base::Unretained(this));
    test_session_.AllocateBuffer(&test_segment_);
  }

  using FunctionCallLogger::function_id_map_;

  // The session and segment that are passed to the function call logger.
  TestRpcSession test_session_;
  TraceFileSegment test_segment_;

  // Stores information about the allocations made to test_segment.
  struct AllocationInfo {
    int record_type;
    size_t record_size;
    void* record;
  };
  std::vector<AllocationInfo> allocation_infos;

  // This callback is invoked whenever test_segment is written to.
  void AllocateCallback(
      int record_type, size_t record_size, void* record) {
    AllocationInfo info = { record_type, record_size, record };
    allocation_infos.push_back(info);
  }
};

void TestEmitDetailedFunctionCall(TestFunctionCallLogger* fcl) {
  ASSERT_NE(static_cast<TestFunctionCallLogger*>(nullptr), fcl);
  EMIT_DETAILED_FUNCTION_CALL((*fcl), fcl);
}

}  // namespace

TEST(FunctionCallLogger, TraceFunctionNameTableEntry) {
  TestFunctionCallLogger fcl;
  EXPECT_EQ(0u, fcl.function_id_map_.size());

  std::string name("foo");
  EXPECT_EQ(0u, fcl.GetFunctionId(name));
  EXPECT_EQ(1u, fcl.function_id_map_.size());
  EXPECT_THAT(fcl.function_id_map_,
              testing::Contains(std::make_pair(name, 0)));
  EXPECT_EQ(1u, fcl.allocation_infos.size());

  const auto& info = fcl.allocation_infos.front();
  EXPECT_EQ(TraceFunctionNameTableEntry::kTypeId,
            info.record_type);
  EXPECT_TRUE(info.record != nullptr);
  TraceFunctionNameTableEntry* data =
      reinterpret_cast<TraceFunctionNameTableEntry*>(
          info.record);
  EXPECT_LE(
      FIELD_OFFSET(TraceFunctionNameTableEntry, name) + data->name_length,
      info.record_size);
  EXPECT_EQ(name, data->name);
  fcl.allocation_infos.empty();

  // Adding the same name again should do nothing.
  EXPECT_EQ(0u, fcl.GetFunctionId("foo"));
  EXPECT_EQ(1u, fcl.function_id_map_.size());
  EXPECT_THAT(fcl.function_id_map_,
              testing::Contains(std::make_pair(std::string("foo"), 0)));
  EXPECT_EQ(1u, fcl.allocation_infos.size());
}

TEST(FunctionCallLogger, TraceDetailedFunctionCall) {
  TestFunctionCallLogger fcl;
  EXPECT_EQ(0u, fcl.function_id_map_.size());

  std::string name("agent::memprof::`anonymous-namespace'::"
                   "TestEmitDetailedFunctionCall");
  TestEmitDetailedFunctionCall(&fcl);
  EXPECT_EQ(1u, fcl.function_id_map_.size());
  EXPECT_THAT(fcl.function_id_map_,
              testing::Contains(std::make_pair(name, 0)));
  EXPECT_EQ(2u, fcl.allocation_infos.size());

  // Validate that the name record was appropriately written.
  const auto& info0 = fcl.allocation_infos[0];
  EXPECT_EQ(TraceFunctionNameTableEntry::kTypeId,
            info0.record_type);
  EXPECT_TRUE(info0.record != nullptr);
  TraceFunctionNameTableEntry* data0 =
      reinterpret_cast<TraceFunctionNameTableEntry*>(
          info0.record);
  EXPECT_LE(
      FIELD_OFFSET(TraceFunctionNameTableEntry, name) + data0->name_length,
      info0.record_size);
  EXPECT_EQ(name, data0->name);

  // Validate that the detailed function call record was written correctly.
  const auto& info1 = fcl.allocation_infos[1];
  EXPECT_EQ(TraceDetailedFunctionCall::kTypeId,
            info1.record_type);
  EXPECT_TRUE(info1.record != nullptr);
  TraceDetailedFunctionCall* data1 =
      reinterpret_cast<TraceDetailedFunctionCall*>(info1.record);
  EXPECT_LE(
      FIELD_OFFSET(TraceDetailedFunctionCall, argument_data) +
          data1->argument_data_size,
      info1.record_size);
  EXPECT_EQ(0u, data1->function_id);
  EXPECT_EQ(0u, data1->stack_trace_id);
  EXPECT_NE(0ull, data1->timestamp);
  // Number of arguments, size of argument, content of argument.
  EXPECT_EQ(2 * sizeof(uint32) + sizeof(void*), data1->argument_data_size);

  void* fcl_ptr = &fcl;
  uint8 ptr[4] = {};
  ::memcpy(ptr, &fcl_ptr, sizeof(ptr));
  const uint8 kExpectedContents[] = {
      0x01, 0x00, 0x00, 0x00,  // 1 argument...
      0x04, 0x00, 0x00, 0x00,  // ...of size 4...
      ptr[0], ptr[1], ptr[2], ptr[3],  // ...pointing to |fcl|.
      };
  ASSERT_EQ(arraysize(kExpectedContents), data1->argument_data_size);
  EXPECT_EQ(0u, ::memcmp(kExpectedContents, data1->argument_data,
                         data1->argument_data_size));
}

}  // namespace memprof
}  // namespace agent
