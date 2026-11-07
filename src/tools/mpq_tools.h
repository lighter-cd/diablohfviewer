// src/tools/mpq_tools.h
//
// Declarations for the MPQ tool group (the C/C++ port of the former
// TypeScript orchestration layer). See mpq_tools.cpp for the tool bodies.

#pragma once

#include "tools/registry.h"

namespace mcp {
namespace tools {

// Register mpq.list / mpq.extract / mpq.verify / mpq.recover.
void registerMpqTools(ToolRegistry& reg);

} // namespace tools
} // namespace mcp
