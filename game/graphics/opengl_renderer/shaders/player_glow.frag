#version 410 core

// Multiplayer player glow composite (see PlayerGlow.h). glow_tex holds the glowing players drawn
// flat: rgb = their glow color (already faded), a = 1 where they're a halo source, 0.25 where
// they're only masked out (a visible part). Around the sources this grows a halo, never over a
// player, which is added to the picture.

out vec4 color;

uniform sampler2D glow_tex;
uniform vec2 texel;       // 1 / size of glow_tex
uniform float width;      // halo width, in pixels
uniform float rounding;   // 0 = square corners, 1 = round
uniform float blur;       // 0 = hard edge, 1 = fades out across the whole width

const int RINGS = 8;
const int DIRECTIONS = 16;
const float TAU = 6.2831853;

void main() {
  vec2 uv = gl_FragCoord.xy * texel;
  if (textureLod(glow_tex, uv, 0.0).a > 0.1) {
    color = vec4(0.0);  // on a player
    return;
  }

  // sample directions on a superellipse of radius 1: exponent 2 is a circle, large is a square
  float p = mix(12.0, 2.0, rounding);
  vec2 dirs[DIRECTIONS];
  for (int k = 0; k < DIRECTIONS; k++) {
    vec2 d = vec2(cos(TAU * float(k) / float(DIRECTIONS)), sin(TAU * float(k) / float(DIRECTIONS)));
    dirs[k] = d / pow(pow(abs(d.x), p) + pow(abs(d.y), p), 1.0 / p);
  }

  // the nearest source decides: full strength out to the inner edge, faded to nothing at width
  float inner = width * (1.0 - blur);
  float halo = 0.0;
  vec3 halo_color = vec3(0.0);
  for (int ring = 1; ring <= RINGS; ring++) {
    float r = width * (float(ring) - 0.5) / float(RINGS);
    // coarser mips further out, so the gaps between directions are covered
    float lod = log2(max(1.0, TAU * r / float(DIRECTIONS)));
    vec3 ring_color = vec3(0.0);
    float ring_amount = 0.0;
    for (int k = 0; k < DIRECTIONS; k++) {
      vec4 s = textureLod(glow_tex, uv + dirs[k] * r * texel, lod);
      ring_color += s.rgb;
      ring_amount += clamp((s.a - 0.25) / 0.75, 0.0, 1.0);
    }
    float edge = clamp((width - r) / max(width - inner, 0.001), 0.0, 1.0);
    float strength = min(1.0, 3.0 * ring_amount / float(DIRECTIONS)) * edge * edge * (3.0 - 2.0 * edge);
    if (strength > halo) {
      halo = strength;
      halo_color = ring_color / max(ring_amount, 0.001);
    }
  }
  color = vec4(halo_color * halo * 0.9, 1.0);
}
