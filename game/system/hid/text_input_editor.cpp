#include "game/system/hid/text_input_editor.h"

#include <algorithm>

#include "common/util/unicode_util.h"

#include "third-party/SDL/include/SDL3/SDL_keycode.h"

TextInputEditor::TextInputEditor(Clipboard clipboard) : clipboard_(std::move(clipboard)) {}

uint32_t TextInputEditor::begin(const std::string_view initial_text, Policy policy) {
  std::lock_guard lock(mutex_);
  if (policy.max_bytes == 0 || initial_text.size() > policy.max_bytes ||
      !unicode::is_valid_utf8(initial_text)) {
    return 0;
  }
  std::vector<char32_t> allowed;
  if (!policy.allowed_characters.empty() &&
      !unicode::decode_utf8(policy.allowed_characters, allowed)) {
    return 0;
  }
  std::unordered_set candidate_allowed(allowed.begin(), allowed.end());
  std::string normalized_initial;
  size_t offset = 0;
  while (offset < initial_text.size()) {
    char32_t codepoint = 0;
    if (!unicode::decode_utf8_codepoint(initial_text, offset, codepoint)) {
      return 0;
    }
    if (policy.uppercase_ascii && codepoint >= 'a' && codepoint <= 'z') {
      codepoint -= 'a' - 'A';
    }
    if (codepoint < 0x20 || codepoint == 0x7f ||
        (!policy.allowed_characters.empty() && !candidate_allowed.contains(codepoint))) {
      return 0;
    }
    normalized_initial += unicode::encode_utf8_codepoint(codepoint);
  }
  if (normalized_initial.size() > policy.max_bytes) {
    return 0;
  }
  allowed_ = std::move(candidate_allowed);
  policy_ = std::move(policy);
  state_ = {};
  state_.token = next_token_++;
  if (next_token_ == 0) {
    next_token_ = 1;
  }
  state_.revision = 1;
  state_.status = Status::EDITING;
  state_.text = std::move(normalized_initial);
  state_.cursor = state_.anchor = state_.text.size();
  return state_.token;
}

bool TextInputEditor::handle_event(const SDL_Event& event) {
  std::lock_guard lock(mutex_);
  if (state_.status == Status::INACTIVE) {
    return false;
  }
  if (event.type == SDL_EVENT_KEY_DOWN) {
    if (state_.status == Status::EDITING) {
      handle_key(event.key);
    }
    return true;
  }
  if (event.type == SDL_EVENT_KEY_UP) {
    return true;
  }
  if (event.type == SDL_EVENT_TEXT_INPUT) {
    if (state_.status == Status::EDITING && event.text.text) {
      clear_composition();
      insert(event.text.text);
    }
    return true;
  }
  if (event.type == SDL_EVENT_TEXT_EDITING) {
    if (state_.status == Status::EDITING) {
      const std::string composition = event.edit.text ? event.edit.text : "";
      if (unicode::is_valid_utf8(composition)) {
        const size_t start = event.edit.start < 0 ? 0 : static_cast<size_t>(event.edit.start);
        const size_t length = event.edit.length < 0 ? 0 : static_cast<size_t>(event.edit.length);
        state_.composition = composition;
        state_.composition_cursor =
            unicode::utf8_codepoint_to_byte_offset(state_.composition, start);
        state_.composition_anchor =
            unicode::utf8_codepoint_to_byte_offset(state_.composition, start + length);
        changed();
      }
    }
    return true;
  }
  return false;
}

bool TextInputEditor::request_submit(const uint32_t token) {
  std::lock_guard lock(mutex_);
  if (!valid_token(token) || state_.status != Status::EDITING || !state_.composition.empty()) {
    return false;
  }
  clear_composition();
  state_.status = Status::SUBMITTED;
  changed();
  return true;
}

bool TextInputEditor::request_cancel(const uint32_t token) {
  std::lock_guard lock(mutex_);
  if (!valid_token(token) || state_.status == Status::INACTIVE) {
    return false;
  }
  clear_composition();
  state_.status = Status::CANCELLED;
  changed();
  return true;
}

bool TextInputEditor::resolve_submission(const uint32_t token, const bool accepted) {
  std::lock_guard lock(mutex_);
  if (!valid_token(token) || state_.status != Status::SUBMITTED) {
    return false;
  }
  state_.status = accepted ? Status::INACTIVE : Status::EDITING;
  changed();
  return true;
}

bool TextInputEditor::close(const uint32_t token) {
  std::lock_guard lock(mutex_);
  if (!valid_token(token)) {
    return false;
  }
  state_.status = Status::INACTIVE;
  clear_composition();
  changed();
  return true;
}

void TextInputEditor::fail_active_session() {
  std::lock_guard lock(mutex_);
  if (state_.status != Status::INACTIVE) {
    state_.status = Status::CANCELLED;
    clear_composition();
    changed();
  }
}

TextInputEditor::Snapshot TextInputEditor::snapshot(const uint32_t token) const {
  std::lock_guard lock(mutex_);
  if (token != 0 && token != state_.token) {
    return {};
  }
  return state_;
}

bool TextInputEditor::active() const {
  std::lock_guard lock(mutex_);
  return state_.status != Status::INACTIVE;
}

bool TextInputEditor::valid_token(const uint32_t token) const {
  return token != 0 && token == state_.token;
}

