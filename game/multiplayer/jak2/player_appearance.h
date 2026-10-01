#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

#include "game/multiplayer/platform/core/types.h"

using multiplayer::platform::PlayerCharacter;

enum class MPPlayerAppearanceGroup : uint32_t {
  PRIMARY = 0,
  JAK_JACKET = 1,
  JAK_ARMOR = 2,
  JAK_LEGGINGS = 3,
  JAK_PANTS = 4,
  JAK_BOOTS = 5,
  JAK_SCARF = 6,
  JAK_POUCH = 7,
  JAK_STRAPS = 8,
  JAK_GLOVES = 9,
  DAXTER_HAT = 16,
  DAXTER_FUR = 17,
  INVALID = 0xffffffffu,
};

enum class MPPlayerTintPolicy : uint8_t {
  GRAYSCALE_DETAIL = 0,
  WHITE_BASE = 1,
};

inline constexpr size_t kMPPlayerAppearanceSlotCount = 32;
inline constexpr size_t kMPJakAppearanceSlotBegin = 1;
inline constexpr size_t kMPJakAppearanceSlotEnd = 16;
inline constexpr size_t kMPDaxterAppearanceSlotBegin = 16;
inline constexpr size_t kMPDaxterAppearanceSlotEnd = kMPPlayerAppearanceSlotCount;
inline constexpr uint8_t kMPInvalidPlayerAppearanceSlot = 0xff;

static_assert(kMPJakAppearanceSlotBegin < kMPJakAppearanceSlotEnd);
static_assert(kMPJakAppearanceSlotEnd == kMPDaxterAppearanceSlotBegin);
static_assert(kMPDaxterAppearanceSlotEnd <= kMPInvalidPlayerAppearanceSlot);
static_assert(static_cast<size_t>(MPPlayerAppearanceGroup::JAK_GLOVES) < kMPJakAppearanceSlotEnd);
static_assert(static_cast<size_t>(MPPlayerAppearanceGroup::DAXTER_HAT) >=
              kMPDaxterAppearanceSlotBegin);
static_assert(static_cast<size_t>(MPPlayerAppearanceGroup::DAXTER_FUR) <
              kMPDaxterAppearanceSlotEnd);

struct MPPlayerAppearance {
  std::array<uint32_t, kMPPlayerAppearanceSlotCount> colors = {};
  std::array<float, kMPPlayerAppearanceSlotCount> strengths = {};
};
static_assert(sizeof(MPPlayerAppearance) == 256);

struct MPPlayerTextureGroupDefinition {
  MPPlayerAppearanceGroup group;
  std::string_view name;
  std::string_view preference_key;
  PlayerCharacter character;
  MPPlayerTintPolicy tint_policy;
  std::span<const std::string_view> textures;
};

inline constexpr std::array<std::string_view, 4> kMPJakJacketTextures = {
    "jakbsmall-jacketbody",
    "jakbsmall-jacketsleeve",
    "jakb-jacketbody",
    "jakb-jacketsleeve",
};
inline constexpr std::array<std::string_view, 2> kMPJakArmorTextures = {
    "jakbsmall-armor",
    "jakb-armor",
};
inline constexpr std::array<std::string_view, 1> kMPJakLeggingsTextures = {
    "jakbsmall-leggging",
};
inline constexpr std::array<std::string_view, 2> kMPJakPantsTextures = {
    "jakbsmall-pants",
    "jakb-pants",
};
inline constexpr std::array<std::string_view, 6> kMPJakBootsTextures = {
    "jakbsmall-shoetop", "jakbsmall-shoebottom", "jakb-shoeteop",
    "jakb-shoebottom",   "jakb-shoemetal",       "jakb-lightbrownspat",
};
inline constexpr std::array<std::string_view, 2> kMPJakScarfTextures = {
    "jakbsmall-scarf",
    "jakb-scarf",
};
inline constexpr std::array<std::string_view, 2> kMPJakPouchTextures = {
    "jakbsmall-leatherpouch",
    "jakb-leatherpouch",
};
inline constexpr std::array<std::string_view, 8> kMPJakStrapsTextures = {
    "jak-belt",        "jakbsmall-blackstrap", "jakbsmall-brownleather", "jakbsmall-leatherstrap",
    "jakb-blackstrap", "jakb-brownleather",    "jakb-leatherstrap",      "jakb-lightbrownstrap",
};
inline constexpr std::array<std::string_view, 2> kMPJakGlovesTextures = {
    "jakbsmall-glovetop",
    "jakb-glovetop",
};
inline constexpr std::array<std::string_view, 2> kMPDaxterHatTextures = {
    "bam-leather-belt",
    "daxterhelmetplain",
};
inline constexpr std::array<std::string_view, 20> kMPDaxterFurTextures = {
    "bam-hairhilite",  "sk-armfur",           "sk-bodyfur",          "sk-ear",
    "sk-eye-lid",      "sk-finger",           "sk-orange2yellowfur", "sk-solidorangefur",
    "sk-yellowfurnew", "daxter-eyelid",       "daxter-furhilite",    "daxter-orange",
    "daxterarm",       "daxterbodyshort-eix", "daxterear",           "daxterfinger",
    "daxterfoot",      "daxterfoot-bottom",   "daxterheadwidenew",   "daxtertuft",
};

