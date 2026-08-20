#version 410 core

out vec4 color;
in vec4 vtx_color;
in vec3 vtx_material_color;
in vec3 vtx_light_color;
in vec2 vtx_st;
in float fog;

uniform sampler2D tex_T0;
uniform sampler2D tex_T1;

uniform float darkjak_interp;

uniform vec3 player_tint_color;
uniform int player_tint_enabled;
uniform float player_tint_strength;
uniform int player_tint_white_base;

uniform vec4 fog_color;
uniform int ignore_alpha;
uniform vec4 light_dir0_fade;
uniform vec4 light_dir1_fade_en;

uniform int decal_enable;

uniform int gfx_hack_no_tex;

const vec3 PLAYER_TINT_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
const float PLAYER_TINT_MATERIAL_NORMALIZATION = 0.5;
const float PLAYER_TINT_DETAIL_CONTRAST = 0.65;
const float PLAYER_TINT_DARK_DETAIL_MIX = 0.18;
const float PLAYER_TINT_WHITE_BASE_DETAIL_MIX = 0.25;


vec3 player_tint_grayscale_detail(
  vec3 source_material,
  vec3 target_color) {
  float source_luminance = clamp(
    dot(source_material, PLAYER_TINT_LUMINANCE_WEIGHTS) *
      PLAYER_TINT_MATERIAL_NORMALIZATION,
    0.0,
    1.0);
  float target_luminance = dot(
    target_color,
    PLAYER_TINT_LUMINANCE_WEIGHTS);

  float detail_luminance = mix(
    1.0,
    source_luminance,
    PLAYER_TINT_DETAIL_CONTRAST);
  float neutral_detail_mix =
    PLAYER_TINT_DARK_DETAIL_MIX * (1.0 - target_luminance);
  vec3 detail_supported_target = mix(
    target_color,
    vec3(1.0),
    neutral_detail_mix);

  return detail_supported_target * detail_luminance;
}


vec3 player_tint_white_base_detail(
  vec3 source_material,
  vec3 target_color) {
  float source_luminance = clamp(
    dot(source_material, PLAYER_TINT_LUMINANCE_WEIGHTS) *
      PLAYER_TINT_MATERIAL_NORMALIZATION,
    0.0,
    1.0);

  return mix(
    target_color,
    vec3(source_luminance),
    PLAYER_TINT_WHITE_BASE_DETAIL_MIX);
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
      vec3 source_material = T0.rgb;
      vec3 scene_light = vec3(1.0);
      if (decal_enable == 0) {
        source_material = vtx_material_color * T0.rgb * 2.0;
        scene_light = vtx_light_color;
      }

      vec3 tinted_material;
      if (player_tint_white_base != 0) {
        tinted_material = player_tint_white_base_detail(
          source_material,
          player_tint_color);
      } else {
        tinted_material = player_tint_grayscale_detail(
          source_material,
          player_tint_color);
      }
      vec3 tinted_lit_color = tinted_material * scene_light;
      color.rgb = mix(
        color.rgb,
        tinted_lit_color,
        clamp(player_tint_strength, 0.0, 1.0));
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
