// Copyright 2007-2008 Wincent Colaiuta
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include "parser.h"
#include "ary.h"
#include "str.h"
#include "wikitext.h"
#include "wikitext_ragel.h"

#define IN(type) ary_includes(parser->scope, type)

// poor man's object orientation in C:
// instead of parsing around multiple parameters between functions in the parser
// we pack everything into a struct and pass around only a pointer to that
// TODO: consider changing some of the VALUE members (eg link_target) to the more efficient str_t type
typedef struct
{
    VALUE   output;                 // for accumulating output to be returned
    VALUE   capture;                // for capturing substrings
    VALUE   link_target;            // short term "memory" for parsing links
    VALUE   link_text;              // short term "memory" for parsing links
    VALUE   external_link_class;    // CSS class applied to external links
    VALUE   img_prefix;             // path prepended when emitting img tags
    ary_t   *scope;                 // stack for tracking scope
    ary_t   *line;                  // stack for tracking scope as implied by current line
    ary_t   *line_buffer;           // stack for tracking raw tokens (not scope) on current line
    VALUE   pending_crlf;           // boolean (Qtrue or Qfalse)
    VALUE   autolink;               // boolean (Qtrue or Qfalse)
    VALUE   treat_slash_as_special; // boolean (Qtrue or Qfalse)
    VALUE   space_to_underscore;    // boolean (Qtrue or Qfalse)
    VALUE   special_link;           // boolean (Qtrue or Qfalse): is the current link_target a "special" link?
    str_t   *line_ending;
    int     base_indent;            // controlled by the :indent option to Wikitext::Parser#parse
    int     current_indent;         // fluctuates according to currently nested structures
    str_t   *tabulation;            // caching buffer for emitting indentation
    VALUE   parameters;
} parser_t;

const char escaped_no_wiki_start[]      = "&lt;nowiki&gt;";
const char escaped_no_wiki_end[]        = "&lt;/nowiki&gt;";
const char literal_strong_em[]          = "'''''";
const char literal_strong[]             = "'''";
const char literal_em[]                 = "''";
const char escaped_em_start[]           = "&lt;em&gt;";
const char escaped_em_end[]             = "&lt;/em&gt;";
const char escaped_strong_start[]       = "&lt;strong&gt;";
const char escaped_strong_end[]         = "&lt;/strong&gt;";
const char escaped_tt_start[]           = "&lt;tt&gt;";
const char escaped_tt_end[]             = "&lt;/tt&gt;";
const char literal_h6[]                 = "======";
const char literal_h5[]                 = "=====";
const char literal_h4[]                 = "====";
const char literal_h3[]                 = "===";
const char literal_h2[]                 = "==";
const char literal_h1[]                 = "=";
const char pre_start[]                  = "<pre>";
const char pre_end[]                    = "</pre>";
const char escaped_pre_start[]          = "&lt;pre&gt;";
const char escaped_pre_end[]            = "&lt;/pre&gt;";
const char blockquote_start[]           = "<blockquote>";
const char blockquote_end[]             = "</blockquote>";
const char escaped_blockquote_start[]   = "&lt;blockquote&gt;";
const char escaped_blockquote_end[]     = "&lt;/blockquote&gt;";
const char strong_em_start[]            = "<strong><em>";
const char strong_start[]               = "<strong>";
const char strong_end[]                 = "</strong>";
const char em_start[]                   = "<em>";
const char em_end[]                     = "</em>";
const char tt_start[]                   = "<tt>";
const char tt_end[]                     = "</tt>";
const char ol_start[]                   = "<ol>";
const char ol_end[]                     = "</ol>";
const char ul_start[]                   = "<ul>";
const char ul_end[]                     = "</ul>";
const char li_start[]                   = "<li>";
const char li_end[]                     = "</li>";
const char h6_start[]                   = "<h6>";
const char h6_end[]                     = "</h6>";
const char h5_start[]                   = "<h5>";
const char h5_end[]                     = "</h5>";
const char h4_start[]                   = "<h4>";
const char h4_end[]                     = "</h4>";
const char h3_start[]                   = "<h3>";
const char h3_end[]                     = "</h3>";
const char h2_start[]                   = "<h2>";
const char h2_end[]                     = "</h2>";
const char h1_start[]                   = "<h1>";
const char h1_end[]                     = "</h1>";
const char p_start[]                    = "<p>";
const char p_end[]                      = "</p>";
const char space[]                      = " ";
const char a_start[]                    = "<a href=\"";
const char a_class[]                    = "\" class=\"";
const char a_start_close[]              = "\">";
const char a_end[]                      = "</a>";
const char link_start[]                 = "[[";
const char link_end[]                   = "]]";
const char separator[]                  = "|";
const char ext_link_start[]             = "[";
const char backtick[]                   = "`";
const char quote[]                      = "\"";
const char ampersand[]                  = "&";
const char quot_entity[]                = "&quot;";
const char amp_entity[]                 = "&amp;";
const char lt_entity[]                  = "&lt;";
const char gt_entity[]                  = "&gt;";
const char escaped_blockquote[]         = "&gt; ";
const char ext_link_end[]               = "]";
const char literal_img_start[]          = "{{";
const char img_start[]                  = "<img src=\"";
const char img_end[]                    = "\" />";
const char img_alt[]                    = "\" alt=\"";
const char literal_parameter_start[]    = "{{{";
const char literal_parameter_end[]    = "}}}";

// for testing and debugging only
VALUE Wikitext_parser_tokenize(VALUE self, VALUE string)
{
    if (NIL_P(string))
        return Qnil;
    string = StringValue(string);
    VALUE tokens = rb_ary_new();
    char *p = RSTRING_PTR(string);
    long len = RSTRING_LEN(string);
    char *pe = p + len;
    token_t token;
    next_token(&token, NULL, p, pe);
    rb_ary_push(tokens, _Wikitext_token(&token));
    while (token.type != END_OF_FILE)
    {
        next_token(&token, &token, NULL, pe);
        rb_ary_push(tokens, _Wikitext_token(&token));
    }
    return tokens;
}

// for benchmarking raw tokenization speed only
VALUE Wikitext_parser_benchmarking_tokenize(VALUE self, VALUE string)
{
    if (NIL_P(string))
        return Qnil;
    string = StringValue(string);
    char *p = RSTRING_PTR(string);
    long len = RSTRING_LEN(string);
    char *pe = p + len;
    token_t token;
    next_token(&token, NULL, p, pe);
    while (token.type != END_OF_FILE)
        next_token(&token, &token, NULL, pe);
    return Qnil;
}

VALUE Wikitext_parser_fulltext_tokenize(int argc, VALUE *argv, VALUE self)
{
    // process arguments
    VALUE string, options;
    if (rb_scan_args(argc, argv, "11", &string, &options) == 1) // 1 mandatory argument, 1 optional argument
        options = Qnil;
    if (NIL_P(string))
        return Qnil;
    string = StringValue(string);
    VALUE tokens = rb_ary_new();

    // check instance variables
    VALUE min = rb_iv_get(self, "@minimum_fulltext_token_length");

    // process options hash (can override instance variables)
    if (!NIL_P(options) && TYPE(options) == T_HASH)
    {
        if (rb_funcall(options, rb_intern("has_key?"), 1, ID2SYM(rb_intern("minimum"))) == Qtrue)
            min = rb_hash_aref(options, ID2SYM(rb_intern("minimum")));
    }
    int min_len = NIL_P(min) ? 3 : NUM2INT(min);
    if (min_len < 0)
        min_len = 0;

    // set up scanner
    char *p = RSTRING_PTR(string);
    long len = RSTRING_LEN(string);
    char *pe = p + len;
    token_t token;
    token_t *_token = &token;
    next_token(&token, NULL, p, pe);
    while (token.type != END_OF_FILE)
    {
        switch (token.type)
        {
            case URI:
            case MAIL:
            case ALNUM:
                if (TOKEN_LEN(_token) >= min_len)
                    rb_ary_push(tokens, TOKEN_TEXT(_token));
                break;
            default:
                // ignore everything else
                break;
        }
        next_token(&token, &token, NULL, pe);
    }
    return tokens;
}

// we downcase "in place", overwriting the original contents of the buffer and returning the same string
VALUE _Wikitext_downcase(VALUE string)
{
    char *ptr   = RSTRING_PTR(string);
    long len    = RSTRING_LEN(string);
    for (long i = 0; i < len; i++)
    {
        if (ptr[i] >= 'A' && ptr[i] <= 'Z')
            ptr[i] += 32;
    }
    return string;
}

VALUE _Wikitext_hyperlink(VALUE link_prefix, VALUE link_target, VALUE link_text, VALUE link_class)
{
    VALUE string = rb_str_new(a_start, sizeof(a_start) - 1);        // <a href="
    if (!NIL_P(link_prefix))
        rb_str_append(string, link_prefix);
    rb_str_append(string, link_target);
    if (link_class != Qnil)
    {
        rb_str_cat(string, a_class, sizeof(a_class) - 1);           // " class="
        rb_str_append(string, link_class);
    }
    rb_str_cat(string, a_start_close, sizeof(a_start_close) - 1);   // ">
    rb_str_append(string, link_text);
    rb_str_cat(string, a_end, sizeof(a_end) - 1);
    return string;
}

void _Wikitext_append_img(parser_t *parser, char *token_ptr, int token_len)
{
    rb_str_cat(parser->output, img_start, sizeof(img_start) - 1);   // <img src="
    if (!NIL_P(parser->img_prefix))
        rb_str_append(parser->output, parser->img_prefix);
    rb_str_cat(parser->output, token_ptr, token_len);
    rb_str_cat(parser->output, img_alt, sizeof(img_alt) - 1);       // " alt="
    rb_str_cat(parser->output, token_ptr, token_len);
    rb_str_cat(parser->output, img_end, sizeof(img_end) - 1);       // " />
}

void _Wikitext_include_parameter(parser_t *parser, char *token_ptr, int token_len)
{
    VALUE val = Qnil;
    if (!NIL_P(parser->parameters)) {
      VALUE key = rb_str_new2(token_ptr);
      val = rb_funcall(parser->parameters, rb_intern("[]"), 1, key);
    }

    if (!NIL_P(val)) {
      val = rb_funcall(val, rb_intern("to_s"), 0);
      rb_str_append(parser->output, val);
      return;
    }

    rb_str_cat(parser->output, literal_parameter_start, sizeof(literal_parameter_start) - 1);
    rb_str_cat(parser->output, token_ptr, token_len);
    rb_str_cat(parser->output, literal_parameter_end, sizeof(literal_parameter_start) - 1);
}

