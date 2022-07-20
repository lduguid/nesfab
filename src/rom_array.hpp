#ifndef ROM_ARRAY_HPP
#define ROM_ARRAY_HPP

#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

#include "robin/map.hpp"

#include "rom_array_decl.hpp"
#include "bitset.hpp"
#include "decl.hpp"
#include "ir_decl.hpp"
#include "locator.hpp"


struct rom_array_t
{
    std::vector<locator_t> data;

    auto operator<=>(rom_array_t const&) const = default;
};

struct rom_array_meta_t
{
    rom_array_meta_t();

    void mark_used_by(fn_ht fn);
    void mark_used_by(group_data_ht group_data);

    static rom_array_meta_t& get(rom_array_ht);

    std::mutex mutex; // Protects the members below
    bitset_t used_by_fns;
    bitset_t used_by_group_data;
};

template<>
struct std::hash<rom_array_t>
{
    std::size_t operator()(rom_array_t const& a) const noexcept
    {
        std::hash<locator_t> lh;

        // Include array size in the hash:
        std::size_t h = a.data.size();

        // Only hash the first 4 locators, for speed:
        unsigned const n = std::min<unsigned>(a.data.size(), 4);
        for(unsigned i = 0; i < n; ++i)
            h = rh::hash_combine(h, lh(a.data[i]));

        // Also hash the last locator:
        h = rh::hash_combine(h, lh(a.data.back()));

        return h;
    }
};

using rom_array_map_t = rh::joker_map<rom_array_t, rom_array_meta_t>;
extern std::mutex rom_array_map_mutex;
extern rom_array_map_t rom_array_map;

// Creates a new rom_array, or returns an existing one matching 'rom_array'.
locator_t lookup_rom_array(fn_ht fn, group_data_ht group_data, rom_array_t&& rom_array, std::uint16_t offset=0);

// Orders CFG basic blocks, trying to find an optimal layout for branches.
void build_rom_arrays(fn_ht fn, ir_t& ir);

#endif