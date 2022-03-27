#include "type.hpp"

#include <algorithm>

#include "robin/hash.hpp"
#include "robin/collection.hpp"

#include "alloca.hpp"
#include "array_pool.hpp"
#include "compiler_error.hpp"
#include "fixed.hpp"
#include "format.hpp"
#include "globals.hpp"
#include "group.hpp"
#include "pstring.hpp"
#include "eval.hpp"

using namespace std::literals;

struct array_thunk_t
{
    pstring_t pstring;
    type_t elem_type;
    token_t const* expr;
};

namespace  // Anonymous
{
    // Implementation detail!
    // This takes a range of types and returns a pointer to allocated memory
    // that contains the same data.
    // The point being, it's faster to pass a pointer around than the actual range.
    template<typename T>
    class tails_manager_t
    {
        struct map_elem_t
        {
            std::uint16_t size;
            T const* tail;
        };

        rh::robin_auto_table<map_elem_t> map;
        array_pool_t<T> tails;

    public:
        T const* get(T const* begin, T const* end)
        {
            if(end - begin == 0)
                return nullptr;

            // Hash the range.

            std::size_t size = end - begin;
            std::size_t hash = size;

            for(T const* it = begin; it < end; ++it)
            {
                std::hash<T> hasher;
                hash = rh::hash_combine(hash, hasher(*it));
            }

            // Now insert into the map:

            rh::apair<map_elem_t*, bool> result = map.emplace(
                hash,
                [begin, end, size](map_elem_t elem) -> bool
                {
                    return (elem.size == size && std::equal(begin, end, elem.tail));
                },
                [this, begin, end, size]() -> map_elem_t
                { 
                    return { size, tails.insert(begin, end) };
                });

            assert(std::equal(begin, end, result.first->tail));

            return result.first->tail;
        }

        T const* get(T const& t) { return get(&t, &t+1); }
    };

    thread_local tails_manager_t<type_t> type_tails;
    thread_local tails_manager_t<group_ht> group_tails;
    thread_local array_pool_t<array_thunk_t> array_thunks;
} // end anonymous namespace

type_t const* type_t::new_type(type_t const& type) { return type_tails.get(type); }

bool type_t::operator==(type_t o) const
{
    if(m_name != o.m_name || m_size != o.m_size)
        return false;

    if(has_type_tail(name()))
        return std::equal(types(), types() + type_tail_size(), o.types());
    else if(has_group_tail(name()))
        return std::equal(groups(), groups() + group_tail_size(), o.groups());

    return true;
}

group_ht type_t::group(unsigned i) const { return groups()[i]; }

type_t type_t::buffer(unsigned size)
{ 
    return type_t(TYPE_BUFFER, size); 
}

type_t type_t::array(type_t elem_type, unsigned size)
{ 
    assert(is_thunk(elem_type.name()) || !has_array(elem_type));
    return type_t(TYPE_ARRAY, size, type_tails.get(elem_type));
}

type_t type_t::array_thunk(pstring_t pstring, type_t elem_type, token_t const* tokens)
{
    return type_t(TYPE_ARRAY_THUNK, 0, &array_thunks.insert({ pstring, elem_type, tokens }));
}

type_t type_t::ptr(group_ht const* begin, group_ht const* end, bool banked)
{
    std::size_t const n = end - begin;
    group_ht* groups = ALLOCA_T(group_ht, n);
    std::copy(begin, end, groups);
    std::sort(groups, groups + n);
    group_ht* groups_end = std::unique(groups, groups + n);
    return type_t(banked ? TYPE_BANKED_PTR : TYPE_PTR, 
                  groups_end - groups, 
                  group_tails.get(groups, groups_end));
}

type_t type_t::fn(type_t* begin, type_t* end)
{ 
    return type_t(TYPE_FN, end - begin, type_tails.get(begin, end)); 
}

type_t type_t::struct_thunk(global_t const& global)
{
    return type_t(TYPE_STRUCT_THUNK, 0, &global);
}

type_t type_t::struct_(struct_t const& s)
{
    return type_t(TYPE_STRUCT, 0, &s);
}

