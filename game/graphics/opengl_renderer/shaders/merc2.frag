#version 410 core

out vec4 color;
in vec4 vtx_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;
uniform sampler2D tex_T1;

uniform float darkjak_interp;

uniform vec3 player_tint_color;
uniform int player_tint_enabled;
uniform float player_tint_strength;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;


vec3 rgb_to_hsv(vec3 c) {
  vec4 K = vec4(
    0.0,
    -1.0 / 3.0,
    2.0 / 3.0,
    -1.0);

  vec4 p = mix(vec4(c.bg, K.wz), vec4(c.gb, K.xy), step(c.b, c.g));
  vec4 q = mix(vec4(p.xyw, c.r), vec4(c.r, p.yzx), step(p.x, c.r));
  float d = q.x - min(q.w, q.y);
  float e = 1.0e-10;

  return vec3(
    abs(q.z + (q.w - q.y) / (6.0 * d + e)),
    d / (q.x + e),
    q.x);
}


vec3 hsv_to_rgb(vec3 c) {
  vec3 p = abs(fract(c.xxx + vec3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0);

  return c.z * mix(vec3(1.0), clamp(p - 1.0, 0.0, 1.0), c.y);
}

vec3 apply_player_tint(
  vec3 source_color,
  vec3 target_color) {

  vec3 source_hsv = rgb_to_hsv(source_color);
  vec3 target_hsv = rgb_to_hsv(target_color);
  
  float tint_mask = smoothstep(0.04, 0.12, source_hsv.y);
  vec3 recolored_hsv = source_hsv;

  recolored_hsv.x = target_hsv.x;
  recolored_hsv.y = max(source_hsv.y, target_hsv.y * 0.85);
  recolored_hsv.z = source_hsv.z;

  vec3 recolored = hsv_to_rgb(recolored_hsv);

  return mix(
    source_color,
    recolored,
    tint_mask * clamp(player_tint_strength, 0.0, 1.0));
}


void main() {
  if (gfx_hack_no_tex == 0) {
    vec4 T0 = texture(tex_T0, vtx_st);
  
    if (darkjak_interp >= 0.0) {
      vec4 darkT0 = texture(tex_T1, vtx_st);
      T0 = mix(T0, darkT0, clamp(darkjak_interp, 0.0, 1.0));
    }
    // all merc is tcc=rgba and modulate
    if (decal_enable == 0) {
      color = vtx_color * T0 * 2;
    } else {
      color = T0;
    }

    if (player_tint_enabled != 0) {
      color.rgb = apply_player_tint(
        color.rgb,
        player_tint_color);
    }

    color.a *= 2;
  } else {
    color.rgb = vtx_color.rgb;

    if (decal_enable == 0) {
      color.a = vtx_color.a * 2;
    } else {
      color.a = 1;
    }
  }

  if (light_dir1_fade_en.w > 0) {
    color.a = light_dir0_fade.w;
  } else if (light_dir1_fade_en.w < 0) {
    color.a *= light_dir0_fade.w;
  }


  if (ignore_alpha == 0 && color.w < 0.128) {
    discard;
  }

  color.xyz = mix(color.xyz, fog_color.rgb, clamp(fog_color.a * fog, 0, 1));
}
