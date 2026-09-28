/**
 * @file tests/unit/platform/windows/test_streaming_priority.cpp
 * @brief Test Windows streaming process-priority recovery.
 */
#ifdef _WIN32

  #include <chrono>
  #include <gtest/gtest.h>
  #include <src/platform/common.h>
  #include <thread>
  #include <Windows.h>

namespace {
  class process_priority_restore_t {
  public:
    process_priority_restore_t():
        original_priority_ {GetPriorityClass(GetCurrentProcess())} {
    }

    ~process_priority_restore_t() {
      if (streaming_started_) {
        platf::streaming_will_stop();
      }
      if (original_priority_ != 0) {
        SetPriorityClass(GetCurrentProcess(), original_priority_);
      }
    }

    void mark_streaming_started() {
      streaming_started_ = true;
    }

    [[nodiscard]] DWORD original_priority() const {
      return original_priority_;
    }

  private:
    DWORD original_priority_;
    bool streaming_started_ = false;
  };
}  // namespace

TEST(WindowsStreamingPriorityTests, RestoresHighPriorityAfterExternalDemotion) {
  using namespace std::chrono_literals;

  process_priority_restore_t restore;
  ASSERT_NE(restore.original_priority(), 0U);
  ASSERT_TRUE(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS));

  platf::streaming_will_start();
  restore.mark_streaming_started();
  ASSERT_EQ(GetPriorityClass(GetCurrentProcess()), HIGH_PRIORITY_CLASS);

  ASSERT_TRUE(SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS));
  const auto deadline = std::chrono::steady_clock::now() + 2500ms;
  while (GetPriorityClass(GetCurrentProcess()) != HIGH_PRIORITY_CLASS &&
         std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(50ms);
  }

  EXPECT_EQ(GetPriorityClass(GetCurrentProcess()), HIGH_PRIORITY_CLASS);
}

#endif
