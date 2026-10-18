#include "PlayerGlow.h"

#include <algorithm>

#include "common/log/log.h"

#include "game/graphics/gfx.h"

namespace {
// a few stray pixels (feet in the floor...) don't make a player visible
constexpr GLuint kVisibleSamples = 24;
// a player not drawn for this many frames starts over (hidden, no glow)
constexpr u64 kForgetFrames = 60;
}  // namespace

PlayerGlow::~PlayerGlow() {
  delete_buffers();
  for (auto& player : m_players) {
    recycle_queries(player.queries);
    recycle_queries(player.pending);
  }
  if (!m_query_pool.empty()) {
    glDeleteQueries(m_query_pool.size(), m_query_pool.data());
  }
  if (m_vao) {
    glDeleteVertexArrays(1, &m_vao);
  }
}

void PlayerGlow::delete_buffers() {
  if (m_fbo) {
    glDeleteFramebuffers(1, &m_fbo);
    glDeleteFramebuffers(1, &m_resolve_fbo);
    glDeleteRenderbuffers(1, &m_color);
    glDeleteTextures(1, &m_texture);
  }
  m_fbo = m_resolve_fbo = m_color = m_texture = 0;
}

bool PlayerGlow::setup_buffers(SharedRenderState* render_state) {
  // The glow buffer draws against the game's own depth buffer (attached, never written), so it's
  // tested against exactly what has been drawn so far.
  GLint type = 0, depth = 0;
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
  if (type != GL_RENDERBUFFER) {
    return false;
  }
  glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT,
                                        GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &depth);
  GLint samples = 0, width = 0, height = 0;
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_SAMPLES, &samples);
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_WIDTH, &width);
  glGetRenderbufferParameteriv(GL_RENDERBUFFER, GL_RENDERBUFFER_HEIGHT, &height);
  if (m_fbo && (GLuint)depth == m_depth_source && samples == m_samples && width == m_width &&
      height == m_height) {
    return true;
  }

  delete_buffers();
  m_depth_source = depth;
  m_samples = samples;
  m_samples_per_pixel = std::max(1, samples);
  m_width = width;
  m_height = height;

  glGenFramebuffers(1, &m_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glGenRenderbuffers(1, &m_color);
  glBindRenderbuffer(GL_RENDERBUFFER, m_color);
  if (samples > 0) {
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
  } else {
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
  }
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, m_color);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, depth);
  bool ok = glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

  glGenFramebuffers(1, &m_resolve_fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, m_resolve_fbo);
  glGenTextures(1, &m_texture);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_texture, 0);
  ok = ok && glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;

  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  if (!ok) {
    lg::error("[PlayerGlow] Glow framebuffer incomplete, player glow disabled");
    delete_buffers();
    m_failed = true;
    return false;
  }
  lg::info("[PlayerGlow] Glow buffer {}x{}, {} samples", width, height, samples);
  return true;
}

bool PlayerGlow::begin(SharedRenderState* render_state) {
  if (m_failed || render_state->render_fb == 0 || !setup_buffers(render_state)) {
    return false;
  }
  glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
  glViewport(render_state->render_fb_x, render_state->render_fb_y, render_state->render_fb_w,
             render_state->render_fb_h);
  if (!m_dirty) {
    m_dirty = true;
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  return true;
}

void PlayerGlow::end(SharedRenderState* render_state) {
  glBindFramebuffer(GL_FRAMEBUFFER, render_state->render_fb);
  glViewport(render_state->render_fb_x, render_state->render_fb_y, render_state->render_fb_w,
             render_state->render_fb_h);
}

void PlayerGlow::recycle_queries(std::vector<GLuint>& queries) {
  m_query_pool.insert(m_query_pool.end(), queries.begin(), queries.end());
  queries.clear();
}

void PlayerGlow::resolve_queries(Player& player) {
  if (player.pending.empty()) {
    player.pending.swap(player.queries);
    return;
  }
  for (auto query : player.pending) {
    GLuint available = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT_AVAILABLE, &available);
    if (!available) {
      // still waiting on the GPU: keep the old answer and skip last frame's count
      recycle_queries(player.queries);
      return;
    }
  }
  GLuint samples = 0;
  for (auto query : player.pending) {
    GLuint count = 0;
    glGetQueryObjectuiv(query, GL_QUERY_RESULT, &count);
    samples += count;
  }
  player.visible = samples > kVisibleSamples * m_samples_per_pixel;
  recycle_queries(player.pending);
  player.pending.swap(player.queries);
}

