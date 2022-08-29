#include "ast.hpp"

unsigned ast_node_t::num_children() const
{
    using namespace lex;

    switch(token.type)
    {
    case TOK_apply:
    case TOK_cast:
    case TOK_push_paa:
        assert(children);
        return token.value;

    case TOK_unary_minus:
    case TOK_unary_xor:
    case TOK_unary_negate:
    case TOK_sizeof_expr:
    case TOK_len_expr:
    case TOK_period:
        assert(children);
        return 1;

    default:
        if(is_operator(token.type))
        {
            assert(children);
            return 2;
        }

        assert(!children);
        return 0;
    }
}