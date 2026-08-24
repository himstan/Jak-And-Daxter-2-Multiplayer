#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

enum class MPPlayerCharacter : uint32_t {
  UNKNOWN = 0,
  JAK = 1,
  DAXTER = 2,
};

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
static_assert(static_cast<size_t>(MPPlayerAppearanceGroup::JAK_GLOVES) <
              kMPJakAppearanceSlotEnd);
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
  MPPlayerCharacter character;
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
    "jakbsmall-shoetop",
    "jakbsmall-shoebottom",
    "jakb-shoeteop",
    "jakb-shoebottom",
    "jakb-shoemetal",
    "jakb-lightbrownspat",
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
    "jak-belt",
    "jakbsmall-blackstrap",
    "jakbsmall-brownleather",
    "jakbsmall-leatherstrap",
    "jakb-blackstrap",
    "jakb-brownleather",
    "jakb-leatherstrap",
    "jakb-lightbrownstrap",
};
inline constexpr std::array<std::string_view, 2> kMPJakGlovesTextures = {
    "jakbsmall-glovetop",
    "jakb-glovetop",
};
inline constexpr std::array<std::string_view, 2> kMPDaxterHatTextures = {
    "bam-leather-belt",
    "daxterhelmetplain",
};
inline constexpr std::array<std::string_view, 18> kMPDaxterFurTextures = {
    "bam-hairhilite",
    "sk-armfur",
    "sk-bodyfur",
    "sk-ear",
    "sk-finger",
    "sk-orange2yellowfur",
    "sk-solidorangefur",
    "sk-yellowfurnew",
    "daxter-furhilite",
    "daxter-orange",
    "daxterarm",
    "daxterbodyshort-eix",
    "daxterear",
    "daxterfinger",
    "daxterfoot",
    "daxterfoot-bottom",
    "daxterheadwidenew",
    "daxtertuft",
};

inline constexpr auto kMPPlayerTextureGroups =
    std::to_array<MPPlayerTextureGroupDefinition>({
        {MPPlayerAppearanceGroup::JAK_JACKET, "Jacket", "jak_jacket",
         MPPlayerCharacter::JAK, MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakJacketTextures},
        {MPPlayerAppearanceGroup::JAK_ARMOR, "Armor", "jak_armor", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakArmorTextures},
        {MPPlayerAppearanceGroup::JAK_LEGGINGS, "Leggings", "jak_leggings",
         MPPlayerCharacter::JAK, MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakLeggingsTextures},
        {MPPlayerAppearanceGroup::JAK_PANTS, "Pants", "jak_pants", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakPantsTextures},
        {MPPlayerAppearanceGroup::JAK_BOOTS, "Boots", "jak_boots", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakBootsTextures},
        {MPPlayerAppearanceGroup::JAK_SCARF, "Scarf", "jak_scarf", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakScarfTextures},
        {MPPlayerAppearanceGroup::JAK_POUCH, "Pouch", "jak_pouch", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakPouchTextures},
        {MPPlayerAppearanceGroup::JAK_STRAPS, "Straps", "jak_straps", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakStrapsTextures},
        {MPPlayerAppearanceGroup::JAK_GLOVES, "Gloves", "jak_gloves", MPPlayerCharacter::JAK,
         MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPJakGlovesTextures},
        {MPPlayerAppearanceGroup::DAXTER_HAT, "Hat", "daxter_hat",
         MPPlayerCharacter::DAXTER, MPPlayerTintPolicy::WHITE_BASE, kMPDaxterHatTextures},
        {MPPlayerAppearanceGroup::DAXTER_FUR, "Fur", "daxter_fur",
         MPPlayerCharacter::DAXTER, MPPlayerTintPolicy::GRAYSCALE_DETAIL, kMPDaxterFurTextures},
    });

inline constexpr size_t mp_player_appearance_group_index(MPPlayerAppearanceGroup group) {
  return static_cast<size_t>(group);
}

inline constexpr bool mp_valid_player_character(MPPlayerCharacter character) {
  return character == MPPlayerCharacter::JAK || character == MPPlayerCharacter::DAXTER;
}

inline const MPPlayerTextureGroupDefinition* mp_player_texture_group_definition(
    MPPlayerAppearanceGroup group) {
  for (const auto& definition : kMPPlayerTextureGroups) {
    if (definition.group == group) {
      return &definition;
    }
  }
  return nullptr;
}

inline bool mp_player_appearance_slot_registered(size_t slot) {
  if (slot == mp_player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY)) {
    return true;
  }
  for (const auto& definition : kMPPlayerTextureGroups) {
    if (mp_player_appearance_group_index(definition.group) == slot) {
      return true;
    }
  }
  return false;
}

inline MPPlayerAppearanceGroup mp_player_texture_group_for_name(std::string_view texture_name) {
  for (const auto& definition : kMPPlayerTextureGroups) {
    for (const auto candidate : definition.textures) {
      if (candidate == texture_name) {
        return definition.group;
      }
    }
  }
  return MPPlayerAppearanceGroup::INVALID;
}

inline MPPlayerCharacter mp_player_model_character(std::string_view model_name) {
  if (model_name == "jakb-lod0" || model_name == "jak-highres-lod0") {
    return MPPlayerCharacter::JAK;
  }
  if (model_name == "daxter-lod0" || model_name == "daxter-highres-lod0") {
    return MPPlayerCharacter::DAXTER;
  }
  return MPPlayerCharacter::UNKNOWN;
}

inline bool mp_valid_player_appearance(const MPPlayerAppearance& appearance) {
  const size_t primary_slot =
      mp_player_appearance_group_index(MPPlayerAppearanceGroup::PRIMARY);
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    if ((appearance.colors[slot] & 0xff000000u) != 0 ||
        !std::isfinite(appearance.strengths[slot]) || appearance.strengths[slot] < 0.0f ||
        appearance.strengths[slot] > 1.0f) {
      return false;
    }
    if (!mp_player_appearance_slot_registered(slot) &&
        (appearance.colors[slot] != appearance.colors[primary_slot] ||
         appearance.strengths[slot] != 0.0f)) {
      return false;
    }
  }
  return appearance.strengths[primary_slot] == 0.0f;
}

inline MPPlayerAppearance mp_default_player_appearance(uint32_t primary_color,
                                                       float legacy_strength = 1.0f) {
  MPPlayerAppearance appearance = {};
  for (size_t slot = 0; slot < kMPPlayerAppearanceSlotCount; ++slot) {
    appearance.colors[slot] = primary_color;
  }
  appearance.strengths[mp_player_appearance_group_index(
      MPPlayerAppearanceGroup::JAK_JACKET)] = legacy_strength;
  appearance.strengths[mp_player_appearance_group_index(
      MPPlayerAppearanceGroup::DAXTER_HAT)] = legacy_strength;
  return appearance;
}
