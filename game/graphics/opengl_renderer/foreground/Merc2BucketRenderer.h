#pragma once

#include "game/graphics/opengl_renderer/BucketRenderer.h"
#include "game/graphics/opengl_renderer/foreground/Merc2.h"

class Merc2BucketRenderer : public BucketRenderer {
 public:
  Merc2BucketRenderer(const std::string& name,
                      int my_id,
                      std::shared_ptr<Merc2> merc,
                      bool clear_depth_before_draw = false);
  void draw_debug_window() override;
  void render(DmaFollower& dma, SharedRenderState* render_state, ScopedProfilerNode& prof) override;
  bool empty() const override;

 private:
  bool m_empty = false;
  bool m_clear_depth_before_draw = false;
  std::shared_ptr<Merc2> m_renderer;
  MercDebugStats m_debug_stats;
};