inline constexpr auto kMPPlayerTextureGroups = std::to_array<MPPlayerTextureGroupDefinition>({
    {.group = MPPlayerAppearanceGroup::JAK_JACKET,
     .name = "Jacket",
     .preference_key = "jak_jacket",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakJacketTextures},
    {.group = MPPlayerAppearanceGroup::JAK_ARMOR,
     .name = "Armor",
     .preference_key = "jak_armor",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakArmorTextures},
    {.group = MPPlayerAppearanceGroup::JAK_LEGGINGS,
     .name = "Leggings",
     .preference_key = "jak_leggings",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakLeggingsTextures},
    {.group = MPPlayerAppearanceGroup::JAK_PANTS,
     .name = "Pants",
     .preference_key = "jak_pants",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakPantsTextures},
    {.group = MPPlayerAppearanceGroup::JAK_BOOTS,
     .name = "Boots",
     .preference_key = "jak_boots",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakBootsTextures},
    {.group = MPPlayerAppearanceGroup::JAK_SCARF,
     .name = "Scarf",
     .preference_key = "jak_scarf",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakScarfTextures},
    {.group = MPPlayerAppearanceGroup::JAK_POUCH,
     .name = "Pouch",
     .preference_key = "jak_pouch",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakPouchTextures},
    {.group = MPPlayerAppearanceGroup::JAK_STRAPS,
     .name = "Straps",
     .preference_key = "jak_straps",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakStrapsTextures},
    {.group = MPPlayerAppearanceGroup::JAK_GLOVES,
     .name = "Gloves",
     .preference_key = "jak_gloves",
     .character = PlayerCharacter::JAK,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPJakGlovesTextures},
    {.group = MPPlayerAppearanceGroup::DAXTER_HAT,
     .name = "Hat",
     .preference_key = "daxter_hat",
     .character = PlayerCharacter::DAXTER,
     .tint_policy = MPPlayerTintPolicy::WHITE_BASE,
     .textures = kMPDaxterHatTextures},
    {.group = MPPlayerAppearanceGroup::DAXTER_FUR,
     .name = "Fur",
     .preference_key = "daxter_fur",
     .character = PlayerCharacter::DAXTER,
     .tint_policy = MPPlayerTintPolicy::GRAYSCALE_DETAIL,
     .textures = kMPDaxterFurTextures},
});

constexpr size_t player_appearance_group_index(MPPlayerAppearanceGroup group) {
  return static_cast<size_t>(group);
}

constexpr bool is_player_character_valid(const PlayerCharacter character) {
  return character == PlayerCharacter::JAK || character == PlayerCharacter::DAXTER;
}

inline const MPPlayerTextureGroupDefinition* get_player_texture_group_definition(
    const MPPlayerAppearanceGroup group) {
  for (const auto& definition : kMPPlayerTextureGroups) {
    if (definition.group == group) {
      return &definition;
    }
  }
  return nullptr;
}

inline bool is_player_appearance_slot_registered(const size_t slot) {
  if (slot == player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)) {
    return true;
  }
  for (const auto& definition : kMPPlayerTextureGroups) {
    if (player_appearance_group_index(definition.group) == slot) {
      return true;
    }
  }
  return false;
}

inline MPPlayerAppearanceGroup get_player_texture_group_for_name(
    const std::string_view texture_name) {
  for (const auto& definition : kMPPlayerTextureGroups) {
    for (const auto candidate : definition.textures) {
      if (candidate == texture_name) {
        return definition.group;
      }
    }
  }
  return MPPlayerAppearanceGroup::INVALID;
}

inline PlayerCharacter get_player_model_character(const std::string_view model_name) {
  if (model_name == "jakb-lod0" || model_name == "jak-highres-lod0") {
    return PlayerCharacter::JAK;
  }
  if (model_name == "daxter-lod0" || model_name == "daxter-highres-lod0") {
    return PlayerCharacter::DAXTER;
  }
  return PlayerCharacter::UNKNOWN;
}

inline bool is_player_appearance_valid(const MPPlayerAppearance& appearance) {
  constexpr size_t primary_slot = player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY);
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    if ((appearance.colors[slot] & 0xff000000u) != 0 ||
        !std::isfinite(appearance.strengths[slot]) || appearance.strengths[slot] < 0.0f ||
        appearance.strengths[slot] > 1.0f) {
      return false;
    }
    if (!is_player_appearance_slot_registered(slot) &&
        (appearance.colors[slot] != appearance.colors[primary_slot] ||
         appearance.strengths[slot] != 0.0f)) {
      return false;
    }
  }
  return appearance.strengths[primary_slot] == 0.0f;
}

inline MPPlayerAppearance get_default_player_appearance(const uint32_t primary_color) {
  MPPlayerAppearance appearance = {};
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    appearance.colors[slot] = primary_color;
  }
  appearance.strengths[player_appearance_group_index(MPPlayerAppearanceGroup::JAK_JACKET)] = 1.0f;
  appearance.strengths[player_appearance_group_index(MPPlayerAppearanceGroup::DAXTER_HAT)] = 1.0f;
  return appearance;
}
