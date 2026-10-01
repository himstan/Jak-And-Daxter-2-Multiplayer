#include "game/system/hid/text_input_editor.h"
#include "game/system/hid/input_manager.h"

#include <atomic>
#include <chrono>
#include <thread>

#include "common/util/FileUtil.h"
#include "common/util/unicode_util.h"

#include "gtest/gtest.h"
#include "third-party/SDL/include/SDL3/SDL_hints.h"

namespace {

SDL_Event key_event(const SDL_Keycode key, const SDL_Keymod modifiers = SDL_KMOD_NONE) {
  SDL_Event event = {};
  event.type = SDL_EVENT_KEY_DOWN;
  event.key.key = key;
  event.key.mod = modifiers;
  return event;
}

SDL_Event text_event(const char* text) {
  SDL_Event event = {};
  event.type = SDL_EVENT_TEXT_INPUT;
  event.text.text = text;
  return event;
}

}  // namespace

TEST(UnicodeUtil, ValidatesAndTraversesCodepoints) {
  const std::string text = "A\xc3\xa9\xf0\x9f\x98\x80";
  EXPECT_TRUE(unicode::is_valid_utf8(text));
  EXPECT_FALSE(unicode::is_valid_utf8("\xc0\xaf"));
  EXPECT_FALSE(unicode::is_valid_utf8("\xed\xa0\x80"));
  EXPECT_EQ(unicode::next_utf8_boundary(text, 1), 3);
  EXPECT_EQ(unicode::next_utf8_boundary(text, 4), 7);
  EXPECT_EQ(unicode::previous_utf8_boundary(text, text.size()), 3);
  EXPECT_EQ(unicode::utf8_codepoint_to_byte_offset(text, 2), 3);
}

TEST(TextInputEditor, FiltersNormalizesAndHonorsByteLimit) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("", {.max_bytes = 5,
                                            .allowed_characters = "ABC123",
                                            .uppercase_ascii = true});
  ASSERT_NE(token, 0u);
  editor.handle_event(text_event("a!b12"));
  EXPECT_EQ(editor.snapshot(token).text, "AB12");
  editor.handle_event(text_event("3"));
  EXPECT_EQ(editor.snapshot(token).text, "AB123");
}

TEST(TextInputEditor, EditsUtf8SelectionAtCodepointBoundaries) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("A\xc3\xa9Z", {.max_bytes = 16});
  ASSERT_NE(token, 0u);
  editor.handle_event(key_event(SDLK_LEFT));
  editor.handle_event(key_event(SDLK_LEFT, SDL_KMOD_SHIFT));
  auto snapshot = editor.snapshot(token);
  EXPECT_EQ(snapshot.cursor, 1u);
  EXPECT_EQ(snapshot.anchor, 3u);
  editor.handle_event(text_event("x"));
  EXPECT_EQ(editor.snapshot(token).text, "AxZ");
  editor.handle_event(key_event(SDLK_BACKSPACE));
  EXPECT_EQ(editor.snapshot(token).text, "AZ");
}

TEST(TextInputEditor, SupportsClipboardShortcuts) {
  std::string clipboard;
  TextInputEditor editor({[&]() { return clipboard; },
                          [&](const std::string_view text) { clipboard = text; }});
  const uint32_t token = editor.begin("hello", {.max_bytes = 16});
  ASSERT_NE(token, 0u);
  editor.handle_event(key_event(SDLK_A, SDL_KMOD_CTRL));
  editor.handle_event(key_event(SDLK_C, SDL_KMOD_CTRL));
  EXPECT_EQ(clipboard, "hello");
  editor.handle_event(key_event(SDLK_X, SDL_KMOD_CTRL));
  EXPECT_TRUE(editor.snapshot(token).text.empty());
  editor.handle_event(key_event(SDLK_V, SDL_KMOD_CTRL));
  EXPECT_EQ(editor.snapshot(token).text, "hello");
}

