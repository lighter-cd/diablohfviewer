// Data tables for the Hell (L4) dungeon generator.
// Faithful port of the static tables in DevilutionX Source/levels/drlg_l4.cpp.
#pragma once

#include "decoders/dungeon_gen_common.h"

namespace dhfc {
namespace gen {

// A lookup table for the 16 possible patterns of a 2x2 area, where each cell
// either contains a wall or it doesn't. Index = (BR<<3)|(BL<<2)|(TR<<1)|(TL<<0).
const uint8_t L4ConvTbl[16] = { 30, 6, 1, 6, 2, 6, 6, 6, 9, 6, 1, 6, 2, 6, 3, 6 };

// Miniset: Stairs up.
const Miniset L4USTAIRS {
    4, 5,
    {
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
    },
    {
        { 0, 0, 0, 0 },
        { 36, 38, 35, 0 },
        { 37, 34, 33, 32 },
        { 0, 0, 31, 0 },
        { 0, 0, 0, 0 },
    }
};

// Miniset: Stairs up to town.
const Miniset L4TWARP {
    4, 5,
    {
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
        { 6, 6, 6, 6 },
    },
    {
        { 0, 0, 0, 0 },
        { 134, 136, 133, 0 },
        { 135, 132, 131, 130 },
        { 0, 0, 129, 0 },
        { 0, 0, 0, 0 },
    }
};

// Miniset: Stairs down.
const Miniset L4DSTAIRS {
    5, 5,
    {
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
    },
    {
        { 0, 0, 0, 0, 0 },
        { 0, 0, 45, 41, 0 },
        { 0, 44, 43, 40, 0 },
        { 0, 46, 42, 39, 0 },
        { 0, 0, 0, 0, 0 },
    }
};

// Miniset: Pentagram (closed hell gate, level 15).
const Miniset L4PENTA {
    5, 5,
    {
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
    },
    {
        { 0, 0, 0, 0, 0 },
        { 0, 98, 100, 103, 0 },
        { 0, 99, 102, 105, 0 },
        { 0, 101, 104, 106, 0 },
        { 0, 0, 0, 0, 0 },
    }
};

// Miniset: Pentagram portal (open hell gate, level 16).
const Miniset L4PENTA2 {
    5, 5,
    {
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
        { 6, 6, 6, 6, 6 },
    },
    {
        { 0, 0, 0, 0, 0 },
        { 0, 107, 109, 112, 0 },
        { 0, 108, 111, 114, 0 },
        { 0, 110, 113, 115, 0 },
        { 0, 0, 0, 0, 0 },
    }
};

// Maps tile IDs to their corresponding undecorated tile ID (used by Substitution).
const uint8_t L4BTYPES[140] = {
    0, 1, 2, 3, 4, 5, 6, 7, 8, 9,
    10, 11, 12, 13, 14, 15, 16, 17, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 6,
    6, 6, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 1, 2, 1, 2, 1, 2, 1, 1, 2,
    2, 0, 0, 0, 0, 0, 0, 15, 16, 9,
    12, 4, 5, 7, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

} // namespace gen
} // namespace dhfc
