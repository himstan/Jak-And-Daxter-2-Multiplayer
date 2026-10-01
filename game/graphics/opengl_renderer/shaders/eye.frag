#version 410 core

out vec4 color;
in vec2 st;
uniform sampler2D tex_T0;
uniform vec3 lid_tint_color;
uniform float lid_tint_strength;

const vec3 PLAYER_TINT_LUMINANCE_WEIGHTS = vec3(0.2126, 0.7152, 0.0722);
const float PLAYER_TINT_MATERIAL_NORMALIZATION = 0.5;
const float PLAYER_TINT_DETAIL_CONTRAST = 0.65;
const float PLAYER_TINT_DARK_DETAIL_MIX = 0.18;

vec3 player_tint_grayscale_detail(vec3 source_material, vec3 target_color) {
  float source_luminance = clamp(
    dot(source_material, PLAYER_TINT_LUMINANCE_WEIGHTS) *
      PLAYER_TINT_MATERIAL_NORMALIZATION,
    0.0,
    1.0);
  float target_luminance = dot(target_color, PLAYER_TINT_LUMINANCE_WEIGHTS);
  float detail_luminance = mix(1.0, source_luminance, PLAYER_TINT_DETAIL_CONTRAST);
  float neutral_detail_mix = PLAYER_TINT_DARK_DETAIL_MIX * (1.0 - target_luminance);
  vec3 detail_supported_target = mix(target_color, vec3(1.0), neutral_detail_mix);
  return detail_supported_target * detail_luminance;
}

void main() {
  color = texture(tex_T0, st);
  vec3 tinted_material = player_tint_grayscale_detail(color.rgb, lid_tint_color);
  color.rgb = mix(color.rgb, tinted_material, clamp(lid_tint_strength, 0.0, 1.0));
  color.w *= 2;
}