TEST(TextInputEditor, PublishesCompositionAndSessionStates) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("x", {.max_bytes = 16});
  ASSERT_NE(token, 0u);
  SDL_Event composition = {};
  composition.type = SDL_EVENT_TEXT_EDITING;
  composition.edit.text = "\xed\x95\x9c\xea\xb8\x80";
  composition.edit.start = 1;
  composition.edit.length = 1;
  editor.handle_event(composition);
  auto snapshot = editor.snapshot(token);
  EXPECT_EQ(snapshot.composition, "\xed\x95\x9c\xea\xb8\x80");
  EXPECT_EQ(snapshot.composition_cursor, 3u);
  EXPECT_EQ(snapshot.composition_anchor, 6u);
  editor.handle_event(text_event("\xed\x95\x9c\xea\xb8\x80"));
  EXPECT_TRUE(editor.request_submit(token));
  EXPECT_EQ(editor.snapshot(token).status, TextInputEditor::Status::SUBMITTED);
  EXPECT_TRUE(editor.resolve_submission(token, false));
  EXPECT_EQ(editor.snapshot(token).status, TextInputEditor::Status::EDITING);
  EXPECT_TRUE(editor.request_cancel(token));
  EXPECT_EQ(editor.snapshot(token).status, TextInputEditor::Status::CANCELLED);
  EXPECT_TRUE(editor.close(token));
  EXPECT_FALSE(editor.active());
}

TEST(TextInputEditor, NewSessionInvalidatesOldToken) {
  TextInputEditor editor;
  const uint32_t first = editor.begin("one", {.max_bytes = 16});
  const uint32_t second = editor.begin("two", {.max_bytes = 16});
  ASSERT_NE(first, second);
  EXPECT_EQ(editor.snapshot(first).token, 0u);
  EXPECT_FALSE(editor.request_submit(first));
  EXPECT_EQ(editor.snapshot(second).text, "two");
}

TEST(TextInputEditor, InvalidBeginPreservesActivePolicyAndSession) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("abc", {.max_bytes = 8});
  ASSERT_NE(token, 0u);
  EXPECT_EQ(editor.begin("bad!", {.max_bytes = 8, .allowed_characters = "abc"}), 0u);
  editor.handle_event(text_event("z"));
  EXPECT_EQ(editor.snapshot(token).text, "abcz");
  EXPECT_EQ(editor.snapshot(token).status, TextInputEditor::Status::EDITING);
}

TEST(TextInputEditor, NoOpDeletesAndRejectedControlTextKeepRevision) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("", {.max_bytes = 8});
  ASSERT_NE(token, 0u);
  const uint32_t revision = editor.snapshot(token).revision;
  editor.handle_event(key_event(SDLK_BACKSPACE));
  editor.handle_event(key_event(SDLK_DELETE));
  editor.handle_event(text_event("\n"));
  EXPECT_EQ(editor.snapshot(token).revision, revision);
  EXPECT_TRUE(editor.snapshot(token).text.empty());
}

TEST(TextInputEditor, HomeEndSelectionAndDeleteRespectUtf8Boundaries) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("A\xf0\x9f\x98\x80Z", {.max_bytes = 16});
  ASSERT_NE(token, 0u);
  editor.handle_event(key_event(SDLK_HOME));
  EXPECT_EQ(editor.snapshot(token).cursor, 0u);
  editor.handle_event(key_event(SDLK_RIGHT));
  editor.handle_event(key_event(SDLK_DELETE));
  EXPECT_EQ(editor.snapshot(token).text, "AZ");
  editor.handle_event(key_event(SDLK_END, SDL_KMOD_SHIFT));
  EXPECT_EQ(editor.snapshot(token).anchor, 1u);
  EXPECT_EQ(editor.snapshot(token).cursor, 2u);
  editor.handle_event(text_event("x"));
  EXPECT_EQ(editor.snapshot(token).text, "Ax");
}

TEST(TextInputEditor, PasteRejectsMalformedUtf8AndDoesNotSplitCodepoints) {
  std::string clipboard = "\xf0\x9f\x98\x80";
  TextInputEditor editor({[&]() { return clipboard; }, {}});
  const uint32_t token = editor.begin("A", {.max_bytes = 4});
  ASSERT_NE(token, 0u);
  editor.handle_event(key_event(SDLK_V, SDL_KMOD_GUI));
  EXPECT_EQ(editor.snapshot(token).text, "A");
  clipboard = "\xc0\xaf";
  editor.handle_event(key_event(SDLK_V, SDL_KMOD_GUI));
  EXPECT_EQ(editor.snapshot(token).text, "A");
  EXPECT_TRUE(unicode::is_valid_utf8(editor.snapshot(token).text));
}

