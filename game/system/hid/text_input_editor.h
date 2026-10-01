#pragma once

#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include "third-party/SDL/include/SDL3/SDL_events.h"

class TextInputEditor {
 public:
  enum class Status : uint8_t { INACTIVE, EDITING, SUBMITTED, CANCELLED };

  struct Policy {
    size_t max_bytes = 0;
    std::string allowed_characters;
    bool uppercase_ascii = false;
  };

  struct Snapshot {
    uint32_t token = 0;
    uint32_t revision = 0;
    Status status = Status::INACTIVE;
    std::string text;
    size_t cursor = 0;
    size_t anchor = 0;
    std::string composition;
    size_t composition_cursor = 0;
    size_t composition_anchor = 0;
  };

  struct Clipboard {
    std::function<std::string()> read;
    std::function<void(std::string_view)> write;
  };

  explicit TextInputEditor(Clipboard clipboard = {});

  uint32_t begin(std::string_view initial_text, Policy policy);
  bool handle_event(const SDL_Event& event);
  bool request_submit(uint32_t token);
  bool request_cancel(uint32_t token);
  bool resolve_submission(uint32_t token, bool accepted);
  bool close(uint32_t token);
  void fail_active_session();
  Snapshot snapshot(uint32_t token = 0) const;
  bool active() const;

 private:
  bool valid_token(uint32_t token) const;
  bool accepts(char32_t codepoint) const;
  bool insert(std::string_view text);
  bool erase_selection();
  bool handle_key(const SDL_KeyboardEvent& event);
  void move_cursor(size_t target, bool select);
  void select_all();
  void copy_selection() const;
  void clear_composition();
  void changed();

  mutable std::mutex mutex_;
  Clipboard clipboard_;
  Policy policy_;
  std::unordered_set<char32_t> allowed_;
  Snapshot state_;
  uint32_t next_token_ = 1;
};
