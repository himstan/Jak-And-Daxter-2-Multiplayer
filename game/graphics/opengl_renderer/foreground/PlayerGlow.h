#pragma once

#include <array>
#include <chrono>
#include <memory>
#include <vector>

#include "game/graphics/opengl_renderer/BucketRenderer.h"

/*!
 * Multiplayer player glow, like the Left 4 Dead glow effect.
 *
 * During the merc passes, Merc2 draws the glowing players flat into a glow buffer that shares the
 * game's depth buffer: the parts that glow as halo sources, and in WALL mode their visible parts as
 * a mask so a halo never covers a player. composite() then grows those silhouettes into a halo and
 * adds it to the picture.
 *
 * In WALL mode a player only glows while completely out of sight. Occlusion queries on their
 * visible parts decide that, one frame late, and the glow fades in and out.
 */
class PlayerGlow {
 public:
  static constexpr int kMaxPlayers = 16;

  ~PlayerGlow();

  // Starts drawing into the glow buffer. False if there's nothing to draw into right now.
  bool begin(SharedRenderState* render_state);
  // Back to the game's framebuffer.
  void end(SharedRenderState* render_state);

  // How much this player glows this frame (0 to 1). Call before drawing them.
  float player_fade(u8 player_id, u64 frame);
  // Counts how much of this player is visible, for player_fade next frame.
  void begin_visibility_query(u8 player_id);
  void end_visibility_query(u8 player_id);

  // Turns everything drawn since the last composite into a halo over the picture.
  void composite(SharedRenderState* render_state);

 private:
  struct Player {
    float fade = 0.f;
    bool visible = true;
    u64 last_frame = UINT64_MAX;
    std::vector<GLuint> queries;  // this frame's
    std::vector<GLuint> pending;  // last frame's, waiting for their results
  };

  bool setup_buffers(SharedRenderState* render_state);
  void delete_buffers();
  void resolve_queries(Player& player);
  void recycle_queries(std::vector<GLuint>& queries);

  std::array<Player, kMaxPlayers> m_players;
  std::vector<GLuint> m_query_pool;
  GLuint m_active_query = 0;

  u64 m_frame = UINT64_MAX;
  float m_frame_seconds = 0.f;
  std::chrono::steady_clock::time_point m_frame_start;

  GLuint m_fbo = 0;          // glow color + the game's depth
  GLuint m_color = 0;        // multisampled like the game's picture
  GLuint m_resolve_fbo = 0;  // m_texture, readable by the composite
  GLuint m_texture = 0;
  GLuint m_depth_source = 0;
  GLuint m_vao = 0;
  int m_width = 0;
  int m_height = 0;
  int m_samples = 0;
  int m_samples_per_pixel = 1;
  bool m_failed = false;
  bool m_dirty = false;  // drawn into since the last composite
};