TEST(TextInputEditor, ConsumesOnlyKeyboardInputWhileSessionIsActive) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("", {.max_bytes = 8});
  ASSERT_NE(token, 0u);
  SDL_Event mouse = {};
  mouse.type = SDL_EVENT_MOUSE_MOTION;
  EXPECT_FALSE(editor.handle_event(mouse));
  EXPECT_TRUE(editor.handle_event(key_event(SDLK_A)));
  EXPECT_TRUE(editor.handle_event(text_event("a")));
  ASSERT_TRUE(editor.close(token));
  EXPECT_FALSE(editor.handle_event(key_event(SDLK_A)));
  EXPECT_FALSE(editor.handle_event(text_event("a")));
}

TEST(TextInputEditor, ImeConfirmationDoesNotSubmitBeforeCommittedTextArrives) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("", {.max_bytes = 16});
  ASSERT_NE(token, 0u);
  SDL_Event composition = {};
  composition.type = SDL_EVENT_TEXT_EDITING;
  composition.edit.text = "\xed\x95\x9c";
  editor.handle_event(composition);
  EXPECT_FALSE(editor.request_submit(token));
  editor.handle_event(key_event(SDLK_RETURN));
  EXPECT_EQ(editor.snapshot(token).status, TextInputEditor::Status::EDITING);
  editor.handle_event(text_event("\xed\x95\x9c"));
  EXPECT_EQ(editor.snapshot(token).text, "\xed\x95\x9c");
  EXPECT_TRUE(editor.request_submit(token));
}

TEST(TextInputEditor, ConcurrentSnapshotsRemainCoherent) {
  TextInputEditor editor;
  const uint32_t token = editor.begin("", {.max_bytes = 64});
  ASSERT_NE(token, 0u);
  std::atomic<bool> done = false;
  std::thread reader([&]() {
    while (!done.load()) {
      const auto snapshot = editor.snapshot(token);
      EXPECT_TRUE(unicode::is_valid_utf8(snapshot.text));
      EXPECT_LE(snapshot.cursor, snapshot.text.size());
      EXPECT_LE(snapshot.anchor, snapshot.text.size());
    }
  });
  for (int index = 0; index < 32; ++index) {
    editor.handle_event(text_event("x"));
  }
  done = true;
  reader.join();
}

TEST(InputManagerTextInput, KeyboardCommandsResumeAfterEditing) {
  const auto test_dir = fs::temp_directory_path() /
                        ("opengoal-text-input-" + std::to_string(
                             std::chrono::steady_clock::now().time_since_epoch().count()));
  file_util::override_user_config_dir(test_dir, true);
  const char* previous_video_driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
  const std::string saved_video_driver = previous_video_driver ? previous_video_driver : "";
  SDL_SetHint(SDL_HINT_VIDEO_DRIVER, "dummy");
  ASSERT_TRUE(SDL_Init(SDL_INIT_VIDEO));
  SDL_Window* window = SDL_CreateWindow("text-input-test", 1, 1, SDL_WINDOW_HIDDEN);
  ASSERT_NE(window, nullptr);
  {
    InputManager input(window);
    int command_count = 0;
    input.register_command(CommandBinding::Source::KEYBOARD,
                           CommandBinding(SDLK_A, [&]() { ++command_count; }));
    input.process_sdl_event(key_event(SDLK_A));
    EXPECT_EQ(command_count, 1);
    const uint32_t token = input.begin_text_input("", {.max_bytes = 8});
    ASSERT_NE(token, 0u);
    input.process_sdl_event(key_event(SDLK_A));
    input.process_sdl_event(text_event("a"));
    EXPECT_EQ(command_count, 1);
    EXPECT_EQ(input.text_input_snapshot(token).text, "a");
    ASSERT_TRUE(input.close_text_input(token));
    input.process_sdl_event(key_event(SDLK_A));
    EXPECT_EQ(command_count, 2);
  }
  SDL_DestroyWindow(window);
  SDL_QuitSubSystem(SDL_INIT_VIDEO);
  if (saved_video_driver.empty()) {
    SDL_ResetHint(SDL_HINT_VIDEO_DRIVER);
  } else {
    SDL_SetHint(SDL_HINT_VIDEO_DRIVER, saved_video_driver.c_str());
  }
  file_util::override_user_config_dir({}, true);
  fs::remove_all(test_dir);
}
