#pragma once

#include <optional>
#include <string>

std::optional<void*> claim_device(const std::string& claim_key);
void release_device(void* claim);