float PlayerGlow::player_fade(u8 player_id, u64 frame) {
  if (frame != m_frame) {
    const auto now = std::chrono::steady_clock::now();
    m_frame_seconds =
        m_frame == UINT64_MAX
            ? 0.f
            : std::clamp(std::chrono::duration<float>(now - m_frame_start).count(), 0.f, 0.25f);
    m_frame_start = now;
    m_frame = frame;
  }
  if (player_id >= kMaxPlayers) {
    return 0.f;
  }
  auto& player = m_players[player_id];
  if (player.last_frame != frame) {
    if (player.last_frame == UINT64_MAX || player.last_frame + kForgetFrames < frame) {
      recycle_queries(player.queries);
      recycle_queries(player.pending);
      player.fade = 0.f;
      player.visible = true;
    } else {
      resolve_queries(player);
      const auto& settings = Gfx::g_global_settings.player_glow;
      const float seconds = player.visible ? settings.fade_out_seconds : settings.fade_in_seconds;
      const float step = seconds > 0.f ? m_frame_seconds / seconds : 1.f;
      player.fade = std::clamp(player.fade + (player.visible ? -step : step), 0.f, 1.f);
    }
    player.last_frame = frame;
  }
  return player.fade;
}

void PlayerGlow::begin_visibility_query(u8 player_id) {
  if (player_id >= kMaxPlayers) {
    return;
  }
  if (m_query_pool.empty()) {
    glGenQueries(1, &m_active_query);
  } else {
    m_active_query = m_query_pool.back();
    m_query_pool.pop_back();
  }
  glBeginQuery(GL_SAMPLES_PASSED, m_active_query);
}

void PlayerGlow::end_visibility_query(u8 player_id) {
  if (player_id >= kMaxPlayers) {
    return;
  }
  glEndQuery(GL_SAMPLES_PASSED);
  m_players[player_id].queries.push_back(m_active_query);
}

void PlayerGlow::composite(SharedRenderState* render_state) {
  if (!m_dirty) {
    return;  // nobody glowed since the last composite
  }
  m_dirty = false;

  // the multisampled glow -> a texture, mipmapped so the halo can sample it coarsely further out
  glBindFramebuffer(GL_READ_FRAMEBUFFER, m_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, m_resolve_fbo);
  glBlitFramebuffer(0, 0, m_width, m_height, 0, 0, m_width, m_height, GL_COLOR_BUFFER_BIT,
                    GL_NEAREST);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glGenerateMipmap(GL_TEXTURE_2D);
  end(render_state);

  const auto& settings = Gfx::g_global_settings.player_glow;
  auto& shader = render_state->shaders[ShaderId::PLAYER_GLOW];
  shader.activate();
  const auto id = shader.id();
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, m_texture);
  glUniform1i(glGetUniformLocation(id, "glow_tex"), 0);
  glUniform2f(glGetUniformLocation(id, "texel"), 1.f / m_width, 1.f / m_height);
  // the halo is the same share of the screen at any resolution
  glUniform1f(glGetUniformLocation(id, "width"),
              std::max(1.f, settings.width * render_state->render_fb_h / 1080.f));
  glUniform1f(glGetUniformLocation(id, "rounding"), std::clamp(settings.rounding, 0.f, 1.f));
  glUniform1f(glGetUniformLocation(id, "blur"), std::clamp(settings.blur, 0.f, 1.f));

  glDisable(GL_DEPTH_TEST);
  glDepthMask(GL_FALSE);
  glEnable(GL_BLEND);
  glBlendEquation(GL_FUNC_ADD);
  glBlendFunc(GL_ONE, GL_ONE);
  if (!m_vao) {
    glGenVertexArrays(1, &m_vao);
  }
  glBindVertexArray(m_vao);
  glDrawArrays(GL_TRIANGLES, 0, 3);
  glBindVertexArray(0);
  glDepthMask(GL_TRUE);
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
}