bool TextInputEditor::accepts(const char32_t codepoint) const {
  return codepoint >= 0x20 && codepoint != 0x7f &&
         (policy_.allowed_characters.empty() || allowed_.contains(codepoint));
}

bool TextInputEditor::insert(const std::string_view text) {
  std::string accepted;
  size_t offset = 0;
  const size_t selection_size =
      state_.cursor > state_.anchor ? state_.cursor - state_.anchor : state_.anchor - state_.cursor;
  const size_t available = policy_.max_bytes - (state_.text.size() - selection_size);
  while (offset < text.size()) {
    char32_t codepoint = 0;
    if (!unicode::decode_utf8_codepoint(text, offset, codepoint)) {
      return false;
    }
    if (policy_.uppercase_ascii && codepoint >= 'a' && codepoint <= 'z') {
      codepoint -= 'a' - 'A';
    }
    if (!accepts(codepoint)) {
      continue;
    }
    std::string encoded = unicode::encode_utf8_codepoint(codepoint);
    if (accepted.size() + encoded.size() > available) {
      break;
    }
    accepted += encoded;
  }
  if (accepted.empty()) {
    return false;
  }
  erase_selection();
  state_.text.insert(state_.cursor, accepted);
  state_.cursor += accepted.size();
  state_.anchor = state_.cursor;
  changed();
  return true;
}

bool TextInputEditor::erase_selection() {
  if (state_.cursor == state_.anchor) {
    return false;
  }
  const size_t first = std::min(state_.cursor, state_.anchor);
  const size_t last = std::max(state_.cursor, state_.anchor);
  state_.text.erase(first, last - first);
  state_.cursor = state_.anchor = first;
  return true;
}

bool TextInputEditor::handle_key(const SDL_KeyboardEvent& event) {
  const bool select = (event.mod & SDL_KMOD_SHIFT) != 0;
  if ((event.mod & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) != 0) {
    if (event.key == SDLK_A) {
      select_all();
      return true;
    }
    if (event.key == SDLK_C) {
      copy_selection();
      return true;
    }
    if (event.key == SDLK_X) {
      copy_selection();
      if (erase_selection()) {
        changed();
      }
      return true;
    }
    if (event.key == SDLK_V) {
      if (clipboard_.read) {
        insert(clipboard_.read());
      }
      return true;
    }
  }
  switch (event.key) {
    case SDLK_LEFT:
      move_cursor(state_.cursor != state_.anchor && !select
                      ? std::min(state_.cursor, state_.anchor)
                      : unicode::previous_utf8_boundary(state_.text, state_.cursor),
                  select);
      return true;
    case SDLK_RIGHT:
      move_cursor(state_.cursor != state_.anchor && !select
                      ? std::max(state_.cursor, state_.anchor)
                      : unicode::next_utf8_boundary(state_.text, state_.cursor),
                  select);
      return true;
    case SDLK_HOME:
      move_cursor(0, select);
      return true;
    case SDLK_END:
      move_cursor(state_.text.size(), select);
      return true;
    case SDLK_BACKSPACE:
      if (erase_selection()) {
        changed();
      } else if (state_.cursor > 0) {
        const size_t previous = unicode::previous_utf8_boundary(state_.text, state_.cursor);
        state_.text.erase(previous, state_.cursor - previous);
        state_.cursor = state_.anchor = previous;
        changed();
      }
      return true;
    case SDLK_DELETE:
      if (erase_selection()) {
        changed();
      } else if (state_.cursor < state_.text.size()) {
        const size_t next = unicode::next_utf8_boundary(state_.text, state_.cursor);
        state_.text.erase(state_.cursor, next - state_.cursor);
        state_.anchor = state_.cursor;
        changed();
      }
      return true;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
      if (!state_.composition.empty()) {
        return true;
      }
      clear_composition();
      state_.status = Status::SUBMITTED;
      changed();
      return true;
    case SDLK_ESCAPE:
      if (!state_.composition.empty()) {
        clear_composition();
        changed();
        return true;
      }
      clear_composition();
      state_.status = Status::CANCELLED;
      changed();
      return true;
    default:
      return false;
  }
}

void TextInputEditor::move_cursor(const size_t target, const bool select) {
  const size_t clamped = std::min(target, state_.text.size());
  if (state_.cursor == clamped && (select || state_.anchor == clamped)) {
    return;
  }
  state_.cursor = clamped;
  if (!select) {
    state_.anchor = clamped;
  }
  clear_composition();
  changed();
}

void TextInputEditor::select_all() {
  if (state_.cursor == state_.text.size() && state_.anchor == 0) {
    return;
  }
  state_.anchor = 0;
  state_.cursor = state_.text.size();
  clear_composition();
  changed();
}

void TextInputEditor::copy_selection() const {
  if (state_.cursor == state_.anchor || !clipboard_.write) {
    return;
  }
  const size_t first = std::min(state_.cursor, state_.anchor);
  const size_t last = std::max(state_.cursor, state_.anchor);
  clipboard_.write(std::string_view(state_.text).substr(first, last - first));
}

void TextInputEditor::clear_composition() {
  state_.composition.clear();
  state_.composition_cursor = 0;
  state_.composition_anchor = 0;
}

void TextInputEditor::changed() {
  if (++state_.revision == 0) {
    state_.revision = 1;
  }
}