std::size_t type_t::size_of() const
{
    if(is_arithmetic(name()))
        return whole_bytes(name()) + frac_bytes(name());

    switch(name())
    {
    default:                return 0; // Error!
    case TYPE_PTR:          return 2;
    case TYPE_BANKED_PTR:   return 3;
    case TYPE_ARRAY: return size() * types()[0].size_of();
    case TYPE_STRUCT:
        std::size_t size = 0;
        for(unsigned i = 0; i < struct_().fields().size(); ++i)
            size += struct_().field(i).type().size_of();
        return size;
    }
}

std::size_t type_t::array_length() const
{
    if(name() == TYPE_ARRAY)
        return size();
    return 0;
}

std::size_t type_t::hash() const
{
    std::size_t hash = name();
    hash = rh::hash_combine(hash, size());

    if(has_type_tail(name()))
        for(unsigned i = 0; i < type_tail_size(); ++i)
            hash = rh::hash_combine(hash, type(i).hash());
    else if(has_group_tail(name()))
        for(unsigned i = 0; i < group_tail_size(); ++i)
            hash = rh::hash_combine(hash, group(i).value);

    return hash;
}

std::string to_string(type_t type) 
{ 
    std::string str;

    switch(type.name())
    {
    default: 
        throw std::runtime_error(fmt("bad type %", (int)type.name()));
    case TYPE_ARRAY_THUNK:  str += "array thunk"sv; break;
    case TYPE_STRUCT_THUNK: str += "struct thunk"sv; break;
    case TYPE_VOID:  str += "Void"sv;  break;
    case TYPE_BOOL:  str += "Bool"sv;  break;
    case TYPE_INT:   str += "Int"sv;  break;
    case TYPE_REAL:  str += "Real"sv;  break;
    case TYPE_F1:    str += "F"sv;  break;
    case TYPE_F2:    str += "FF"sv;  break;
    case TYPE_F3:    str += "FFF"sv;  break;
    case TYPE_U10:   str += "U"sv;  break;
    case TYPE_U20:   str += "UU"sv;  break;
    case TYPE_U30:   str += "UUU"sv;  break;
    case TYPE_U11:   str += "UF"sv;  break;
    case TYPE_U21:   str += "UUF"sv;  break;
    case TYPE_U31:   str += "UUUF"sv;  break;
    case TYPE_U12:   str += "UFF"sv;  break;
    case TYPE_U22:   str += "UUFF"sv;  break;
    case TYPE_U32:   str += "UUUFF"sv;  break;
    case TYPE_U13:   str += "UFFF"sv;  break;
    case TYPE_U23:   str += "UUFFF"sv;  break;
    case TYPE_U33:   str += "UUUFFF"sv;  break;
    case TYPE_S10:   str += "S"sv;  break;
    case TYPE_S20:   str += "SS"sv;  break;
    case TYPE_S30:   str += "SSS"sv;  break;
    case TYPE_S11:   str += "SF"sv;  break;
    case TYPE_S21:   str += "SSF"sv;  break;
    case TYPE_S31:   str += "SSSF"sv;  break;
    case TYPE_S12:   str += "SFF"sv;  break;
    case TYPE_S22:   str += "SSFF"sv;  break;
    case TYPE_S32:   str += "SSSFF"sv;  break;
    case TYPE_S13:   str += "SFFF"sv;  break;
    case TYPE_S23:   str += "SSFFF"sv;  break;
    case TYPE_S33:   str += "SSSFFF"sv;  break;
    case TYPE_ARRAY:
        str = fmt("%[%]", to_string(type.elem_type()), type.size());
        break;
    case TYPE_STRUCT:
        str = type.struct_().global.name;
        break;
    case TYPE_BUFFER:
        str = fmt("buffer[%]", type.size());
        break;
    case TYPE_BANKED_PTR:
        str = "P";
        // fall-through
    case TYPE_PTR:
        str = "PP";
        for(unsigned i = 0; i < type.size(); ++i)
            str += type.group(i)->name;
        break;
    case TYPE_FN:
        assert(type.size() > 0);
        str = "fn("sv;
        for(unsigned i = 0; i < type.size(); ++i)
        {
            if(i == type.size() - 1)
                str += ") "sv;
            else if(i != 0)
                str += ", "sv;
            str += to_string(type.type(i));
        }
        break;
    }

    return str;
}