// will emit indentation only if we are about to emit any of:
//      <blockquote>, <p>, <ul>, <ol>, <li>, <h1> etc, <pre>
// each time we enter one of those spans must ++ the indentation level
void _Wikitext_indent(parser_t *parser)
{
    int space_count = parser->current_indent + parser->base_indent;
    if (space_count > 0)
    {
        char *old_end, *new_end;
        if (parser->tabulation->len < space_count)
            str_grow(parser->tabulation, space_count); // reallocates if necessary
        old_end = parser->tabulation->ptr + parser->tabulation->len;
        new_end = parser->tabulation->ptr + space_count;
        while (old_end < new_end)
            *old_end++ = ' ';
        if (space_count > parser->tabulation->len)
            parser->tabulation->len = space_count;
        rb_str_cat(parser->output, parser->tabulation->ptr, space_count);
    }
    parser->current_indent += 2;
}

void _Wikitext_dedent(parser_t *parser, VALUE emit)
{
    parser->current_indent -= 2;
    if (emit != Qtrue)
        return;
    int space_count = parser->current_indent + parser->base_indent;
    if (space_count > 0)
        rb_str_cat(parser->output, parser->tabulation->ptr, space_count);
}

// Pops a single item off the parser's scope stack.
// A corresponding closing tag is written to the target string.
// The target string may be the main output buffer, or a substring capturing buffer if a link is being scanned.
void _Wikitext_pop_from_stack(parser_t *parser, VALUE target)
{
    int top = ary_entry(parser->scope, -1);
    if (NO_ITEM(top))
        return;
    if (NIL_P(target))
        target = parser->output;
    switch (top)
    {
        case PRE:
        case PRE_START:
            rb_str_cat(target, pre_end, sizeof(pre_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case BLOCKQUOTE:
        case BLOCKQUOTE_START:
            _Wikitext_dedent(parser, Qtrue);
            rb_str_cat(target, blockquote_end, sizeof(blockquote_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            break;

        case NO_WIKI_START:
            // not a real HTML tag; so nothing to pop
            break;

        case STRONG:
        case STRONG_START:
            rb_str_cat(target, strong_end, sizeof(strong_end) - 1);
            break;

        case EM:
        case EM_START:
            rb_str_cat(target, em_end, sizeof(em_end) - 1);
            break;

        case TT:
        case TT_START:
            rb_str_cat(target, tt_end, sizeof(tt_end) - 1);
            break;

        case OL:
            _Wikitext_dedent(parser, Qtrue);
            rb_str_cat(target, ol_end, sizeof(ol_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            break;

        case UL:
            _Wikitext_dedent(parser, Qtrue);
            rb_str_cat(target, ul_end, sizeof(ul_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            break;

        case NESTED_LIST:
            // next token to pop will be a LI
            // LI is an interesting token because sometimes we want it to behave like P (ie. do a non-emitting indent)
            // and other times we want it to behave like BLOCKQUOTE (ie. when it has a nested list inside)
            // hence this hack: we do an emitting dedent on behalf of the LI that we know must be coming
            // and then when we pop the actual LI itself (below) we do the standard non-emitting indent
            _Wikitext_dedent(parser, Qtrue);    // we really only want to emit the spaces
            parser->current_indent += 2;        // we don't want to decrement the actual indent level, so put it back
            break;

        case LI:
            rb_str_cat(target, li_end, sizeof(li_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H6_START:
            rb_str_cat(target, h6_end, sizeof(h6_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H5_START:
            rb_str_cat(target, h5_end, sizeof(h5_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H4_START:
            rb_str_cat(target, h4_end, sizeof(h4_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H3_START:
            rb_str_cat(target, h3_end, sizeof(h3_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H2_START:
            rb_str_cat(target, h2_end, sizeof(h2_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case H1_START:
            rb_str_cat(target, h1_end, sizeof(h1_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case LINK_START:
            // not an HTML tag; so nothing to emit
            break;

        case EXT_LINK_START:
            // not an HTML tag; so nothing to emit
            break;

        case SPACE:
            // not an HTML tag (only used to separate an external link target from the link text); so nothing to emit
            break;

        case SEPARATOR:
            // not an HTML tag (only used to separate an external link target from the link text); so nothing to emit
            break;

        case P:
            rb_str_cat(target, p_end, sizeof(p_end) - 1);
            rb_str_cat(target, parser->line_ending->ptr, parser->line_ending->len);
            _Wikitext_dedent(parser, Qfalse);
            break;

        case END_OF_FILE:
            // nothing to do
            break;

        default:
            // should probably raise an exception here
            break;
    }
    ary_pop(parser->scope);
}

// Pops items off the top of parser's scope stack, accumulating closing tags for them into the target string, until item is reached.
// If including is Qtrue then the item itself is also popped.
// The target string may be the main output buffer, or a substring capturing buffer when scanning links.
void _Wikitext_pop_from_stack_up_to(parser_t *parser, VALUE target, int item, VALUE including)
{
    int continue_looping = 1;
    do
    {
        int top = ary_entry(parser->scope, -1);
        if (NO_ITEM(top))
            return;
        if (top == item)
        {
            if (including != Qtrue)
                return;
            continue_looping = 0;
        }
        _Wikitext_pop_from_stack(parser, target);
    } while (continue_looping);
}

void _Wikitext_start_para_if_necessary(parser_t *parser)
{
    if (!NIL_P(parser->capture))    // we don't do anything if in capturing mode
        return;

    // if no block open yet, or top of stack is BLOCKQUOTE/BLOCKQUOTE_START (with nothing in it yet)
    if (parser->scope->count == 0 ||
        ary_entry(parser->scope, -1) == BLOCKQUOTE ||
        ary_entry(parser->scope, -1) == BLOCKQUOTE_START)
    {
        _Wikitext_indent(parser);
        rb_str_cat(parser->output, p_start, sizeof(p_start) - 1);
        ary_push(parser->scope, P);
        ary_push(parser->line, P);
    }
    else if (parser->pending_crlf == Qtrue)
    {
        if (IN(P))
            // already in a paragraph block; convert pending CRLF into a space
            rb_str_cat(parser->output, space, sizeof(space) - 1);
        else if (IN(PRE))
            // PRE blocks can have pending CRLF too (helps us avoid emitting the trailing newline)
            rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
    }
    parser->pending_crlf = Qfalse;
}

void _Wikitext_emit_pending_crlf_if_necessary(parser_t *parser)
{
    if (parser->pending_crlf == Qtrue)
    {
        rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
        parser->pending_crlf = Qfalse;
    }
}

// Helper function that pops any excess elements off scope (pushing is already handled in the respective rules).
// For example, given input like:
//
//      > > foo
//      bar
//
// Upon seeing "bar", we want to pop two BLOCKQUOTE elements from the scope.
// The reverse case (shown below) is handled from inside the BLOCKQUOTE rule itself:
//
//      foo
//      > > bar
//
// Things are made slightly more complicated by the fact that there is one block-level tag that can be on the scope
// but not on the line scope:
//
//      <blockquote>foo
//      bar</blockquote>
//
// Here on seeing "bar" we have one item on the scope (BLOCKQUOTE_START) which we don't want to pop, but we have nothing
// on the line scope.
// Luckily, BLOCKQUOTE_START tokens can only appear at the start of the scope array, so we can check for them first before
// entering the for loop.
void _Wikitext_pop_excess_elements(parser_t *parser)
{
    if (!NIL_P(parser->capture)) // we don't pop anything if in capturing mode
        return;
    for (int i = parser->scope->count - ary_count(parser->scope, BLOCKQUOTE_START), j = parser->line->count; i > j; i--)
    {
        // special case for last item on scope
        if (i - j == 1)
        {
            // don't auto-pop P if it is only item on scope
            if (ary_entry(parser->scope, -1) == P)
            {
                // add P to the line scope to prevent us entering the loop at all next time around
                ary_push(parser->line, P);
                continue;
            }
        }
        _Wikitext_pop_from_stack(parser, parser->output);
    }
}

#define INVALID_ENCODING(msg)  do { if (dest_ptr) free(dest_ptr); rb_raise(eWikitextParserError, "invalid encoding: " msg); } while(0)

// convert a single UTF-8 codepoint to UTF-32
// expects an input buffer, src, containing a UTF-8 encoded character (which may be multi-byte)
// the end of the input buffer, end, is also passed in to allow the detection of invalidly truncated codepoints
// the number of bytes in the UTF-8 character (between 1 and 4) is returned by reference in width_out
// raises a RangeError if the supplied character is invalid UTF-8
// (in which case it also frees the block of memory indicated by dest_ptr if it is non-NULL)
uint32_t _Wikitext_utf8_to_utf32(char *src, char *end, long *width_out, void *dest_ptr)
{
    uint32_t dest;
    if ((unsigned char)src[0] <= 0x7f)                      // ASCII
    {
        dest = src[0];
        *width_out = 1;
    }
    else if ((src[0] & 0xe0) == 0xc0)                       // byte starts with 110..... : this should be a two-byte sequence
    {
        if (src + 1 >= end)
            INVALID_ENCODING("truncated byte sequence");    // no second byte
        else if (((unsigned char)src[0] == 0xc0) || ((unsigned char)src[0] == 0xc1))
            INVALID_ENCODING("overlong encoding");          // overlong encoding: lead byte of 110..... but code point <= 127
        else if ((src[1] & 0xc0) != 0x80 )
            INVALID_ENCODING("malformed byte sequence");    // should have second byte starting with 10......
        dest = ((uint32_t)(src[0] & 0x1f)) << 6 | (src[1] & 0x3f);
        *width_out = 2;
    }
    else if ((src[0] & 0xf0) == 0xe0)                       // byte starts with 1110.... : this should be a three-byte sequence
    {
        if (src + 2 >= end)
            INVALID_ENCODING("truncated byte sequence");    // missing second or third byte
        else if (((src[1] & 0xc0) != 0x80 ) || ((src[2] & 0xc0) != 0x80 ))
            INVALID_ENCODING("malformed byte sequence");    // should have second and third bytes starting with 10......
        dest = ((uint32_t)(src[0] & 0x0f)) << 12 | ((uint32_t)(src[1] & 0x3f)) << 6 | (src[2] & 0x3f);
        *width_out = 3;
    }
    else if ((src[0] & 0xf8) == 0xf0)                       // bytes starts with 11110... : this should be a four-byte sequence
    {
        if (src + 3 >= end)
            INVALID_ENCODING("truncated byte sequence");    // missing second, third, or fourth byte
        else if ((unsigned char)src[0] >= 0xf5 && (unsigned char)src[0] <= 0xf7)
            INVALID_ENCODING("overlong encoding");          // disallowed by RFC 3629 (codepoints above 0x10ffff)
        else if (((src[1] & 0xc0) != 0x80 ) || ((src[2] & 0xc0) != 0x80 ) || ((src[3] & 0xc0) != 0x80 ))
            INVALID_ENCODING("malformed byte sequence");    // should have second and third bytes starting with 10......
        dest = ((uint32_t)(src[0] & 0x07)) << 18 | ((uint32_t)(src[1] & 0x3f)) << 12 | ((uint32_t)(src[1] & 0x3f)) << 6 | (src[2] & 0x3f);
        *width_out = 4;
    }
    else                                                    // invalid input
        INVALID_ENCODING("unexpected byte");
    return dest;
}

VALUE _Wikitext_utf32_char_to_entity(uint32_t character)
{
    // TODO: consider special casing some entities (ie. quot, amp, lt, gt etc)?
    char hex_string[8]  = { '&', '#', 'x', 0, 0, 0, 0, ';' };
    char scratch        = (character & 0xf000) >> 12;
    hex_string[3]       = (scratch <= 9 ? scratch + 48 : scratch + 87);
    scratch             = (character & 0x0f00) >> 8;
    hex_string[4]       = (scratch <= 9 ? scratch + 48 : scratch + 87);
    scratch             = (character & 0x00f0) >> 4;
    hex_string[5]       = (scratch <= 9 ? scratch + 48 : scratch + 87);
    scratch             = character & 0x000f;
    hex_string[6]       = (scratch <= 9 ? scratch + 48 : scratch + 87);
    return rb_str_new((const char *)hex_string, sizeof(hex_string));
}

VALUE _Wikitext_parser_trim_link_target(VALUE string)
{
    string              = StringValue(string);
    char    *src        = RSTRING_PTR(string);
    char    *start      = src;                  // remember this so we can check if we're at the start
    char    *left       = src;
    char    *non_space  = src;                  // remember last non-space character output
    long    len         = RSTRING_LEN(string);
    char    *end        = src + len;
    while (src < end)
    {
        if (*src == ' ')
        {
            if (src == left)
                left++;
        }
        else
            non_space = src;
        src++;
    }
    if (left == start && non_space + 1 == end)
        return string;
    else
        return rb_str_new(left, (non_space + 1) - left);
}

// - non-printable (non-ASCII) characters converted to numeric entities
// - QUOT and AMP characters converted to named entities
// - if rollback is Qtrue, there is no special treatment of spaces
// - if rollback is Qfalse, leading and trailing whitespace trimmed if trimmed
VALUE _Wikitext_parser_sanitize_link_target(parser_t *parser, VALUE rollback)
{
    VALUE string        = StringValue(parser->link_target); // raises if string is nil or doesn't quack like a string
    char    *src        = RSTRING_PTR(string);
    char    *start      = src;                  // remember this so we can check if we're at the start
    long    len         = RSTRING_LEN(string);
    char    *end        = src + len;

    // start with a destination buffer twice the size of the source, will realloc if necessary
    // slop = (len / 8) * 8 (ie. one in every 8 characters can be converted into an entity, each entity requires 8 bytes)
    // this efficiently handles the most common case (where the size of the buffer doesn't change much)
    char    *dest       = ALLOC_N(char, len * 2);
    char    *dest_ptr   = dest; // hang on to this so we can pass it to free() later
    char    *non_space  = dest; // remember last non-space character output
    while (src < end)
    {
        // need at most 8 characters (8 bytes) to display each character
        if (dest + 8 > dest_ptr + len)                      // outgrowing buffer, must reallocate
        {
            char *old_dest      = dest;
            char *old_dest_ptr  = dest_ptr;
            len                 = len + (end - src) * 8;    // allocate enough for worst case
            dest                = realloc(dest_ptr, len);   // will never have to realloc more than once
            if (dest == NULL)
            {
                // would have used reallocf, but this has to run on Linux too, not just Darwin
                free(dest_ptr);
                rb_raise(rb_eNoMemError, "failed to re-allocate temporary storage (memory allocation error)");
            }
            dest_ptr    = dest;
            dest        = dest_ptr + (old_dest - old_dest_ptr);
            non_space   = dest_ptr + (non_space - old_dest_ptr);
        }

        if (*src == '"')                 // QUOT
        {
            char quot_entity_literal[] = { '&', 'q', 'u', 'o', 't', ';' };  // no trailing NUL
            memcpy(dest, quot_entity_literal, sizeof(quot_entity_literal));
            dest += sizeof(quot_entity_literal);
        }
        else if (*src == '&')            // AMP
        {
            char amp_entity_literal[] = { '&', 'a', 'm', 'p', ';' };    // no trailing NUL
            memcpy(dest, amp_entity_literal, sizeof(amp_entity_literal));
            dest += sizeof(amp_entity_literal);
        }
        else if (*src == '<')           // LESS_THAN
        {
            free(dest_ptr);
            rb_raise(rb_eRangeError, "invalid link text (\"<\" may not appear in link text)");
        }
        else if (*src == '>')           // GREATER_THAN
        {
            free(dest_ptr);
            rb_raise(rb_eRangeError, "invalid link text (\">\" may not appear in link text)");
        }
        else if (*src == ' ' && src == start && rollback == Qfalse)
            start++;                // we eat leading space
        else if (*src >= 0x20 && *src <= 0x7e)    // printable ASCII
        {
            *dest = *src;
            dest++;
        }
        else    // all others: must convert to entities
        {
            long        width;
            VALUE       entity      = _Wikitext_utf32_char_to_entity(_Wikitext_utf8_to_utf32(src, end, &width, dest_ptr));
            char        *entity_src = RSTRING_PTR(entity);
            long        entity_len  = RSTRING_LEN(entity); // should always be 8 characters (8 bytes)
            memcpy(dest, entity_src, entity_len);
            dest        += entity_len;
            src         += width;
            non_space   = dest;
            continue;
        }
        if (*src != ' ')
            non_space = dest;
        src++;
    }

    // trim trailing space if necessary
    if (rollback == Qfalse && non_space > dest_ptr && dest != non_space)
        len = non_space - dest_ptr;
    else
        len = dest - dest_ptr;
    VALUE out = rb_str_new(dest_ptr, len);
    free(dest_ptr);
    return out;
}

VALUE Wikitext_parser_sanitize_link_target(VALUE self, VALUE string)
{
    parser_t parser;
    parser.link_target          = string;
    return _Wikitext_parser_sanitize_link_target(&parser, Qfalse);
}

// encodes the input string according to RFCs 2396 and 2718
// leading and trailing whitespace trimmed
// note that the first character of the target link is not case-sensitive
// (this is a recommended application-level constraint; it is not imposed at this level)
// this is to allow links like:
//         ...the [[foo]] is...
// to be equivalent to:
//         thing. [[Foo]] was...
// this is also where we check treat_slash_as_special is true and act accordingly
// basically any link target matching /\A[a-z]+\/\d+\z/ is flagged as special
static void _Wikitext_parser_encode_link_target(parser_t *parser)
{
    VALUE in                = StringValue(parser->link_target);
    char        *input      = RSTRING_PTR(in);
    char        *start      = input;            // remember this so we can check if we're at the start
    long        len         = RSTRING_LEN(in);
    if (!(len > 0))
        return;
    char        *end        = input + len;
    static char hex[]       = { '0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'a', 'b', 'c', 'd', 'e', 'f' };

    // this potential shortcut requires an (admittedly cheap) prescan, so only do it when treat_slash_as_special is true
    parser->special_link = Qfalse;
    if (parser->treat_slash_as_special == Qtrue)
    {
        char *c = input;                                    // \A
        while (c < end && *c >= 'a' && *c <= 'z')           // [a-z]
            c++;                                            // +
        if (c > start && c < end && *c++ == '/')            // \/
        {
            while (c < end && *c >= '0' && *c <= '9')       // \d
            {
                c++;                                        // +
                if (c == end)                               // \z
                {
                    // matches /\A[a-z]+\/\d+\z/ so no transformation required
                    parser->special_link = Qtrue;
                    return;
                }
            }
        }
    }

    // to avoid most reallocations start with a destination buffer twice the size of the source
    // this handles the most common case (where most chars are in the ASCII range and don't require more storage, but there are
    // often quite a few spaces, which are encoded as "%20" and occupy 3 bytes)
    // the worst case is where _every_ byte must be written out using 3 bytes
    long        dest_len    = len * 2;
    char        *dest       = ALLOC_N(char, dest_len);
    char        *dest_ptr   = dest; // hang on to this so we can pass it to free() later
    char        *non_space  = dest; // remember last non-space character output
    for (; input < end; input++)
    {
        if ((dest + 3) > (dest_ptr + dest_len))     // worst case: a single character may grow to 3 characters once encoded
        {
            // outgrowing buffer, must reallocate
            char *old_dest      = dest;
            char *old_dest_ptr  = dest_ptr;
            dest_len            += len;
            dest                = realloc(dest_ptr, dest_len);
            if (dest == NULL)
            {
                // would have used reallocf, but this has to run on Linux too, not just Darwin
                free(dest_ptr);
                rb_raise(rb_eNoMemError, "failed to re-allocate temporary storage (memory allocation error)");
            }
            dest_ptr    = dest;
            dest        = dest_ptr + (old_dest - old_dest_ptr);
            non_space   = dest_ptr + (non_space - old_dest_ptr);
        }

        // pass through unreserved characters
        if (((*input >= 'a') && (*input <= 'z')) ||
            ((*input >= 'A') && (*input <= 'Z')) ||
            ((*input >= '0') && (*input <= '9')) ||
            (*input == '-') ||
            (*input == '_') ||
            (*input == '.') ||
            (*input == '~'))
        {
            *dest++     = *input;
            non_space   = dest;
        }
        else if (*input == ' ' && input == start)
            start++;                    // we eat leading space
        else if (*input == ' ' && parser->space_to_underscore == Qtrue)
            *dest++     = '_';
        else    // everything else gets URL-encoded
        {
            *dest++     = '%';
            *dest++     = hex[(unsigned char)(*input) / 16];   // left
            *dest++     = hex[(unsigned char)(*input) % 16];   // right
            if (*input != ' ')
                non_space = dest;
        }
    }

    // trim trailing space if necessary
    if (non_space > dest_ptr && dest != non_space)
        dest_len = non_space - dest_ptr;
    else
        dest_len = dest - dest_ptr;
    parser->link_target = rb_str_new(dest_ptr, dest_len);
    free(dest_ptr);
}

VALUE Wikitext_parser_encode_link_target(VALUE self, VALUE in)
{
    parser_t parser;
    parser.link_target              = in;
    parser.treat_slash_as_special   = Qfalse;
    parser.space_to_underscore      = Qfalse;
    _Wikitext_parser_encode_link_target(&parser);
    return parser.link_target;
}

// this method exposed for testing only
VALUE Wikitext_parser_encode_special_link_target(VALUE self, VALUE in)
{
    parser_t parser;
    parser.link_target              = in;
    parser.treat_slash_as_special   = Qtrue;
    parser.space_to_underscore      = Qfalse;
    _Wikitext_parser_encode_link_target(&parser);
    return parser.link_target;
}

void _Wikitext_rollback_failed_link(parser_t *parser)
{
    if (!IN(LINK_START))
        return; // nothing to do!
    int scope_includes_separator = IN(SEPARATOR);
    _Wikitext_pop_from_stack_up_to(parser, Qnil, LINK_START, Qtrue);
    rb_str_cat(parser->output, link_start, sizeof(link_start) - 1);
    if (!NIL_P(parser->link_target))
    {
        VALUE sanitized = _Wikitext_parser_sanitize_link_target(parser, Qtrue);
        rb_str_append(parser->output, sanitized);
        if (scope_includes_separator)
        {
            rb_str_cat(parser->output, separator, sizeof(separator) - 1);
            if (!NIL_P(parser->link_text))
                rb_str_append(parser->output, parser->link_text);
        }
    }
    parser->capture     = Qnil;
    parser->link_target = Qnil;
    parser->link_text   = Qnil;
}

void _Wikitext_rollback_failed_external_link(parser_t *parser)
{
    if (!IN(EXT_LINK_START))
        return; // nothing to do!
    int scope_includes_space = IN(SPACE);
    _Wikitext_pop_from_stack_up_to(parser, Qnil, EXT_LINK_START, Qtrue);
    rb_str_cat(parser->output, ext_link_start, sizeof(ext_link_start) - 1);
    if (!NIL_P(parser->link_target))
    {
        if (parser->autolink == Qtrue)
            parser->link_target = _Wikitext_hyperlink(Qnil, parser->link_target, parser->link_target, parser->external_link_class);
        rb_str_append(parser->output, parser->link_target);
        if (scope_includes_space)
        {
            rb_str_cat(parser->output, space, sizeof(space) - 1);
            if (!NIL_P(parser->link_text))
                rb_str_append(parser->output, parser->link_text);
        }
    }
    parser->capture     = Qnil;
    parser->link_target = Qnil;
    parser->link_text   = Qnil;
}

VALUE Wikitext_parser_initialize(int argc, VALUE *argv, VALUE self)
{
    // process arguments
    VALUE options;
    if (rb_scan_args(argc, argv, "01", &options) == 0) // 0 mandatory arguments, 1 optional argument
        options = Qnil;

    // defaults
    VALUE autolink                      = Qtrue;
    VALUE line_ending                   = rb_str_new2("\n");
    VALUE external_link_class           = rb_str_new2("external");
    VALUE mailto_class                  = rb_str_new2("mailto");
    VALUE internal_link_prefix          = rb_str_new2("/wiki/");
    VALUE img_prefix                    = rb_str_new2("/images/");
    VALUE space_to_underscore           = Qfalse;
    VALUE treat_slash_as_special        = Qtrue;
    VALUE minimum_fulltext_token_length = INT2NUM(3);
    VALUE parameters                    = Qnil;

    // process options hash (override defaults)
    if (!NIL_P(options) && TYPE(options) == T_HASH)
    {
#define OVERRIDE_IF_SET(name)   rb_funcall(options, rb_intern("has_key?"), 1, ID2SYM(rb_intern(#name))) == Qtrue ? \
                                rb_hash_aref(options, ID2SYM(rb_intern(#name))) : name
        autolink                        = OVERRIDE_IF_SET(autolink);
        line_ending                     = OVERRIDE_IF_SET(line_ending);
        external_link_class             = OVERRIDE_IF_SET(external_link_class);
        mailto_class                    = OVERRIDE_IF_SET(mailto_class);
        internal_link_prefix            = OVERRIDE_IF_SET(internal_link_prefix);
        img_prefix                      = OVERRIDE_IF_SET(img_prefix);
        space_to_underscore             = OVERRIDE_IF_SET(space_to_underscore);
        treat_slash_as_special          = OVERRIDE_IF_SET(treat_slash_as_special);
        minimum_fulltext_token_length   = OVERRIDE_IF_SET(minimum_fulltext_token_length);
        parameters                      = OVERRIDE_IF_SET(parameters);
    }

    // no need to call super here; rb_call_super()
    rb_iv_set(self, "@autolink",                        autolink);
    rb_iv_set(self, "@line_ending",                     line_ending);
    rb_iv_set(self, "@external_link_class",             external_link_class);
    rb_iv_set(self, "@mailto_class",                    mailto_class);
    rb_iv_set(self, "@internal_link_prefix",            internal_link_prefix);
    rb_iv_set(self, "@img_prefix",                      img_prefix);
    rb_iv_set(self, "@space_to_underscore",             space_to_underscore);
    rb_iv_set(self, "@treat_slash_as_special",          treat_slash_as_special);
    rb_iv_set(self, "@minimum_fulltext_token_length",   minimum_fulltext_token_length);
    rb_iv_set(self, "@parameters",                      parameters);
    return self;
}

VALUE Wikitext_parser_profiling_parse(VALUE self, VALUE string)
{
    for (int i = 0; i < 100000; i++)
        Wikitext_parser_parse(1, &string, self);
    return Qnil;
}

VALUE Wikitext_parser_parse(int argc, VALUE *argv, VALUE self)
{
    // process arguments
    VALUE string, options;
    if (rb_scan_args(argc, argv, "11", &string, &options) == 1) // 1 mandatory argument, 1 optional argument
        options = Qnil;
    if (NIL_P(string))
        return Qnil;
    string = StringValue(string);

    // process options hash
    int base_indent = 0;
    VALUE indent = Qnil;
    if (!NIL_P(options) && TYPE(options) == T_HASH)
    {
        if (rb_funcall(options, rb_intern("has_key?"), 1, ID2SYM(rb_intern("indent"))) == Qtrue)
        {
            indent = rb_hash_aref(options, ID2SYM(rb_intern("indent")));
            base_indent = NUM2INT(indent);
            if (base_indent < 0)
                base_indent = 0;
        }
    }

    // set up scanner
    char *p = RSTRING_PTR(string);
    long len = RSTRING_LEN(string);
    char *pe = p + len;

    // access these once per parse
    VALUE line_ending   = rb_iv_get(self, "@line_ending");
    line_ending         = StringValue(line_ending);
    VALUE link_class    = rb_iv_get(self, "@external_link_class");
    link_class          = NIL_P(link_class) ? Qnil : StringValue(link_class);
    VALUE mailto_class  = rb_iv_get(self, "@mailto_class");
    mailto_class        = NIL_P(mailto_class) ? Qnil : StringValue(mailto_class);
    VALUE prefix        = rb_iv_get(self, "@internal_link_prefix");

    // set up parser struct to make passing parameters a little easier
    // eventually this will encapsulate most or all of the variables above
    parser_t _parser;
    parser_t *parser                = &_parser;
    parser->output                  = rb_str_new2("");
    parser->capture                 = Qnil;
    parser->link_target             = Qnil;
    parser->link_text               = Qnil;
    parser->external_link_class     = link_class;
    parser->img_prefix              = rb_iv_get(self, "@img_prefix");
    parser->scope                   = ary_new();
    GC_WRAP_ARY(parser->scope, scope_gc);
    parser->line                    = ary_new();
    GC_WRAP_ARY(parser->line, line_gc);
    parser->line_buffer             = ary_new();
    GC_WRAP_ARY(parser->line_buffer, line_buffer_gc);
    parser->pending_crlf            = Qfalse;
    parser->autolink                = rb_iv_get(self, "@autolink");
    parser->treat_slash_as_special  = rb_iv_get(self, "@treat_slash_as_special");
    parser->space_to_underscore     = rb_iv_get(self, "@space_to_underscore");
    parser->special_link            = Qfalse;
    parser->line_ending             = str_new_from_string(line_ending);
    GC_WRAP_STR(parser->line_ending, line_ending_gc);
    parser->base_indent             = base_indent;
    parser->current_indent          = 0;
    parser->tabulation              = str_new();
    parser->parameters              = rb_iv_get(self, "@parameters");
    GC_WRAP_STR(parser->tabulation, tabulation_gc);

    token_t _token;
    _token.type = NO_TOKEN;
    token_t *token = NULL;
    do
    {
        // note that whenever we grab a token we push it into the line buffer
        // this provides us with context-sensitive "memory" of what's been seen so far on this line
#define NEXT_TOKEN()    token = &_token, next_token(token, token, NULL, pe), ary_push(parser->line_buffer, token->type)

        // check to see if we have a token hanging around from a previous iteration of this loop
        if (token == NULL)
        {
            if (_token.type == NO_TOKEN)
            {
                // first time here (haven't started scanning yet)
                token = &_token;
                next_token(token, NULL, p, pe);
                ary_push(parser->line_buffer, token->type);
            }
            else
                // already scanning
                NEXT_TOKEN();
        }
        int type = token->type;

        // can't declare new variables inside a switch statement, so predeclare them here
        long remove_strong          = -1;
        long remove_em              = -1;

        // general purpose counters and flags
        long i                      = 0;
        long j                      = 0;
        long k                      = 0;

        // The following giant switch statement contains cases for all the possible token types.
        // In the most basic sense we are emitting the HTML that corresponds to each token,
        // but some tokens require context information in order to decide what to output.
        // For example, does the STRONG token (''') translate to <strong> or </strong>?
        // So when looking at any given token we have three state-maintaining variables which gives us a notion of "where we are":
        //
        //  - the "scope" stack (indicates what HTML DOM structures we are currently nested inside, similar to a CSS selector)
        //  - the line buffer (records tokens seen so far on the current line)
        //  - the line "scope" stack (indicates what the scope should be based only on what is visible on the line so far)
        //
        // Although this is fairly complicated, there is one key simplifying factor:
        // The translator continuously performs auto-correction, and this means that we always have a guarantee that the
        // scope stack (up to the current token) is valid; our translator can take this as a given.
        // Auto-correction basically consists of inserting missing tokens (preventing subsquent HTML from being messed up),
        // or converting illegal (unexpected) tokens to their plain text equivalents (providing visual feedback to Wikitext author).
        switch (type)
        {
            case PRE:
                if (IN(NO_WIKI_START) || IN(PRE_START))
                {
                    rb_str_cat(parser->output, space, sizeof(space) - 1);
                    break;
                }
                else if (IN(BLOCKQUOTE_START))
                {
                    // this kind of nesting not allowed (to avoid user confusion)
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    rb_str_cat(i, space, sizeof(space) - 1);
                    break;
                }

                // count number of BLOCKQUOTE tokens in line buffer and in scope stack
                ary_push(parser->line, PRE);
                i = ary_count(parser->line, BLOCKQUOTE);
                j = ary_count(parser->scope, BLOCKQUOTE);
                if (i < j)
                {
                    // must pop (reduce nesting level)
                    for (i = j - i; i > 0; i--)
                        _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qtrue);
                }

                if (!IN(PRE))
                {
                    parser->pending_crlf = Qfalse;
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qfalse);
                    _Wikitext_indent(parser);
                    rb_str_cat(parser->output, pre_start, sizeof(pre_start) - 1);
                    ary_push(parser->scope, PRE);
                }
                break;

            case PRE_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_pre_start, sizeof(escaped_pre_start) - 1);
                }
                else if (IN(BLOCKQUOTE_START))
                {
                    _Wikitext_rollback_failed_link(parser);             // if any
                    _Wikitext_rollback_failed_external_link(parser);    // if any
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE_START, Qfalse);
                    _Wikitext_indent(parser);
                    rb_str_cat(parser->output, pre_start, sizeof(pre_start) - 1);
                    ary_push(parser->scope, PRE_START);
                    ary_push(parser->line, PRE_START);
                }
                else if (IN(BLOCKQUOTE))
                {
                    // PRE_START is illegal
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(i, escaped_pre_start, sizeof(escaped_pre_start) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_link(parser);             // if any
                    _Wikitext_rollback_failed_external_link(parser);    // if any
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, P, Qtrue);
                    _Wikitext_indent(parser);
                    rb_str_cat(parser->output, pre_start, sizeof(pre_start) - 1);
                    ary_push(parser->scope, PRE_START);
                    ary_push(parser->line, PRE_START);
                }
                break;

            case PRE_END:
                if (IN(NO_WIKI_START) || IN(PRE))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_pre_end, sizeof(escaped_pre_end) - 1);
                }
                else
                {
                    if (IN(PRE_START))
                        _Wikitext_pop_from_stack_up_to(parser, parser->output, PRE_START, Qtrue);
                    else
                    {
                        i = NIL_P(parser->capture) ? parser->output : parser->capture;
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, escaped_pre_end, sizeof(escaped_pre_end) - 1);
                    }
                }
                break;

            case BLOCKQUOTE:
                if (IN(NO_WIKI_START) || IN(PRE_START))
                    // no need to check for <pre>; can never appear inside it
                    rb_str_cat(parser->output, escaped_blockquote, TOKEN_LEN(token) + 3); // will either emit "&gt;" or "&gt; "
                else if (IN(BLOCKQUOTE_START))
                {
                    // this kind of nesting not allowed (to avoid user confusion)
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    rb_str_cat(i, escaped_blockquote, TOKEN_LEN(token) + 3); // will either emit "&gt;" or "&gt; "
                    break;
                }
                else
                {
                    ary_push(parser->line, BLOCKQUOTE);

                    // count number of BLOCKQUOTE tokens in line buffer and in scope stack
                    i = ary_count(parser->line, BLOCKQUOTE);
                    j = ary_count(parser->scope, BLOCKQUOTE);

                    // given that BLOCKQUOTE tokens can be nested, peek ahead and see if there are any more which might affect the decision to push or pop
                    while (NEXT_TOKEN(), (token->type == BLOCKQUOTE))
                    {
                        ary_push(parser->line, BLOCKQUOTE);
                        i++;
                    }

                    // now decide whether to push, pop or do nothing
                    if (i > j)
                    {
                        // must push (increase nesting level)
                        _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qfalse);
                        for (i = i - j; i > 0; i--)
                        {
                            _Wikitext_indent(parser);
                            rb_str_cat(parser->output, blockquote_start, sizeof(blockquote_start) - 1);
                            rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                            ary_push(parser->scope, BLOCKQUOTE);
                        }
                    }
                    else if (i < j)
                    {
                        // must pop (reduce nesting level)
                        for (i = j - i; i > 0; i--)
                            _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qtrue);
                    }

                    // jump to top of the loop to process token we scanned during lookahead
                    continue;
                }
                break;

            case BLOCKQUOTE_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_blockquote_start, sizeof(escaped_blockquote_start) - 1);
                }
                else if (IN(BLOCKQUOTE_START))
                {
                    // nesting is fine here
                    _Wikitext_rollback_failed_link(parser);             // if any
                    _Wikitext_rollback_failed_external_link(parser);    // if any
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE_START, Qfalse);
                    _Wikitext_indent(parser);
                    rb_str_cat(parser->output, blockquote_start, sizeof(blockquote_start) - 1);
                    rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                    ary_push(parser->scope, BLOCKQUOTE_START);
                    ary_push(parser->line, BLOCKQUOTE_START);
                }
                else if (IN(BLOCKQUOTE))
                {
                    // illegal here
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(i, escaped_blockquote_start, sizeof(escaped_blockquote_start) - 1);
                }
                else
                {
                    // would be nice to eliminate the repetition here but it's probably the clearest way
                    _Wikitext_rollback_failed_link(parser);             // if any
                    _Wikitext_rollback_failed_external_link(parser);    // if any
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, P, Qtrue);
                    _Wikitext_indent(parser);
                    rb_str_cat(parser->output, blockquote_start, sizeof(blockquote_start) - 1);
                    rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                    ary_push(parser->scope, BLOCKQUOTE_START);
                    ary_push(parser->line, BLOCKQUOTE_START);
                }
                break;

            case BLOCKQUOTE_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_blockquote_end, sizeof(escaped_blockquote_end) - 1);
                }
                else
                {
                    if (IN(BLOCKQUOTE_START))
                        _Wikitext_pop_from_stack_up_to(parser, parser->output, BLOCKQUOTE_START, Qtrue);
                    else
                    {
                        i = NIL_P(parser->capture) ? parser->output : parser->capture;
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, escaped_blockquote_end, sizeof(escaped_blockquote_end) - 1);
                    }
                }
                break;

            case NO_WIKI_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_no_wiki_start, sizeof(escaped_no_wiki_start) - 1);
                }
                else
                {
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    ary_push(parser->scope, NO_WIKI_START);
                    ary_push(parser->line, NO_WIKI_START);
                }
                break;

            case NO_WIKI_END:
                if (IN(NO_WIKI_START))
                    // <nowiki> should always only ever be the last item in the stack, but use the helper routine just in case
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, NO_WIKI_START, Qtrue);
                else
                {
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_no_wiki_end, sizeof(escaped_no_wiki_end) - 1);
                }
                break;

            case STRONG_EM:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_strong_em, sizeof(literal_strong_em) - 1);
                    break;
                }

                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);

                // if you've seen STRONG/STRONG_START or EM/EM_START, must close them in the reverse order that you saw them!
                // otherwise, must open them
                remove_strong  = -1;
                remove_em      = -1;
                j              = parser->scope->count;
                for (j = j - 1; j >= 0; j--)
                {
                    int val = ary_entry(parser->scope, j);
                    if (val == STRONG || val == STRONG_START)
                    {
                        rb_str_cat(i, strong_end, sizeof(strong_end) - 1);
                        remove_strong = j;
                    }
                    else if (val == EM || val == EM_START)
                    {
                        rb_str_cat(i, em_end, sizeof(em_end) - 1);
                        remove_em = j;
                    }
                }

                if (remove_strong > remove_em)      // must remove strong first
                {
                    ary_pop(parser->scope);
                    if (remove_em > -1)
                        ary_pop(parser->scope);
                    else    // there was no em to remove!, so consider this an opening em tag
                    {
                        rb_str_cat(i, em_start, sizeof(em_start) - 1);
                        ary_push(parser->scope, EM);
                        ary_push(parser->line, EM);
                    }
                }
                else if (remove_em > remove_strong) // must remove em first
                {
                    ary_pop(parser->scope);
                    if (remove_strong > -1)
                        ary_pop(parser->scope);
                    else    // there was no strong to remove!, so consider this an opening strong tag
                    {
                        rb_str_cat(i, strong_start, sizeof(strong_start) - 1);
                        ary_push(parser->scope, STRONG);
                        ary_push(parser->line, STRONG);
                    }
                }
                else    // no strong or em to remove, so this must be a new opening of both
                {
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(i, strong_em_start, sizeof(strong_em_start) - 1);
                    ary_push(parser->scope, STRONG);
                    ary_push(parser->line, STRONG);
                    ary_push(parser->scope, EM);
                    ary_push(parser->line, EM);
                }
                break;

            case STRONG:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_strong, sizeof(literal_strong) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(STRONG_START))
                        // already in span started with <strong>, no choice but to emit this literally
                        rb_str_cat(parser->output, literal_strong, sizeof(literal_strong) - 1);
                    else if (IN(STRONG))
                        // STRONG already seen, this is a closing tag
                        _Wikitext_pop_from_stack_up_to(parser, i, STRONG, Qtrue);
                    else
                    {
                        // this is a new opening
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, strong_start, sizeof(strong_start) - 1);
                        ary_push(parser->scope, STRONG);
                        ary_push(parser->line, STRONG);
                    }
                }
                break;

            case STRONG_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_strong_start, sizeof(escaped_strong_start) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(STRONG_START) || IN(STRONG))
                        rb_str_cat(parser->output, escaped_strong_start, sizeof(escaped_strong_start) - 1);
                    else
                    {
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, strong_start, sizeof(strong_start) - 1);
                        ary_push(parser->scope, STRONG_START);
                        ary_push(parser->line, STRONG_START);
                    }
                }
                break;

            case STRONG_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_strong_end, sizeof(escaped_strong_end) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(STRONG_START))
                        _Wikitext_pop_from_stack_up_to(parser, i, STRONG_START, Qtrue);
                    else
                    {
                        // no STRONG_START in scope, so must interpret the STRONG_END without any special meaning
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, escaped_strong_end, sizeof(escaped_strong_end) - 1);
                    }
                }
                break;

            case EM:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_em, sizeof(literal_em) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(EM_START))
                        // already in span started with <em>, no choice but to emit this literally
                        rb_str_cat(parser->output, literal_em, sizeof(literal_em) - 1);
                    else if (IN(EM))
                        // EM already seen, this is a closing tag
                        _Wikitext_pop_from_stack_up_to(parser, i, EM, Qtrue);
                    else
                    {
                        // this is a new opening
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, em_start, sizeof(em_start) - 1);
                        ary_push(parser->scope, EM);
                        ary_push(parser->line, EM);
                    }
                }
                break;

            case EM_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_em_start, sizeof(escaped_em_start) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(EM_START) || IN(EM))
                        rb_str_cat(i, escaped_em_start, sizeof(escaped_em_start) - 1);
                    else
                    {
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, em_start, sizeof(em_start) - 1);
                        ary_push(parser->scope, EM_START);
                        ary_push(parser->line, EM_START);
                    }
                }
                break;

            case EM_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_em_end, sizeof(escaped_em_end) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(EM_START))
                        _Wikitext_pop_from_stack_up_to(parser, i, EM_START, Qtrue);
                    else
                    {
                        // no EM_START in scope, so must interpret the TT_END without any special meaning
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, escaped_em_end, sizeof(escaped_em_end) - 1);
                    }
                }
                break;

            case TT:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, backtick, sizeof(backtick) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(TT_START))
                        // already in span started with <tt>, no choice but to emit this literally
                        rb_str_cat(parser->output, backtick, sizeof(backtick) - 1);
                    else if (IN(TT))
                        // TT (`) already seen, this is a closing tag
                        _Wikitext_pop_from_stack_up_to(parser, i, TT, Qtrue);
                    else
                    {
                        // this is a new opening
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, tt_start, sizeof(tt_start) - 1);
                        ary_push(parser->scope, TT);
                        ary_push(parser->line, TT);
                    }
                }
                break;

            case TT_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_tt_start, sizeof(escaped_tt_start) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(TT_START) || IN(TT))
                        rb_str_cat(i, escaped_tt_start, sizeof(escaped_tt_start) - 1);
                    else
                    {
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, tt_start, sizeof(tt_start) - 1);
                        ary_push(parser->scope, TT_START);
                        ary_push(parser->line, TT_START);
                    }
                }
                break;

            case TT_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, escaped_tt_end, sizeof(escaped_tt_end) - 1);
                }
                else
                {
                    i = NIL_P(parser->capture) ? parser->output : parser->capture;
                    if (IN(TT_START))
                        _Wikitext_pop_from_stack_up_to(parser, i, TT_START, Qtrue);
                    else
                    {
                        // no TT_START in scope, so must interpret the TT_END without any special meaning
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, escaped_tt_end, sizeof(escaped_tt_end) - 1);
                    }
                }
                break;

            case OL:
            case UL:
                if (IN(NO_WIKI_START) || IN(PRE_START))
                {
                    // no need to check for PRE; can never appear inside it
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                    break;
                }

                // count number of tokens in line and scope stacks
                int bq_count = ary_count(parser->scope, BLOCKQUOTE_START);
                i = parser->line->count - ary_count(parser->line, BLOCKQUOTE_START);
                j = parser->scope->count - bq_count;
                k = i;

                // list tokens can be nested so look ahead for any more which might affect the decision to push or pop
                for (;;)
                {
                    type = token->type;
                    if (type == OL || type == UL)
                    {
                        token = NULL;
                        if (i - k >= 2)                             // already seen at least one OL or UL
                        {
                            ary_push(parser->line, NESTED_LIST);    // which means this is a nested list
                            i += 3;
                        }
                        else
                            i += 2;
                        ary_push(parser->line, type);
                        ary_push(parser->line, LI);

                        // want to compare line with scope but can only do so if scope has enough items on it
                        if (j >= i)
                        {
                            if (ary_entry(parser->scope, i + bq_count - 2) == type && ary_entry(parser->scope, i + bq_count - 1) == LI)
                            {
                                // line and scope match at this point: do nothing yet
                            }
                            else
                            {
                                // item just pushed onto line does not match corresponding slot of scope!
                                for (; j >= i - 2; j--)
                                    // must pop back before emitting
                                    _Wikitext_pop_from_stack(parser, Qnil);

                                // will emit UL or OL, then LI
                                break;
                            }
                        }
                        else        // line stack size now exceeds scope stack size: must increase nesting level
                            break;  // will emit UL or OL, then LI
                    }
                    else
                    {
                        // not a OL or UL token!
                        if (j == i)
                            // must close existing LI and re-open new one
                            _Wikitext_pop_from_stack(parser, Qnil);
                        else if (j > i)
                        {
                            // item just pushed onto line does not match corresponding slot of scope!
                            for (; j >= i; j--)
                                // must pop back before emitting
                                _Wikitext_pop_from_stack(parser, Qnil);
                        }
                        break;
                    }
                    NEXT_TOKEN();
                }

                // will emit
                if (type == OL || type == UL)
                {
                    // if LI is at the top of a stack this is the start of a nested list
                    if (j > 0 && ary_entry(parser->scope, -1) == LI)
                    {
                        // so we should precede it with a CRLF, and indicate that it's a nested list
                        rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                        ary_push(parser->scope, NESTED_LIST);
                    }
                    else
                    {
                        // this is a new list
                        if (IN(BLOCKQUOTE_START))
                            _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE_START, Qfalse);
                        else
                            _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qfalse);
                    }

                    // emit
                    _Wikitext_indent(parser);
                    if (type == OL)
                        rb_str_cat(parser->output, ol_start, sizeof(ol_start) - 1);
                    else if (type == UL)
                        rb_str_cat(parser->output, ul_start, sizeof(ul_start) - 1);
                    ary_push(parser->scope, type);
                    rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                }
                else if (type == SPACE)
                    // silently throw away the optional SPACE token after final list marker
                    token = NULL;

                _Wikitext_indent(parser);
                rb_str_cat(parser->output, li_start, sizeof(li_start) - 1);
                ary_push(parser->scope, LI);

                // any subsequent UL or OL tokens on this line are syntax errors and must be emitted literally
                if (type == OL || type == UL)
                {
                    k = 0;
                    while (k++, NEXT_TOKEN(), (type = token->type))
                    {
                        if (type == OL || type == UL)
                            rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                        else if (type == SPACE && k == 1)
                        {
                            // silently throw away the optional SPACE token after final list marker
                            token = NULL;
                            break;
                        }
                        else
                            break;
                    }
                }

                // jump to top of the loop to process token we scanned during lookahead
                continue;

            case H6_START:
            case H5_START:
            case H4_START:
            case H3_START:
            case H2_START:
            case H1_START:
                if (IN(NO_WIKI_START) || IN(PRE_START))
                {
                    // no need to check for PRE; can never appear inside it
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                    break;
                }

                // pop up to but not including the last BLOCKQUOTE on the scope stack
                if (IN(BLOCKQUOTE_START))
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE_START, Qfalse);
                else
                    _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qfalse);

                // count number of BLOCKQUOTE tokens in line buffer and in scope stack
                ary_push(parser->line, type);
                i = ary_count(parser->line, BLOCKQUOTE);
                j = ary_count(parser->scope, BLOCKQUOTE);

                // decide whether we need to pop off excess BLOCKQUOTE tokens (will never need to push; that is handled above in the BLOCKQUOTE case itself)
                if (i < j)
                {
                    // must pop (reduce nesting level)
                    for (i = j - i; i > 0; i--)
                        _Wikitext_pop_from_stack_up_to(parser, Qnil, BLOCKQUOTE, Qtrue);
                }

                // discard any whitespace here (so that "== foo ==" will be translated to "<h2>foo</h2>" rather than "<h2> foo </h2")
                while (NEXT_TOKEN(), (token->type == SPACE))
                    ; // discard

                ary_push(parser->scope, type);
                _Wikitext_indent(parser);

                // rather than repeat all that code for each kind of heading, share it and use a conditional here
                if (type == H6_START)
                    rb_str_cat(parser->output, h6_start, sizeof(h6_start) - 1);
                else if (type == H5_START)
                    rb_str_cat(parser->output, h5_start, sizeof(h5_start) - 1);
                else if (type == H4_START)
                    rb_str_cat(parser->output, h4_start, sizeof(h4_start) - 1);
                else if (type == H3_START)
                    rb_str_cat(parser->output, h3_start, sizeof(h3_start) - 1);
                else if (type == H2_START)
                    rb_str_cat(parser->output, h2_start, sizeof(h2_start) - 1);
                else if (type == H1_START)
                    rb_str_cat(parser->output, h1_start, sizeof(h1_start) - 1);

                // jump to top of the loop to process token we scanned during lookahead
                continue;

            case H6_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h6, sizeof(literal_h6) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H6_START))
                    {
                        // literal output only if not in h6 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h6, sizeof(literal_h6) - 1);
                    }
                }
                break;

            case H5_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h5, sizeof(literal_h5) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H5_START))
                    {
                        // literal output only if not in h5 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h5, sizeof(literal_h5) - 1);
                    }
                }
                break;

            case H4_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h4, sizeof(literal_h4) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H4_START))
                    {
                        // literal output only if not in h4 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h4, sizeof(literal_h4) - 1);
                    }
                }
                break;

            case H3_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h3, sizeof(literal_h3) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H3_START))
                    {
                        // literal output only if not in h3 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h3, sizeof(literal_h3) - 1);
                    }
                }
                break;

            case H2_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h2, sizeof(literal_h2) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H2_START))
                    {
                        // literal output only if not in h2 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h2, sizeof(literal_h2) - 1);
                    }
                }
                break;

            case H1_END:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, literal_h1, sizeof(literal_h1) - 1);
                }
                else
                {
                    _Wikitext_rollback_failed_external_link(parser); // if any
                    if (!IN(H1_START))
                    {
                        // literal output only if not in h1 scope (we stay silent in that case)
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(parser->output, literal_h1, sizeof(literal_h1) - 1);
                    }
                }
                break;

            case MAIL:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                }
                else
                {
                    // in plain scope, will turn into autolink (with appropriate, user-configurable CSS)
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    i = TOKEN_TEXT(token);
                    if (parser->autolink == Qtrue)
                        i = _Wikitext_hyperlink(rb_str_new2("mailto:"), i, i, mailto_class);
                    rb_str_append(parser->output, i);
                }
                break;

            case URI:
                if (IN(NO_WIKI_START))
                    // user can temporarily suppress autolinking by using <nowiki></nowiki>
                    // note that unlike MediaWiki, we do allow autolinking inside PRE blocks
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                else if (IN(LINK_START))
                {
                    // if the URI were allowed it would have been handled already in LINK_START
                    _Wikitext_rollback_failed_link(parser);
                    i = TOKEN_TEXT(token);
                    if (parser->autolink == Qtrue)
                        i = _Wikitext_hyperlink(Qnil, i, i, parser->external_link_class); // link target, link text
                    rb_str_append(parser->output, i);
                }
                else if (IN(EXT_LINK_START))
                {
                    if (NIL_P(parser->link_target))
                    {
                        // this must be our link target: look ahead to make sure we see the space we're expecting to see
                        i = TOKEN_TEXT(token);
                        NEXT_TOKEN();
                        if (token->type == SPACE)
                        {
                            ary_push(parser->scope, SPACE);
                            parser->link_target = i;
                            parser->link_text   = rb_str_new2("");
                            parser->capture     = parser->link_text;
                            token               = NULL; // silently consume space
                        }
                        else
                        {
                            // didn't see the space! this must be an error
                            _Wikitext_pop_from_stack(parser, Qnil);
                            _Wikitext_pop_excess_elements(parser);
                            _Wikitext_start_para_if_necessary(parser);
                            rb_str_cat(parser->output, ext_link_start, sizeof(ext_link_start) - 1);
                            if (parser->autolink == Qtrue)
                                i = _Wikitext_hyperlink(Qnil, i, i, parser->external_link_class); // link target, link text
                            rb_str_append(parser->output, i);
                        }
                    }
                    else
                    {
                        if (NIL_P(parser->link_text))
                            // this must be the first part of our link text
                            parser->link_text = TOKEN_TEXT(token);
                        else
                            // add to existing link text
                            rb_str_cat(parser->link_text, token->start, TOKEN_LEN(token));
                    }
                }
                else
                {
                    // in plain scope, will turn into autolink (with appropriate, user-configurable CSS)
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    i = TOKEN_TEXT(token);
                    if (parser->autolink == Qtrue)
                        i = _Wikitext_hyperlink(Qnil, i, i, parser->external_link_class); // link target, link text
                    rb_str_append(parser->output, i);
                }
                break;

            // internal links (links to other wiki articles) look like this:
            //      [[another article]] (would point at, for example, "/wiki/another_article")
            //      [[the other article|the link text we'll use for it]]
            //      [[the other article | the link text we'll use for it]]
            // note that the forward slash is a reserved character which changes the meaning of an internal link;
            // this is a link that is external to the wiki but internal to the site as a whole:
            //      [[bug/12]] (a relative link to "/bug/12")
            // MediaWiki has strict requirements about what it will accept as a link target:
            //      all wikitext markup is disallowed:
            //          example [[foo ''bar'' baz]]
            //          renders [[foo <em>bar</em> baz]]        (ie. not a link)
            //          example [[foo <em>bar</em> baz]]
            //          renders [[foo <em>bar</em> baz]]        (ie. not a link)
            //          example [[foo <nowiki>''</nowiki> baz]]
            //          renders [[foo '' baz]]                  (ie. not a link)
            //          example [[foo <bar> baz]]
            //          renders [[foo &lt;bar&gt; baz]]         (ie. not a link)
            //      HTML entities and non-ASCII, however, make it through:
            //          example [[foo &euro;]]
            //          renders <a href="/wiki/Foo_%E2%82%AC">foo &euro;</a>
            //          example [[foo €]]
            //          renders <a href="/wiki/Foo_%E2%82%AC">foo €</a>
            // we'll impose similar restrictions here for the link target; allowed tokens will be:
            //      SPACE, SPECIAL_URI_CHARS, PRINTABLE, ALNUM, DEFAULT, QUOT and AMP
            // everything else will be rejected
            case LINK_START:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(i, link_start, sizeof(link_start) - 1);
                }
                else if (IN(EXT_LINK_START))
                    // already in external link scope! (and in fact, must be capturing link_text right now)
                    rb_str_cat(i, link_start, sizeof(link_start) - 1);
                else if (IN(LINK_START))
                {
                    // already in internal link scope! this is a syntax error
                    _Wikitext_rollback_failed_link(parser);
                    rb_str_cat(parser->output, link_start, sizeof(link_start) - 1);
                }
                else if (IN(SEPARATOR))
                {
                    // scanning internal link text
                }
                else // not in internal link scope yet
                {
                    // will either emit a link, or the rollback of a failed link, so start the para now
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    ary_push(parser->scope, LINK_START);

                    // look ahead and try to gobble up link target
                    while (NEXT_TOKEN(), (type = token->type))
                    {
                        if (type == SPACE               ||
                            type == SPECIAL_URI_CHARS   ||
                            type == PRINTABLE           ||
                            type == ALNUM               ||
                            type == DEFAULT             ||
                            type == QUOT                ||
                            type == QUOT_ENTITY         ||
                            type == AMP                 ||
                            type == AMP_ENTITY          ||
                            type == IMG_START           ||
                            type == IMG_END             ||
                            type == LEFT_CURLY          ||
                            type == RIGHT_CURLY)
                        {
                            // accumulate these tokens into link_target
                            if (NIL_P(parser->link_target))
                            {
                                parser->link_target = rb_str_new2("");
                                parser->capture     = parser->link_target;
                            }
                            if (type == QUOT_ENTITY)
                                // don't insert the entity, insert the literal quote
                                rb_str_cat(parser->link_target, quote, sizeof(quote) - 1);
                            else if (type == AMP_ENTITY)
                                // don't insert the entity, insert the literal ampersand
                                rb_str_cat(parser->link_target, ampersand, sizeof(ampersand) - 1);
                            else
                                rb_str_cat(parser->link_target, token->start, TOKEN_LEN(token));
                        }
                        else if (type == LINK_END)
                            break; // jump back to top of loop (will handle this in LINK_END case below)
                        else if (type == SEPARATOR)
                        {
                            ary_push(parser->scope, SEPARATOR);
                            parser->link_text   = rb_str_new2("");
                            parser->capture     = parser->link_text;
                            token               = NULL;
                            break;
                        }
                        else // unexpected token (syntax error)
                        {
                            _Wikitext_rollback_failed_link(parser);
                            break; // jump back to top of loop to handle unexpected token
                        }
                    }

                    // jump to top of the loop to process token we scanned during lookahead (if any)
                    continue;
                }
                break;

            case LINK_END:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(i, link_end, sizeof(link_end) - 1);
                }
                else if (IN(EXT_LINK_START))
                    // already in external link scope! (and in fact, must be capturing link_text right now)
                    rb_str_cat(i, link_end, sizeof(link_end) - 1);
                else if (IN(LINK_START))
                {
                    // in internal link scope!
                    if (NIL_P(parser->link_text) || RSTRING_LEN(parser->link_text) == 0)
                        // use link target as link text
                        parser->link_text = _Wikitext_parser_sanitize_link_target(parser, Qfalse);
                    else
                        parser->link_text = _Wikitext_parser_trim_link_target(parser->link_text);
                    _Wikitext_parser_encode_link_target(parser);
                    _Wikitext_pop_from_stack_up_to(parser, i, LINK_START, Qtrue);
                    parser->capture     = Qnil;
                    if (parser->special_link)
                        i = _Wikitext_hyperlink(rb_str_new2("/"), parser->link_target, parser->link_text, Qnil);
                    else
                        i = _Wikitext_hyperlink(prefix, parser->link_target, parser->link_text, Qnil);
                    rb_str_append(parser->output, i);
                    parser->link_target = Qnil;
                    parser->link_text   = Qnil;
                }
                else // wasn't in internal link scope
                {
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(i, link_end, sizeof(link_end) - 1);
                }
                break;

            // external links look like this:
            //      [http://google.com/ the link text]
            // strings in square brackets which don't match this syntax get passed through literally; eg:
            //      he was very angery [sic] about the turn of events
            case EXT_LINK_START:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(i, ext_link_start, sizeof(ext_link_start) - 1);
                }
                else if (IN(EXT_LINK_START))
                    // already in external link scope! (and in fact, must be capturing link_text right now)
                    rb_str_cat(i, ext_link_start, sizeof(ext_link_start) - 1);
                else if (IN(LINK_START))
                {
                    // already in internal link scope!
                    i = rb_str_new(ext_link_start, sizeof(ext_link_start) - 1);
                    if (NIL_P(parser->link_target))
                        // this must be the first character of our link target
                        parser->link_target = i;
                    else if (IN(SPACE))
                    {
                        // link target has already been scanned
                        if (NIL_P(parser->link_text))
                            // this must be the first character of our link text
                            parser->link_text = i;
                        else
                            // add to existing link text
                            rb_str_append(parser->link_text, i);
                    }
                    else
                        // add to existing link target
                        rb_str_append(parser->link_target, i);
                }
                else // not in external link scope yet
                {
                    // will either emit a link, or the rollback of a failed link, so start the para now
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);

                    // look ahead: expect a URI
                    NEXT_TOKEN();
                    if (token->type == URI)
                        ary_push(parser->scope, EXT_LINK_START);    // so far so good, jump back to the top of the loop
                    else
                        // only get here if there was a syntax error (missing URI)
                        rb_str_cat(parser->output, ext_link_start, sizeof(ext_link_start) - 1);
                    continue; // jump back to top of loop to handle token (either URI or whatever it is)
                }
                break;

            case EXT_LINK_END:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(i, ext_link_end, sizeof(ext_link_end) - 1);
                }
                else if (IN(EXT_LINK_START))
                {
                    if (NIL_P(parser->link_text))
                        // syntax error: external link with no link text
                        _Wikitext_rollback_failed_external_link(parser);
                    else
                    {
                        // success!
                        _Wikitext_pop_from_stack_up_to(parser, i, EXT_LINK_START, Qtrue);
                        parser->capture = Qnil;
                        i = _Wikitext_hyperlink(Qnil, parser->link_target, parser->link_text, parser->external_link_class);
                        rb_str_append(parser->output, i);
                    }
                    parser->link_target = Qnil;
                    parser->link_text   = Qnil;
                }
                else
                {
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);
                    rb_str_cat(parser->output, ext_link_end, sizeof(ext_link_end) - 1);
                }
                break;

            case SEPARATOR:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, separator, sizeof(separator) - 1);
                break;

            case SPACE:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(i, token->start, TOKEN_LEN(token));
                }
                else
                {
                    // peek ahead to see next token
                    char    *token_ptr  = token->start;
                    int     token_len   = TOKEN_LEN(token);
                    NEXT_TOKEN();
                    type = token->type;
                    if (((type == H6_END) && IN(H6_START)) ||
                        ((type == H5_END) && IN(H5_START)) ||
                        ((type == H4_END) && IN(H4_START)) ||
                        ((type == H3_END) && IN(H3_START)) ||
                        ((type == H2_END) && IN(H2_START)) ||
                        ((type == H1_END) && IN(H1_START)))
                    {
                        // will suppress emission of space (discard) if next token is a H6_END, H5_END etc and we are in the corresponding scope
                    }
                    else
                    {
                        // emit the space
                        _Wikitext_pop_excess_elements(parser);
                        _Wikitext_start_para_if_necessary(parser);
                        rb_str_cat(i, token_ptr, token_len);
                    }

                    // jump to top of the loop to process token we scanned during lookahead
                    continue;
                }
                break;

            case QUOT_ENTITY:
            case AMP_ENTITY:
            case NAMED_ENTITY:
            case DECIMAL_ENTITY:
                // pass these through unaltered as they are case sensitive
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, token->start, TOKEN_LEN(token));
                break;

            case HEX_ENTITY:
                // normalize hex entities (downcase them)
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_append(i, _Wikitext_downcase(TOKEN_TEXT(token)));
                break;

            case QUOT:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, quot_entity, sizeof(quot_entity) - 1);
                break;

            case AMP:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, amp_entity, sizeof(amp_entity) - 1);
                break;

            case LESS:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, lt_entity, sizeof(lt_entity) - 1);
                break;

            case GREATER:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, gt_entity, sizeof(gt_entity) - 1);
                break;

            case IMG_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                }
                else if (!NIL_P(parser->capture))
                    rb_str_cat(parser->capture, token->start, TOKEN_LEN(token));
                else
                {
                    // not currently capturing: will be emitting something on success or failure, so get ready
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);

                    // scan ahead consuming PRINTABLE, ALNUM and SPECIAL_URI_CHARS tokens
                    // will cheat here and abuse the link_target capture buffer to accumulate text
                    if (NIL_P(parser->link_target))
                        parser->link_target = rb_str_new2("");
                    while (NEXT_TOKEN(), (type = token->type))
                    {
                        if (type == PRINTABLE || type == ALNUM || type == SPECIAL_URI_CHARS)
                            rb_str_cat(parser->link_target, token->start, TOKEN_LEN(token));
                        else if (type == IMG_END)
                        {
                            // success
                            _Wikitext_append_img(parser, RSTRING_PTR(parser->link_target), RSTRING_LEN(parser->link_target));
                            token = NULL;
                            break;
                        }
                        else // unexpected token (syntax error)
                        {
                            // rollback
                            rb_str_cat(parser->output, literal_img_start, sizeof(literal_img_start) - 1);
                            rb_str_cat(parser->output, RSTRING_PTR(parser->link_target), RSTRING_LEN(parser->link_target));
                            break;
                        }
                    }

                    // jump to top of the loop to process token we scanned during lookahead
                    parser->link_target = Qnil;
                    continue;
                }
                break;

            case CRLF:
                i = parser->pending_crlf;
                parser->pending_crlf = Qfalse;
                _Wikitext_rollback_failed_link(parser);             // if any
                _Wikitext_rollback_failed_external_link(parser);    // if any
                if (IN(NO_WIKI_START) || IN(PRE_START))
                {
                    ary_clear(parser->line_buffer);
                    rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                    break;
                }
                else if (IN(PRE))
                {
                    // beware when BLOCKQUOTE on line buffer (not line stack!) prior to CRLF, that must be end of PRE block
                    if (ary_entry(parser->line_buffer, -2) == BLOCKQUOTE)
                        // don't emit in this case
                        _Wikitext_pop_from_stack_up_to(parser, parser->output, PRE, Qtrue);
                    else
                    {
                        if (ary_entry(parser->line_buffer, -2) == PRE)
                        {
                             // only thing on line is the PRE: emit pending line ending (if we had one)
                             if (i == Qtrue)
                                 rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);
                        }

                        // clear these _before_ calling NEXT_TOKEN (NEXT_TOKEN adds to the line_buffer)
                        ary_clear(parser->line);
                        ary_clear(parser->line_buffer);

                        // peek ahead to see if this is definitely the end of the PRE block
                        NEXT_TOKEN();
                        type = token->type;
                        if (type != BLOCKQUOTE && type != PRE)
                            // this is definitely the end of the block, so don't emit
                            _Wikitext_pop_from_stack_up_to(parser, parser->output, PRE, Qtrue);
                        else
                            // potentially will emit
                            parser->pending_crlf = Qtrue;

                        continue; // jump back to top of loop to handle token grabbed via lookahead
                    }
                }
                else
                {
                    parser->pending_crlf = Qtrue;

                    // count number of BLOCKQUOTE tokens in line buffer (can be zero) and pop back to that level
                    // as a side effect, this handles any open span-level elements and unclosed blocks
                    // (with special handling for P blocks and LI elements)
                    i = ary_count(parser->line, BLOCKQUOTE) + ary_count(parser->scope, BLOCKQUOTE_START);
                    for (j = parser->scope->count; j > i; j--)
                    {
                        if (parser->scope->count > 0 && ary_entry(parser->scope, -1) == LI)
                        {
                            parser->pending_crlf = Qfalse;
                            break;
                        }

                        // special handling on last iteration through the loop if the top item on the scope is a P block
                        if ((j - i == 1) && ary_entry(parser->scope, -1) == P)
                        {
                            // if nothing or BLOCKQUOTE on line buffer (not line stack!) prior to CRLF, this must be a paragraph break
                            // (note that we have to make sure we're not inside a BLOCKQUOTE_START block
                            // because in those blocks BLOCKQUOTE tokens have no special meaning)
                            if (NO_ITEM(ary_entry(parser->line_buffer, -2)) ||
                                (ary_entry(parser->line_buffer, -2) == BLOCKQUOTE && !IN(BLOCKQUOTE_START)))
                                // paragraph break
                                parser->pending_crlf = Qfalse;
                            else
                                // not a paragraph break!
                                continue;
                        }
                        _Wikitext_pop_from_stack(parser, Qnil);
                    }
                }

                // delete the entire contents of the line scope stack and buffer
                ary_clear(parser->line);
                ary_clear(parser->line_buffer);
                break;

            case SPECIAL_URI_CHARS:
            case PRINTABLE:
            case ALNUM:
            case IMG_END:
            case LEFT_CURLY:
            case RIGHT_CURLY:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_cat(i, token->start, TOKEN_LEN(token));
                break;

            case DEFAULT:
                i = NIL_P(parser->capture) ? parser->output : parser->capture;
                _Wikitext_pop_excess_elements(parser);
                _Wikitext_start_para_if_necessary(parser);
                rb_str_append(i, _Wikitext_utf32_char_to_entity(token->code_point));    // convert to entity
                break;

            case END_OF_FILE:
                // special case for input like " foo\n " (see pre_spec.rb)
                if (IN(PRE) &&
                    ary_entry(parser->line_buffer, -2) == PRE &&
                    parser->pending_crlf == Qtrue)
                    rb_str_cat(parser->output, parser->line_ending->ptr, parser->line_ending->len);

                // close any open scopes on hitting EOF
                _Wikitext_rollback_failed_external_link(parser);    // if any
                _Wikitext_rollback_failed_link(parser);             // if any
                for (i = 0, j = parser->scope->count; i < j; i++)
                    _Wikitext_pop_from_stack(parser, Qnil);
                goto return_output; // break not enough here (want to break out of outer while loop, not inner switch statement)
            case PARAMETER_START:
                if (IN(NO_WIKI_START) || IN(PRE) || IN(PRE_START))
                {
                    _Wikitext_emit_pending_crlf_if_necessary(parser);
                    rb_str_cat(parser->output, token->start, TOKEN_LEN(token));
                }
                else if (!NIL_P(parser->capture))
                    rb_str_cat(parser->capture, token->start, TOKEN_LEN(token));
                else
                {
                    // not currently capturing: will be emitting something on success or failure, so get ready
                    _Wikitext_pop_excess_elements(parser);
                    _Wikitext_start_para_if_necessary(parser);

                    // scan ahead consuming PRINTABLE, ALNUM and SPECIAL_URI_CHARS tokens
                    // will cheat here and abuse the link_target capture buffer to accumulate text
                    if (NIL_P(parser->link_target))
                        parser->link_target = rb_str_new2("");
                    while (NEXT_TOKEN(), (type = token->type))
                    {
                        if (type == PRINTABLE || type == ALNUM || type == SPECIAL_URI_CHARS)
                            rb_str_cat(parser->link_target, token->start, TOKEN_LEN(token));
                        else if (type == PARAMETER_END)
                        {
                            // success
                            _Wikitext_include_parameter(parser, RSTRING_PTR(parser->link_target), RSTRING_LEN(parser->link_target));
                            token = NULL;
                            break;
                        }
                        else // unexpected token (syntax error)
                        {
                            // rollback
                            // rb_str_cat(parser->output, literal_param_start, sizeof(literal_param_start) - 1);
                            rb_str_cat(parser->output, RSTRING_PTR(parser->link_target), RSTRING_LEN(parser->link_target));
                            // rb_str_cat(parser->output, literal_param_end, sizeof(literal_param_end) - 1);
                            break;
                        }
                    }

                    // jump to top of the loop to process token we scanned during lookahead
                    parser->link_target = Qnil;
                    continue;
                }
                break;

            default:
                break;
        }

        // reset current token; forcing lexer to return another token at the top of the loop
        token = NULL;
    } while (1);
return_output:
    return parser->output;
}
