/*
 *  Copyright (c) 2023 The WebRTC project authors. All Rights Reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "test/testsupport/y4m_frame_generator.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "absl/strings/string_view.h"
#include "test/gtest.h"
#include "test/testsupport/file_utils.h"

namespace webrtc {
namespace test {

class Y4mFrameGeneratorTest : public testing::Test {
 protected:
  Y4mFrameGeneratorTest() = default;
  ~Y4mFrameGeneratorTest() = default;

  void SetUp() {
    input_filepath_ = TempFilename(OutputPath(), "2x2.y4m");
    FILE* y4m_file = fopen(input_filepath_.c_str(), "wb");

    // Input Y4M file: 3 YUV frames of 2x2 resolution.
    std::string y4m_content =
        "YUV4MPEG2 W2 H2 F2:1 C420\n"
        "FRAME\n"
        "123456FRAME\n"
        "abcdefFRAME\n"
        "987654";
    std::fprintf(y4m_file, "%s", y4m_content.c_str());
    fclose(y4m_file);
  }

  void TearDown() { remove(input_filepath_.c_str()); }

  std::string input_filepath_;
};

TEST_F(Y4mFrameGeneratorTest, CanReadResolutionFromFile) {
  Y4mFrameGenerator generator(input_filepath_,
                              Y4mFrameGenerator::RepeatMode::kSingle);
  FrameGeneratorInterface::Resolution res = generator.GetResolution();
  EXPECT_EQ(res.width, 2u);
  EXPECT_EQ(res.height, 2u);
}

TEST_F(Y4mFrameGeneratorTest, SingleRepeatMode) {
  Y4mFrameGenerator generator(input_filepath_,
                              Y4mFrameGenerator::RepeatMode::kSingle);

  std::vector<std::string> expected_frame_ys = {"123456", "abcdef", "987654"};
  for (absl::string_view frame_y : expected_frame_ys) {
    EXPECT_EQ(frame_y.size(), 6u);
    FrameGeneratorInterface::VideoFrameData frame = generator.NextFrame();
    EXPECT_EQ(memcmp(frame_y.data(), frame.buffer->GetI420()->DataY(), 6), 0);
  }
  FrameGeneratorInterface::VideoFrameData frame = generator.NextFrame();
  EXPECT_EQ(frame.buffer, nullptr);
}

TEST_F(Y4mFrameGeneratorTest, LoopRepeatMode) {
  Y4mFrameGenerator generator(input_filepath_,
                              Y4mFrameGenerator::RepeatMode::kLoop);

  std::vector<std::string> expected_frame_ys = {"123456", "abcdef", "987654",
                                                "123456", "abcdef", "987654"};
  for (absl::string_view frame_y : expected_frame_ys) {
    EXPECT_EQ(frame_y.size(), 6u);
    FrameGeneratorInterface::VideoFrameData frame = generator.NextFrame();
    EXPECT_EQ(memcmp(frame_y.data(), frame.buffer->GetI420()->DataY(), 6), 0);
  }
}

TEST_F(Y4mFrameGeneratorTest, PingPongRepeatMode) {
  Y4mFrameGenerator generator(input_filepath_,
                              Y4mFrameGenerator::RepeatMode::kPingPong);

  std::vector<std::string> expected_frame_ys = {
      "123456", "abcdef", "987654", "abcdef", "123456", "abcdef", "987654"};
  for (absl::string_view frame_y : expected_frame_ys) {
    EXPECT_EQ(frame_y.size(), 6u);
    FrameGeneratorInterface::VideoFrameData frame = generator.NextFrame();
    EXPECT_EQ(memcmp(frame_y.data(), frame.buffer->GetI420()->DataY(), 6), 0);
  }
}

}  // namespace test
}  // namespace webrtc