std::ostream& operator<<(std::ostream& ostr, type_t const& type)
{
    ostr << to_string(type);
    return ostr;
}

cast_result_t can_cast(type_t const& from, type_t const& to, bool implicit)
{
    // Buffers should be converted to ptrs, prior.
    assert(from.name() != TYPE_BUFFER && to.name() != TYPE_BUFFER);

    // Same types; no cast needed!
    if(from == to)
        return CAST_NOP;

    /* TODO: remove
    // Buffers can convert to pointers.
    // TODO: buffers should convert earlier, not as cast
    if(from.name() == TYPE_BUFFER)
    {
        if(from_ram && to.name() == TYPE_RAM_PTR)
        {
            if((1ull << ramb.value)  & to.ramb_bitset())
                return CAST_ADDROF;
            return CAST_FAIL;
        }
        else if(!from_ram && to.name() == TYPE_RAM_PTR)
        {
            if(bank == to.bank())
                return CAST_ADDROF;
            return CAST_FAIL;
        }
        return CAST_FAIL;
    }
    */

    // RAM pointers can generalize
    // i.e. ram{foo} can convert to ram{foo, bar}
    /* TODO
    if(from.name() == TYPE_RAM_PTR && to.name() == TYPE_RAM_PTR)
    {
        if((from.group_bitset() & to.group_bitset()) == from.group_bitset())
            return CAST_NOP;
        return CAST_FAIL;
    }
    */

    // Otherwise you can't cast different pointers.
    if(is_ptr(from.name()) || is_ptr(to.name()))
        return CAST_FAIL;

    // Othewise arithmetic types can be converted to bool using "!= 0".
    if(is_arithmetic(from.name()) && to == TYPE_BOOL)
        return CAST_BOOLIFY;

    // Otherwise Reals have special casting rules:
    if(from.name() == TYPE_REAL)
    {
        if(implicit && frac_bytes(to.name()) == 0)
            return CAST_FAIL; // Can't implicitly convert to non-fixed point.
        else if(is_arithmetic(to.name()))
            return CAST_ROUND_REAL; // Reals implement rounding.
    }

    // Otherwise Ints have special casting rules:
    if(from.name() == TYPE_INT && is_arithmetic(to.name()))
        return CAST_CONVERT_INT;

    // Otherwise arithmetic types can be converted amongst each other,
    if(is_arithmetic(from.name()) && is_arithmetic(to.name()))
    {
        if(is_arithmetic_subset(from.name(), to.name()))
            return CAST_PROMOTE;
        else
            return implicit ? CAST_FAIL : CAST_TRUNCATE;
    }

    return CAST_FAIL;
}

/* TODO
type_name_t smallest_representable(fixed_t fixed)
{
    if(!fixed)
        return TYPE_BYTE;

    int const min = builtin::ctz(fixed.value) / 8;
    int const max = builtin::rclz(fixed.value) / 8;

    int const whole = std::max(max - 3, 1);
    int const frac  = std::max(3 - min, 0);

    return TYPE_arithmetic(whole, frac);
}
*/

bool is_ct(type_t type)
{
    switch(type.name())
    {
    case TYPE_REAL:
    case TYPE_INT:
        return true;
    case TYPE_ARRAY:
        return is_ct(type.elem_type());
    case TYPE_STRUCT:
        for(auto const& pair : type.struct_().fields())
            if(is_ct(pair.second.type()))
                return true;
        return false;
    default:
        return false;
    }
}

unsigned num_members(type_t type)
{
    assert(!is_thunk(type.name()));

    if(type.name() == TYPE_STRUCT)
    {
        unsigned count = 0; 
        for(auto const& pair : type.struct_().fields())
            count += num_members(pair.second.type());
        return count;
    }
    else if(type.name() == TYPE_ARRAY)
        return num_members(type.elem_type());
    return 1;
}

unsigned num_atoms(type_t type)
{
    assert(!is_thunk(type.name()));

    switch(type.name())
    {
    case TYPE_STRUCT: assert(false); // TODO
    case TYPE_ARRAY: return 1;
    case TYPE_PTR: return 1;
    case TYPE_BANKED_PTR: return 2;
    default: return type.size_of();
    }
}

unsigned member_index(type_t const& type, unsigned i)
{
    assert(!is_thunk(type.name()));

    switch(type.name())
    {
    case TYPE_STRUCT: return type.struct_().member(i);
    case TYPE_ARRAY: return member_index(type.elem_type(), i);
    default: return 0;
    }
}

type_t member_type(type_t const& type, unsigned i)
{
    assert(i < num_members(type));
    if(type.name() == TYPE_STRUCT)
        return type.struct_().member_type(i);
    else if(type.name() == TYPE_ARRAY)
    {
        type_t mt = member_type(type.elem_type(), i);
        assert(!is_aggregate(mt.name()));
        return type_t::array(mt, type.size());
    }
    return type;
}

type_t strip_array(type_t const& type)
{
    if(type.name() == TYPE_ARRAY)
        return type.elem_type();
    return type;
}

bool has_array(type_t const& type)
{
    assert(type.name() != TYPE_STRUCT_THUNK);

    switch(type.name())
    {
    case TYPE_STRUCT: return type.struct_().has_array_member();
    case TYPE_ARRAY_THUNK:
    case TYPE_ARRAY: return true;
    default: return false;
    }
}

type_t dethunkify(src_type_t src_type, eval_t* env)
{
    type_t& t = src_type.type;

    assert(compiler_phase() == PHASE_COMPILE);
    switch(t.name())
    {
    case TYPE_STRUCT_THUNK:
        if(t.global().gclass() != GLOBAL_STRUCT)
            throw std::runtime_error(fmt("%: Expected struct type.", t.global().name));
        return type_t::struct_(t.global().impl<struct_t>());

    case TYPE_ARRAY_THUNK:
        {
            array_thunk_t const& thunk = t.array_thunk();
            spair_t const result = interpret_expr(thunk.pstring, thunk.expr, TYPE_U, env);
            assert(result.value.size());
            unsigned size = std::get<ssa_value_t>(result.value[0]).whole();
            if(has_array(thunk.elem_type))
                compiler_error(thunk.pstring, "Arrays cannot be multidimensional.");
            if(size <= 0 || size > 256)
                compiler_error(thunk.pstring, "Invalid array size.");
            return type_t::array(thunk.elem_type, size);
        }

    case TYPE_ARRAY:
        {
            type_t const elem = dethunkify({ t.elem_type(), src_type.pstring }, env);
            if(has_array(elem))
                compiler_error(src_type.pstring, "Arrays cannot be multi-dimensional.");
            return type_t::array(elem, t.size());
        }

    case TYPE_FN:
        {
            type_t* args = ALLOCA_T(type_t, t.size());
            for(unsigned i = 0; i < t.size(); ++i)
                args[i] = dethunkify({ t.type(i), src_type.pstring }, env);
            return type_t::fn(args, args + t.size());
        }

    default:
        return t;
    }
}

/////////////
// STRUCTS //
/////////////

/* TODO
type_t arg_struct(type_t fn_type)
{
    assert(fn_type.name() == TYPE_FN);
    assert(fn_type.size() >= 1);
    fn_type.m_name = TYPE_STRUCT;
    fn_type.m_size -= 1;
    return fn_type;
}

std::size_t struct_size(type_t type)
{
    if(type.name() != TYPE_STRUCT)
        return 1;
    std::size_t size = 0;
    for(type_t const& sub : type)
        size += struct_size(sub);
    return size;
}

static type_t const* _struct_index(type_t const& type, unsigned& i)
{
    if(type.name() != TYPE_STRUCT)
    {
        if(i == 0)
            return &type;
        --i;
        return nullptr;
    }

    for(type_t const& sub : type)
        if(type_t const* found = _struct_index(sub, i))
            return found;

    return nullptr;
}

type_t struct_index(type_t type, unsigned i)
{
    type_t const* result = _struct_index(type, i);
    assert(result);
    return *result;
}

static void _struct_fill(type_t type, type_t*& vec)
{
    if(type.name() != TYPE_STRUCT)
        *(vec++) = type;
    else for(type_t const& sub : type)
        _struct_fill(sub, vec);
}

void struct_fill(type_t type, type_t* vec)
{
    struct_fill(type, vec);
}
*/

