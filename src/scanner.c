#include "tree_sitter/array.h"
#include "tree_sitter/parser.h"

#include <string.h>
#include <wctype.h>

// Mostly a copy paste of tree-sitter-javascript/src/scanner.c

enum TokenType {
  AUTOMATIC_SEMICOLON,
  MULTILINE_COMMENT,
  STRING_START,
  STRING_END,
  STRING_CONTENT,
  PRIMARY_CONSTRUCTOR_KEYWORD,
  IMPORT_DOT,
  INTERPOLATION_EXPRESSION_START,
  INTERPOLATION_IDENTIFIER_START,
  BY_DELEGATION_HINT,
  BACKING_FIELD_HINT,
  ACCESSOR_START,
  ANNOTATION_ARGS_PAREN,
  CONSTRUCTOR_PAREN_HINT,
};

/* Pretty much all of this code is taken from the Julia tree-sitter
   parser.

   Julia has similar problems with multiline comments that can be nested,
   line comments, as well as line and multiline strings.

   The most heavily edited section is `scan_string_content`,
   particularly with respect to interpolation.
 */

// Block comments are easy to parse, but strings require extra-attention.

// The main problems that arise when parsing strings are:
// 1. Triple quoted strings allow single quotes inside. e.g. """ "foo" """.
// 2. Non-standard string literals don't allow interpolations or escape
//    sequences, but you can always write \" and \`.

// To efficiently store a delimiter, we take advantage of the fact that:
// (int)'"' == 34 && (34 & 1) == 0
// i.e. " has an even numeric representation, so we can store a triple
// quoted delimiter as (delimiter + 1).

#define DELIMITER_LENGTH 3

typedef char Delimiter;

// We use a stack to keep track of the string delimiters.
// Each entry is two bytes: [delimiter_byte, prefix_len_byte].
// delimiter_byte: '"' for single-quoted, '"'+1 for triple-quoted.
// prefix_len_byte: number of '$' signs required to trigger interpolation
//   (1 for regular strings and $"...", 2 for $$"...", etc.; max 255).
typedef Array(Delimiter) Stack;

static inline void stack_push(Stack *stack, char chr, bool triple, uint8_t prefix_len) {
  if (stack->size + 1 >= TREE_SITTER_SERIALIZATION_BUFFER_SIZE) abort();
  array_push(stack, (Delimiter)(triple ? (chr + 1) : chr));
  array_push(stack, (Delimiter)prefix_len);
}

static inline void stack_pop(Stack *stack) {
  if (stack->size < 2) abort();
  stack->size -= 2;
}

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

static inline void advance(TSLexer *lexer) { lexer->advance(lexer, false); }

// Scanner functions

static bool scan_string_start(TSLexer *lexer, Stack *stack) {
  // Count leading '$' signs (the interpolation prefix). Capped at 255.
  uint8_t prefix_len = 0;
  while (lexer->lookahead == '$') {
    advance(lexer);
    if (prefix_len < 255) prefix_len++;
  }
  // Regular strings with no prefix still use a single '$' as the trigger.
  if (prefix_len == 0) prefix_len = 1;

  if (lexer->lookahead != '"') return false;
  advance(lexer);
  lexer->mark_end(lexer);
  for (unsigned count = 1; count < DELIMITER_LENGTH; ++count) {
    if (lexer->lookahead != '"') {
      // It's not a triple quoted delimiter.
      stack_push(stack, '"', false, prefix_len);
      return true;
    }
    advance(lexer);
  }
  lexer->mark_end(lexer);
  stack_push(stack, '"', true, prefix_len);
  return true;
}

static bool scan_string_content(TSLexer *lexer, Stack *stack,
                                const bool *valid_symbols) {
  if (stack->size < 2) return false;  // Stack is empty. We're not in a string.
  uint8_t prefix_len = (uint8_t)stack->contents[stack->size - 1];
  Delimiter raw_delim = stack->contents[stack->size - 2];
  bool is_triple = (raw_delim & 1) != 0;
  char end_char = is_triple ? (char)(raw_delim - 1) : (char)raw_delim;
  bool has_content = false;
  while (true) {
    if (lexer->lookahead == '\0') {
      // Stop at real end-of-input, but treat a literal NUL byte (not EOF) as
      // ordinary string content: consume it to guarantee forward progress.
      // Returning here without advancing would leave the lexer stuck at the
      // same offset (mirrors the NUL handling in scan_multiline_comment).
      if (lexer->eof(lexer)) break;
      advance(lexer);
      has_content = true;
      continue;
    }
    if (lexer->lookahead == '$') {
      // If we already have content, stop here so the caller can emit it
      // before we deal with the potential interpolation.
      if (has_content) {
        lexer->result_symbol = STRING_CONTENT;
        return true;
      }
      // Kotlin 2.1 multi-dollar interpolation: in a string with prefix_len N,
      // exactly N consecutive '$' followed by alpha/'{' triggers interpolation.
      // Excess leading '$' signs are literal string content.
      //
      // Strategy: consume the first '$' and mark_end there, then count
      // remaining '$' signs. If total > prefix_len, return STRING_CONTENT
      // for just the first '$' (tree-sitter rewinds to mark_end). On the
      // next scan call, the remaining dollars will be re-examined.
      advance(lexer);
      lexer->mark_end(lexer);
      uint16_t additional_dollars = 0;
      while (lexer->lookahead == '$') {
        advance(lexer);
        additional_dollars++;
      }
      uint16_t total_dollars = 1 + additional_dollars;
      if (total_dollars >= prefix_len &&
          (iswalpha(lexer->lookahead) || lexer->lookahead == '_' || lexer->lookahead == '{')) {
        if (total_dollars > prefix_len) {
          // Excess: emit first '$' as literal STRING_CONTENT.
          // mark_end is after the first '$'; tree-sitter rewinds there.
          lexer->result_symbol = STRING_CONTENT;
          return true;
        }
        // Exact match: emit interpolation start token.
        if (additional_dollars > 0) {
          lexer->mark_end(lexer);
        }
        if (valid_symbols[INTERPOLATION_EXPRESSION_START] &&
            lexer->lookahead == '{') {
          advance(lexer);
          // Empty interpolation "${}" is invalid Kotlin (compile error:
          // "Expecting an expression"). Refuse to emit the interpolation
          // token so the parser produces an ERROR node instead of matching
          // a zero-width expression.
          if (lexer->lookahead == '}') {
            return false;
          }
          lexer->mark_end(lexer);
          lexer->result_symbol = INTERPOLATION_EXPRESSION_START;
          return true;
        }
        if (valid_symbols[INTERPOLATION_IDENTIFIER_START] &&
            (iswalpha(lexer->lookahead) || lexer->lookahead == '_')) {
          lexer->result_symbol = INTERPOLATION_IDENTIFIER_START;
          return true;
        }
        return false;
      }
      // Not enough '$' signs or not followed by alpha/'{':
      // all consumed dollars are literal string content.
      if (additional_dollars > 0) {
        lexer->mark_end(lexer);
      }
      lexer->result_symbol = STRING_CONTENT;
      return true;
    }
    if (lexer->lookahead == '\\') {
      // if we see a \, then this might possibly escape a dollar sign
      // in which case, we should not defer to the interpolation
      advance(lexer);
      // this dollar sign is escaped, so it must be content.
      // we consume it here so we don't enter the dollar sign case above,
      // which leaves the possibility that it is an interpolation 
      if (lexer->lookahead == '$') {
        advance(lexer);
        // however this leaves an edgecase where an escaped dollar sign could
        // appear at the end of a string (e.g "aa\$") which isn't handled
        // correctly; if we were at the end of the string, terminate properly
        if (lexer->lookahead == end_char) {
          stack_pop(stack);
          advance(lexer);
          lexer->mark_end(lexer);
          lexer->result_symbol = STRING_END;
          return true;
        }
      } else if (is_triple && lexer->lookahead == end_char) {
        // In triple-quoted strings, `\` is NOT an escape character. So `\"` is
        // also literal backslash + quote, and the `"` might be the start of
        // the closing `"""`. Don't advance past it (at the end of the while
        // loop). Let the next iteration handle it.
        has_content = true;
        continue;
      }
    } else if (lexer->lookahead == end_char) {
      if (is_triple) {
        lexer->mark_end(lexer);
        for (unsigned count = 1; count < DELIMITER_LENGTH; ++count) {
          advance(lexer);
          if (lexer->lookahead != end_char) {
            lexer->mark_end(lexer);
            lexer->result_symbol = STRING_CONTENT;
            return true;
          }
        }

        /* This is so if we lex something like
           """foo"""
              ^
           where we are at the `f`, we should quit after
           reading `foo`, and ascribe it to STRING_CONTENT.

           Then, we restart and try to read the end.
           This is to prevent `foo` from being absorbed into
           the STRING_END token.
         */
        if (has_content && lexer->lookahead == end_char) {
          lexer->result_symbol = STRING_CONTENT;
          return true;
        }

        /* Since the string internals are all hidden in the syntax
           tree anyways, there's no point in going to the effort of
           specifically separating the string end from string contents.
           If we see a bunch of quotes in a row, then we just go until
           they stop appearing, then stop lexing and call it the
           string's end.
         */
        lexer->result_symbol = STRING_END;
        lexer->mark_end(lexer);
        while (lexer->lookahead == end_char) {
          advance(lexer);
          lexer->mark_end(lexer);
        }
        stack_pop(stack);
        return true;
      }
      if (has_content) {
        lexer->mark_end(lexer);
        lexer->result_symbol = STRING_CONTENT;
        return true;
      }
      stack_pop(stack);
      advance(lexer);
      lexer->mark_end(lexer);
      lexer->result_symbol = STRING_END;
      return true;
    }
    advance(lexer);
    has_content = true;
  }
  return false;
}


static bool scan_multiline_comment(TSLexer *lexer) {
  if (lexer->lookahead != '/') return false;
  advance(lexer);
  if (lexer->lookahead != '*') return false;
  advance(lexer);

  bool after_star = false;
  unsigned nesting_depth = 1;
  for (;;) {
    switch (lexer->lookahead) {
      case '*':
        advance(lexer);
        after_star = true;
        break;
      case '/':
        advance(lexer);
        if (after_star) {
          after_star = false;
          nesting_depth -= 1;
          if (nesting_depth == 0) {
            lexer->result_symbol = MULTILINE_COMMENT;
            lexer->mark_end(lexer);
            return true;
          }
        } else {
          after_star = false;
          if (lexer->lookahead == '*') {
            nesting_depth += 1;
            advance(lexer);
          }
        }
        break;
      case '\0':
        // Accept unterminated block comments at EOF rather than rejecting them.
        // This matches JetBrains PSI behavior which recognizes unclosed /* as a
        // BLOCK_COMMENT token (plus an error element). Without this, the scanner
        // returns false and tree-sitter tries to parse the comment delimiters
        // as operators/expressions.
        if (lexer->eof(lexer)) {
          lexer->result_symbol = MULTILINE_COMMENT;
          lexer->mark_end(lexer);
          return true;
        }
        // A literal NUL byte inside the comment (lookahead is '\0' but not EOF)
        // must be consumed like any other comment content. Returning here without
        // advancing leaves the lexer at the same offset, so tree-sitter re-invokes
        // the scanner forever on inputs such as "/*\0" -> a parse-time hang (DoS).
        // fallthrough
      default:
        advance(lexer);
        after_star = false;
        break;
    }
  }
}

static bool scan_whitespace_and_comments(TSLexer *lexer) {
  while (iswspace(lexer->lookahead)) skip(lexer);
  return true;
}

// Test for any identifier character other than the first character.
// This is meant to match the regexp [\p{L}_\p{Nd}]
// as found in '_alpha_identifier' (see grammar.js).
static bool is_word_char(int32_t c) {
  return (iswalnum(c) || c == '_');
}

// Scan for [the end of] a nonempty alphanumeric identifier or
// alphanumeric keyword (including '_').
static bool scan_for_word(TSLexer *lexer, const char* word, unsigned len) {
    skip(lexer);
    for (unsigned i = 0; i < len; ++i) {
      if (lexer->lookahead != word[i]) return false;
      skip(lexer);
    }
    // check that the identifier stops here
    if (is_word_char(lexer->lookahead)) return false;
    return true;
}

// Check if a sequence of characters matches the given word and is followed
// by a non-word character. Uses skip() so characters are not included in
// the current token.
static bool check_word(TSLexer *lexer, const char *word, unsigned len) {
  for (unsigned i = 0; i < len; i++) {
    if (lexer->lookahead != word[i]) return false;
    skip(lexer);
  }
  return !is_word_char(lexer->lookahead);
}

// Skip whitespace (space, tab, newline, CR) and comments (// and nested /* */)
// using skip() so characters are not included in the current token.
// Returns false if a bare '/' is encountered (not a comment), true otherwise.
static bool skip_whitespace_and_comments(TSLexer *lexer) {
  for (;;) {
    while (iswspace(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead != '/') return true;
    skip(lexer);
    if (lexer->lookahead == '/') {
      // Line comment — skip to end of line
      skip(lexer);
      while (lexer->lookahead != '\n' && lexer->lookahead != '\r' &&
             !lexer->eof(lexer)) {
        skip(lexer);
      }
    } else if (lexer->lookahead == '*') {
      // Block comment — skip to */ (with nesting)
      skip(lexer);
      unsigned depth = 1;
      while (depth > 0 && !lexer->eof(lexer)) {
        if (lexer->lookahead == '*') {
          skip(lexer);
          if (lexer->lookahead == '/') { skip(lexer); depth--; }
        } else if (lexer->lookahead == '/') {
          skip(lexer);
          if (lexer->lookahead == '*') { skip(lexer); depth++; }
        } else {
          skip(lexer);
        }
      }
    } else {
      // Bare '/' — not a comment
      return false;
    }
  }
}

// After scan_for_word has matched "else", peek past optional whitespace
// and comments for "->". If found, this is a when-entry's `else ->`,
// not an if-else. Uses skip() so characters are not included in the
// current token.
static bool followed_by_arrow(TSLexer *lexer) {
  if (!skip_whitespace_and_comments(lexer)) return false;
  if (lexer->lookahead != '-') return false;
  skip(lexer);
  return lexer->lookahead == '>';
}

// Check if the current position has a visibility modifier (public, private,
// protected, internal) followed by whitespace/comments and "constructor".
// Uses skip() — safe to call speculatively since no token boundary is changed.
static bool check_modifier_then_constructor(TSLexer *lexer) {
  // Buffer the first word to identify the modifier
  char word[20];
  unsigned len = 0;
  while (is_word_char(lexer->lookahead) && len < 19) {
    word[len++] = (char)lexer->lookahead;
    skip(lexer);
  }
  word[len] = '\0';

  if (strcmp(word, "public") != 0 && strcmp(word, "private") != 0 &&
      strcmp(word, "protected") != 0 && strcmp(word, "internal") != 0) {
    return false;
  }

  // A constructor may follow the modifier on another line, with comments
  // between them. This is also used after constructor annotations, where
  // comments and newlines are valid between the modifier and constructor.
  if (!skip_whitespace_and_comments(lexer)) return false;

  return check_word(lexer, "constructor", 11);
}

// Look ahead past one or more annotations (e.g. @Bar, @com.example.Bar,
// @Bar(x=1)) and optional visibility modifier, then check for 'constructor'.
// All characters are consumed with skip() so nothing affects token boundaries.
static bool check_annotation_then_constructor(TSLexer *lexer) {
  // Skip one or more '@annotation' sequences
  while (lexer->lookahead == '@') {
    skip(lexer); // skip '@'
    if (!is_word_char(lexer->lookahead)) return false;
    // Read annotation name, including dot-separated qualifiers
    // (e.g. com.example.Inject)
    while (is_word_char(lexer->lookahead)) skip(lexer);
    while (lexer->lookahead == '.') {
      skip(lexer); // skip '.'
      if (!is_word_char(lexer->lookahead)) break;
      while (is_word_char(lexer->lookahead)) skip(lexer);
    }
    // Skip optional '(...)' argument list (handle nested parens and strings)
    if (lexer->lookahead == '(') {
      unsigned depth = 1;
      skip(lexer);
      while (depth > 0 && lexer->lookahead != '\0' && !lexer->eof(lexer)) {
        if (lexer->lookahead == '"') {
          // Skip over string literal to avoid miscounting parens inside strings
          skip(lexer);
          while (lexer->lookahead != '"' && lexer->lookahead != '\0' && !lexer->eof(lexer)) {
            if (lexer->lookahead == '\\') skip(lexer); // skip escaped char
            skip(lexer);
          }
          if (lexer->lookahead == '"') skip(lexer); // skip closing quote
        } else {
          if (lexer->lookahead == '(') depth++;
          else if (lexer->lookahead == ')') depth--;
          skip(lexer);
        }
      }
    }
    // Skip whitespace, newlines, and comments between annotations or before
    // constructor. A trailing comment must not defeat the lookahead
    // (BrokkAi/bifrost-dev#2695). A bare '/' cannot precede 'constructor',
    // so give up if the helper stops at one.
    if (!skip_whitespace_and_comments(lexer)) return false;
  }
  // Allow an optional visibility modifier before 'constructor'
  if (is_word_char(lexer->lookahead) && lexer->lookahead != 'c') {
    return check_modifier_then_constructor(lexer);
  }
  // Check directly for 'constructor'
  return check_word(lexer, "constructor", 11);
}

// Check the part after `field` that distinguishes an explicit backing field
// from an ordinary use of the soft keyword. A type-only backing field is valid
// in KEEP-0430, so `:` is sufficient here; `::` remains a callable reference.
// Uses skip() so characters are not included in the current token.
static bool check_backing_field_shape(TSLexer *lexer) {
  if (!skip_whitespace_and_comments(lexer)) return false;
  if (lexer->lookahead == '=') return true;
  if (lexer->lookahead == ':') {
    skip(lexer);
    return lexer->lookahead != ':';
  }
  return false;
}

// Skip a string literal during lookahead scans: "..." with escapes or raw
// """...""" (no escapes). The lexer is at the opening quote. Returns false
// on EOF before the closing quote.
static bool skip_string_literal(TSLexer *lexer) {
  advance(lexer); // opening '"'
  bool raw = false;
  if (lexer->lookahead == '"') {
    advance(lexer);
    if (lexer->lookahead == '"') {
      raw = true;
      advance(lexer);
    } else {
      return true; // empty string ""
    }
  }
  unsigned quotes = 0;
  for (;;) {
    if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
    if (!raw && lexer->lookahead == '\\') {
      advance(lexer);
      if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
      advance(lexer);
      continue;
    }
    if (lexer->lookahead == '"') {
      advance(lexer);
      if (!raw) return true;
      if (++quotes >= 3) return true;
      continue;
    }
    quotes = 0;
    advance(lexer);
  }
}

// Skip a character literal during lookahead scans ('x', '\n'). The lexer is
// at the opening quote. Returns false on EOF or a newline before the close
// (then it is not a character literal).
static bool skip_char_literal(TSLexer *lexer) {
  advance(lexer); // opening '\''
  for (;;) {
    if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
    if (lexer->lookahead == '\\') {
      advance(lexer);
      if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
      advance(lexer);
      continue;
    }
    if (lexer->lookahead == '\'') {
      advance(lexer);
      return true;
    }
    if (lexer->lookahead == '\n') return false;
    advance(lexer);
  }
}

// After '(', scan its balanced group for either an empty constructor or a
// constructor-parameter colon: a ':' at the outer paren depth and brace depth
// 0 that is neither '::' (callable reference) nor '?:' (elvis), skipping
// strings, character literals, and comments. A class
// header's primary constructor may start on the next line
// (`class Door\n(val width: Int = 3)`, BrokkAi/bifrost-dev#2762), so the
// ASI scanner consults this before inserting a semicolon at a
// statement-initial '('. Pure lookahead: nothing is consumed permanently
// and mark_end is never touched, so callers keep control of token bounds.
static bool check_constructor_param_colon(TSLexer *lexer) {
  advance(lexer); // consume '('
  unsigned paren_depth = 1;
  unsigned brace_depth = 0;
  bool outer_has_content = false;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == '\0' && lexer->eof(lexer)) return false;
    switch (c) {
      case '"':
        if (paren_depth == 1) outer_has_content = true;
        if (!skip_string_literal(lexer)) return false;
        continue;
      case '\'':
        if (paren_depth == 1) outer_has_content = true;
        if (!skip_char_literal(lexer)) return false;
        continue;
      case '/':
        advance(lexer);
        if (lexer->lookahead == '/') {
          while (lexer->lookahead != '\n' && lexer->lookahead != '\r' &&
                 !(lexer->lookahead == '\0' && lexer->eof(lexer))) {
            advance(lexer);
          }
        } else if (lexer->lookahead == '*') {
          advance(lexer);
          unsigned depth = 1;
          bool terminated = false;
          while (!terminated) {
            if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
            if (lexer->lookahead == '*') {
              advance(lexer);
              if (lexer->lookahead == '/') {
                advance(lexer);
                terminated = --depth == 0;
              }
            } else if (lexer->lookahead == '/') {
              advance(lexer);
              if (lexer->lookahead == '*') {
                advance(lexer);
                depth++;
              }
            } else {
              advance(lexer);
            }
          }
        } else if (paren_depth == 1) {
          outer_has_content = true;
        }
        // a bare '/' is just division; nothing special to skip
        continue;
      case '{':
        if (paren_depth == 1) outer_has_content = true;
        brace_depth++;
        advance(lexer);
        continue;
      case '}':
        if (brace_depth > 0) brace_depth--;
        advance(lexer);
        continue;
      case '(':
        if (paren_depth == 1) outer_has_content = true;
        paren_depth++;
        advance(lexer);
        continue;
      case ')':
        advance(lexer);
        if (--paren_depth == 0) return !outer_has_content;
        continue;
      case '?':
        if (paren_depth == 1) outer_has_content = true;
        advance(lexer);
        if (lexer->lookahead == ':') advance(lexer); // elvis: not a param colon
        continue;
      case ':':
        if (paren_depth == 1) outer_has_content = true;
        advance(lexer);
        if (lexer->lookahead == ':') continue; // callable reference
        if (paren_depth == 1 && brace_depth == 0) return true;
        continue;
      default:
        if (paren_depth == 1 && !iswspace(c)) outer_has_content = true;
        advance(lexer);
        continue;
    }
  }
}

static bool is_backing_field_modifier(const char *word) {
  return strcmp(word, "annotation") == 0 ||
         strcmp(word, "sealed") == 0 ||
         strcmp(word, "data") == 0 ||
         strcmp(word, "inner") == 0 ||
         strcmp(word, "value") == 0 ||
         strcmp(word, "override") == 0 ||
         strcmp(word, "lateinit") == 0 ||
         strcmp(word, "public") == 0 ||
         strcmp(word, "private") == 0 ||
         strcmp(word, "internal") == 0 ||
         strcmp(word, "protected") == 0 ||
         strcmp(word, "tailrec") == 0 ||
         strcmp(word, "operator") == 0 ||
         strcmp(word, "infix") == 0 ||
         strcmp(word, "inline") == 0 ||
         strcmp(word, "external") == 0 ||
         strcmp(word, "const") == 0 ||
         strcmp(word, "abstract") == 0 ||
         strcmp(word, "final") == 0 ||
         strcmp(word, "open") == 0 ||
         strcmp(word, "vararg") == 0 ||
         strcmp(word, "noinline") == 0 ||
         strcmp(word, "crossinline") == 0 ||
         strcmp(word, "suspend") == 0 ||
         strcmp(word, "expect") == 0 ||
         strcmp(word, "actual") == 0;
}

// Scan an annotation-shaped modifier. This is deliberately structural rather
// than semantic: the grammar will validate the annotation, while the scanner
// only needs to know whether a following `field` could still belong to this
// property. It handles qualified names and constructor arguments, including
// nested parentheses and quoted strings.
static bool scan_backing_field_annotation(TSLexer *lexer) {
  if (lexer->lookahead != '@') return false;
  skip(lexer);

  // Optional use-site target, e.g. `@field:Ann`.
  if (!is_word_char(lexer->lookahead)) return false;
  while (is_word_char(lexer->lookahead)) skip(lexer);
  if (lexer->lookahead == ':') {
    skip(lexer);
    if (!is_word_char(lexer->lookahead)) return false;
    while (is_word_char(lexer->lookahead)) skip(lexer);
  }

  while (lexer->lookahead == '.') {
    skip(lexer);
    if (!is_word_char(lexer->lookahead)) return false;
    while (is_word_char(lexer->lookahead)) skip(lexer);
  }

  if (lexer->lookahead == '(') {
    unsigned depth = 1;
    skip(lexer);
    while (depth > 0 && !lexer->eof(lexer)) {
      if (lexer->lookahead == '"') {
        skip(lexer);
        while (lexer->lookahead != '"' && !lexer->eof(lexer)) {
          if (lexer->lookahead == '\\') skip(lexer);
          skip(lexer);
        }
        if (lexer->lookahead == '"') skip(lexer);
      } else {
        if (lexer->lookahead == '(') depth++;
        else if (lexer->lookahead == ')') depth--;
        skip(lexer);
      }
    }
    if (depth != 0) return false;
  }
  return true;
}

typedef enum {
  BACKING_FIELD_NOT_MATCHED,
  BACKING_FIELD_MATCHED,
  BACKING_FIELD_FINALLY,
  BACKING_FIELD_ELSE,
  BACKING_FIELD_AS,
  BACKING_FIELD_WHERE,
  BACKING_FIELD_BY,
  BACKING_FIELD_CATCH,
  BACKING_FIELD_CONSTRUCTOR,
} BackingFieldLookahead;

// Check `field` with zero or more valid backing-field modifiers before it.
// Kotlin's parser parses the modifier list before the FIELD component; this
// lookahead keeps ASI from splitting that list away from the preceding
// property. Comments and newlines are allowed between modifiers. The caller
// must handle the returned kind immediately: this function consumes its
// lookahead while checking it and the external scanner cannot rewind to the
// old switch position.
static BackingFieldLookahead scan_backing_field_lookahead(TSLexer *lexer) {
  bool saw_modifier = false;
  for (;;) {
    if (lexer->lookahead == '@') {
      if (!scan_backing_field_annotation(lexer)) {
        return BACKING_FIELD_NOT_MATCHED;
      }
      saw_modifier = true;
    } else if (is_word_char(lexer->lookahead)) {
      char word[32];
      unsigned len = 0;
      while (is_word_char(lexer->lookahead)) {
        if (len + 1 < sizeof(word)) word[len++] = (char)lexer->lookahead;
        skip(lexer);
      }
      word[len] = '\0';
      if (strcmp(word, "field") == 0) {
        return check_backing_field_shape(lexer)
          ? BACKING_FIELD_MATCHED
          : BACKING_FIELD_NOT_MATCHED;
      }
      if (!is_backing_field_modifier(word)) {
        // These continuations are only relevant when no modifier preceded
        // them. Once a modifier has been consumed, the grammar cannot use
        // them as a backing-field component and ASI should be inserted.
        if (!saw_modifier && strcmp(word, "finally") == 0) {
          return BACKING_FIELD_FINALLY;
        }
        if (!saw_modifier && strcmp(word, "else") == 0) {
          return BACKING_FIELD_ELSE;
        }
        if (!saw_modifier && strcmp(word, "as") == 0) {
          return BACKING_FIELD_AS;
        }
        if (!saw_modifier && strcmp(word, "where") == 0) {
          return BACKING_FIELD_WHERE;
        }
        if (!saw_modifier && strcmp(word, "by") == 0) {
          return BACKING_FIELD_BY;
        }
        if (!saw_modifier && strcmp(word, "catch") == 0) {
          return BACKING_FIELD_CATCH;
        }
        if (strcmp(word, "constructor") == 0) {
          return BACKING_FIELD_CONSTRUCTOR;
        }
        return BACKING_FIELD_NOT_MATCHED;
      }
      saw_modifier = true;
    } else {
      return BACKING_FIELD_NOT_MATCHED;
    }

    if (!skip_whitespace_and_comments(lexer)) {
      return BACKING_FIELD_NOT_MATCHED;
    }
  }
}

static bool scan_automatic_semicolon(TSLexer *lexer, const bool *valid_symbols) {
  lexer->result_symbol = AUTOMATIC_SEMICOLON;
  lexer->mark_end(lexer);

  bool sameline = true;
  for (;;) {
    if (lexer->eof(lexer)) return true;

    if (lexer->lookahead == ';') {
      advance(lexer);
      lexer->mark_end(lexer);
      return true;
    }

    if (!iswspace(lexer->lookahead)) break;

    if (lexer->lookahead == '\n') {
      skip(lexer);
      sameline = false;
      break;
    }

    if (lexer->lookahead == '\r') {
      skip(lexer);

      if (lexer->lookahead == '\n') skip(lexer);

      sameline = false;
      break;
    }

    skip(lexer);
  }

  // Skip whitespace and comments
  if (!scan_whitespace_and_comments(lexer))
    return false;

  if (sameline) {
    switch (lexer->lookahead) {
      // Insert imaginary semicolon before an 'import' but not in front
      // of other words or keywords starting with 'i'
      case 'i':
        return scan_for_word(lexer, "mport", 5);

      case ';':
        advance(lexer);
        lexer->mark_end(lexer);
        return true;

      // Don't insert a semicolon in other cases
      default:
        return false;
    }
  }

  if (valid_symbols[BACKING_FIELD_HINT] &&
      !valid_symbols[STRING_CONTENT] &&
      (is_word_char(lexer->lookahead) || lexer->lookahead == '@')) {
    BackingFieldLookahead lookahead = scan_backing_field_lookahead(lexer);
    switch (lookahead) {
      case BACKING_FIELD_MATCHED:
      case BACKING_FIELD_FINALLY:
        return false;
      case BACKING_FIELD_ELSE:
        return followed_by_arrow(lexer);
      case BACKING_FIELD_AS:
      case BACKING_FIELD_WHERE:
      case BACKING_FIELD_CATCH:
        return false;
      case BACKING_FIELD_BY:
        return !(valid_symbols[BY_DELEGATION_HINT]);
      case BACKING_FIELD_CONSTRUCTOR:
        return !(valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD]);
      case BACKING_FIELD_NOT_MATCHED:
        // The classifier consumed an ordinary word or an invalid modifier
        // sequence. Return the normal ASI token at the original mark_end.
        return true;
    }
  }

  switch (lexer->lookahead) {
      case ',':
      case '.':
      case ':':
      case '*':
      case '%':
      case '>':
      case '<':
      case '=':
      case '{':
      case '[':
      case '?':
      case '|':
      case '&':
        return false;

      // Insert a semicolon before '(': Kotlin requires a call's argument
      // list on the callee's line, so a newline before '(' normally ends
      // the statement (BrokkAi/bifrost-dev#2710). Exception: after a class
      // header, a paren group with constructor-parameter shape (a typed
      // parameter's ':') continues the primary constructor on the next
      // line — `class Door\n(val width: Int = 3)` — so no semicolon there
      // (BrokkAi/bifrost-dev#2762).
      case '(':
        if (valid_symbols[CONSTRUCTOR_PAREN_HINT] &&
            !valid_symbols[STRING_CONTENT] &&
            check_constructor_param_colon(lexer)) {
          return false;
        }
        return true;

      // Handle `/` — could be division, line comment, or block comment.
      // For division: no ASI (continuation operator).
      // For line comments (`//`): skip the comment(s) and check the next
      // real token. If continuation, suppress ASI (return false — tree-sitter
      // resets, parses line_comment internally, then re-checks ASI).
      // If non-continuation, insert ASI (return true at original mark_end).
      // For block comments (`/*`): advance through the comment and produce
      // MULTILINE_COMMENT. The parser then re-calls the scanner for the ASI
      // decision on whatever token follows the comment.
      case '/': {
        advance(lexer);
        if (lexer->lookahead == '/') {
          // Line comment — skip to end of line using skip() since
          // line_comment is an internal token (the grammar handles it).
          skip(lexer);
          while (lexer->lookahead != '\n' && lexer->lookahead != '\r' &&
                 lexer->lookahead != 0 && !lexer->eof(lexer)) {
            skip(lexer);
          }
          // Skip any whitespace and further comments after this line comment.
          // A bare '/' (division) after comments is a continuation operator.
          if (!skip_whitespace_and_comments(lexer)) return false;
          if (valid_symbols[BACKING_FIELD_HINT] &&
              !valid_symbols[STRING_CONTENT] &&
              (is_word_char(lexer->lookahead) || lexer->lookahead == '@')) {
            BackingFieldLookahead lookahead = scan_backing_field_lookahead(lexer);
            switch (lookahead) {
              case BACKING_FIELD_MATCHED:
              case BACKING_FIELD_FINALLY:
                return false;
              case BACKING_FIELD_ELSE:
                return followed_by_arrow(lexer);
              case BACKING_FIELD_AS:
              case BACKING_FIELD_WHERE:
              case BACKING_FIELD_CATCH:
                return false;
              case BACKING_FIELD_BY:
                return !(valid_symbols[BY_DELEGATION_HINT]);
              case BACKING_FIELD_CONSTRUCTOR:
                return !(valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD]);
              case BACKING_FIELD_NOT_MATCHED:
                return true;
            }
          }
          // Now check the next real token.
          switch (lexer->lookahead) {
            case '.': case ',': case ':': case '*': case '%':
            case '>': case '<': case '=': case '{': case '[':
            case '?': case '|': case '&': case '/':
              return false;
            case '(':
              // Statement-initial paren: insert ASI (call parens must be on
              // the callee's line), except after a class header where a
              // constructor-shaped group continues the primary constructor.
              // See the main switch below.
              if (valid_symbols[CONSTRUCTOR_PAREN_HINT] &&
                  !valid_symbols[STRING_CONTENT] &&
                  check_constructor_param_colon(lexer)) {
                return false;
              }
              return true;
            case '!':
              skip(lexer);
              if (lexer->lookahead == '=') return false;
              return true;
            case 'e':
              if (scan_for_word(lexer, "lse", 3)) {
                if (followed_by_arrow(lexer)) return true;
                return false;
              }
              return true;
            case 'a':
              if (scan_for_word(lexer, "s", 1)) return false;
              return true;
            case 'w':
              if (scan_for_word(lexer, "here", 4)) return false;
              return true;
            case 'c':
              if (scan_for_word(lexer, "atch", 4)) return false;
              return true;
            case 'b':
              if (valid_symbols[BY_DELEGATION_HINT] &&
                  scan_for_word(lexer, "y", 1)) return false;
              return true;
            case 'f':
              skip(lexer); // consume 'f'
              if (lexer->lookahead == 'i') {
                skip(lexer); // consume 'i'
                if (lexer->lookahead == 'e' &&
                    valid_symbols[BACKING_FIELD_HINT] &&
                    !valid_symbols[STRING_CONTENT] &&
                    check_backing_field_shape(lexer)) {
                  // Explicit backing field after the comment — no ASI.
                  return false;
                }
                // "fi" consumed; "finally" leaves "nally" to match.
                return !check_word(lexer, "nally", 5);
              }
              return true;
            default:
              return true;
          }
        } else if (lexer->lookahead == '*') {
          // Block comment after a newline. Use advance() to read through the
          // comment so the content is available for MULTILINE_COMMENT if we
          // decide to produce it. DON'T call mark_end yet — we defer that
          // decision until we know what follows the comment.
          advance(lexer);
          unsigned nesting_depth = 1;
          bool after_star = false;
          while (nesting_depth > 0 && !lexer->eof(lexer)) {
            switch (lexer->lookahead) {
              case '*':
                advance(lexer);
                after_star = true;
                break;
              case '/':
                advance(lexer);
                if (after_star) {
                  after_star = false;
                  nesting_depth--;
                } else {
                  if (lexer->lookahead == '*') {
                    nesting_depth++;
                    advance(lexer);
                  }
                  after_star = false;
                }
                break;
              case '\0':
                if (lexer->eof(lexer)) {
                  // Unterminated block comment at EOF — produce it.
                  lexer->result_symbol = MULTILINE_COMMENT;
                  lexer->mark_end(lexer);
                  return true;
                }
                // fallthrough
              default:
                advance(lexer);
                after_star = false;
                break;
            }
          }
          // Skip whitespace after the block comment. Don't skip further
          // comments — the continuation switch handles '/' and '*', so
          // subsequent comments will be correctly treated as continuation.
          // Skipping them here would swallow them (they'd never appear
          // as separate tokens in the parse tree).
          while (iswspace(lexer->lookahead)) skip(lexer);
          // Only backing-field modifier starts need the extra lookahead here.
          // For unrelated declarations (for example `val`), leave mark_end
          // at P0 so the ordinary ASI remains zero-width before the comment.
          bool possible_backing_modifier =
            lexer->lookahead == 'p' || lexer->lookahead == 'i' ||
            lexer->lookahead == 'l';
          if (valid_symbols[BACKING_FIELD_HINT] &&
              !valid_symbols[STRING_CONTENT] && possible_backing_modifier) {
            lexer->mark_end(lexer);
            BackingFieldLookahead lookahead = scan_backing_field_lookahead(lexer);
            switch (lookahead) {
              case BACKING_FIELD_MATCHED:
              case BACKING_FIELD_FINALLY:
              case BACKING_FIELD_AS:
              case BACKING_FIELD_WHERE:
              case BACKING_FIELD_CATCH:
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              case BACKING_FIELD_ELSE:
                if (followed_by_arrow(lexer)) return true;
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              case BACKING_FIELD_BY:
                if (valid_symbols[BY_DELEGATION_HINT]) {
                  lexer->result_symbol = MULTILINE_COMMENT;
                  return true;
                }
                return true;
              case BACKING_FIELD_CONSTRUCTOR:
                if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD]) {
                  lexer->result_symbol = MULTILINE_COMMENT;
                  return true;
                }
                return true;
              case BACKING_FIELD_NOT_MATCHED:
                return true;
            }
          }
          // Check the next real token to decide: MULTILINE_COMMENT or ASI?
          //
          // IMPORTANT: For keyword checks (else, as, where, !=), we must
          // call mark_end BEFORE scan_for_word/skip, because those functions
          // advance the cursor past the keyword. If mark_end were called
          // after, the MULTILINE_COMMENT span would swallow the keyword
          // and the parser would never see it.
          switch (lexer->lookahead) {
            case '.': case ',': case ':': case '%':
            case '>': case '<': case '=': case '{': case '[':
            case '?': case '|': case '&': case '/':
            case '*':
              // Continuation operator — produce MULTILINE_COMMENT.
              lexer->mark_end(lexer);
              lexer->result_symbol = MULTILINE_COMMENT;
              return true;
            case '(':
              // Statement-initial paren: not a continuation — produce ASI at
              // the original position (call parens must be on the callee's
              // line); the block comment is re-scanned as MULTILINE_COMMENT
              // on the next parse step, like the default case. Exception:
              // after a class header, a constructor-shaped group continues
              // the primary constructor — emit the comment now (its span is
              // already consumed) and let the main switch decide ASI at the
              // '(' on the next scan. mark_end only moves in the hint-gated
              // branch, so the re-scan behavior elsewhere is unchanged.
              if (valid_symbols[CONSTRUCTOR_PAREN_HINT] &&
                  !valid_symbols[STRING_CONTENT]) {
                lexer->mark_end(lexer);
                if (check_constructor_param_colon(lexer)) {
                  lexer->result_symbol = MULTILINE_COMMENT;
                  return true;
                }
              }
              return true;
            case '!':
              // mark_end before consuming '!' so it's not swallowed.
              lexer->mark_end(lexer);
              skip(lexer);
              if (lexer->lookahead == '=') {
                // != is continuation — produce MULTILINE_COMMENT.
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              }
              // Unary ! — not continuation. Produce ASI at original
              // position (mark_end was at P0 before, now at '!' position,
              // but the token has no advance()d content past the comment,
              // so tree-sitter will re-scan from here).
              return true;
            case 'e':
              lexer->mark_end(lexer);
              if (scan_for_word(lexer, "lse", 3)) {
                if (followed_by_arrow(lexer)) return true;
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              }
              return true;
            case 'a':
              lexer->mark_end(lexer);
              if (scan_for_word(lexer, "s", 1)) {
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              }
              return true;
            case 'w':
              lexer->mark_end(lexer);
              if (scan_for_word(lexer, "here", 4)) {
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              }
              return true;
            case 'b':
              if (valid_symbols[BY_DELEGATION_HINT]) {
                lexer->mark_end(lexer);
                if (scan_for_word(lexer, "y", 1)) {
                  lexer->result_symbol = MULTILINE_COMMENT;
                  return true;
                }
              }
              return true;
            case 'f':
              // `field` was handled above with its full shape check. Keep
              // comments attached when this is the `finally` continuation.
              lexer->mark_end(lexer);
              if (scan_for_word(lexer, "inally", 6)) {
                lexer->result_symbol = MULTILINE_COMMENT;
                return true;
              }
              return true;
            default:
              // the original position (P0, before the comment), so the
              // ASI token is zero-width. The block comment will be
              // re-scanned as MULTILINE_COMMENT on the next parse step.
              return true;
          }
        }
        // Bare `/` (not `//` or `/*`) — division. No ASI.
        return false;
      }

      // In Kotlin, `+` and `-` after a newline are always prefix operators,
      // not binary continuation. If a binary operation is intended, the
      // operator must be placed at the end of the previous line:
      //   a +       // binary: a + b
      //     b
      //   a         // prefix: a; +b
      //   + b
      // The grammar ensures AUTOMATIC_SEMICOLON is only valid where a
      // statement could end, so this won't fire inside () or [] where
      // newlines don't terminate statements.
      case '+':
      case '-':
        return true;

      // Don't insert a semicolon before `!=`, but do insert one before a unary `!`.
      case '!':
        skip(lexer);
        return lexer->lookahead != '=';

      // Don't insert a semicolon before 'by' in delegation contexts.
      // Gated on BY_DELEGATION_HINT so `by` remains a usable soft-keyword
      // identifier in non-delegation positions.
      case 'b':
        return !(valid_symbols[BY_DELEGATION_HINT] &&
                 scan_for_word(lexer, "y", 1));

      // Don't insert a semicolon before an else, unless it's
      // followed by "->" (a when-entry's else, not an if-else).
      case 'e':
        if (!scan_for_word(lexer, "lse", 3)) return true;
        return followed_by_arrow(lexer);

      // Don't insert a semicolon before an as
      case 'a':
        return !scan_for_word(lexer, "s", 1);

      // Don't insert a semicolon before a where
      case 'w':
        return !scan_for_word(lexer, "here", 4);

      // Don't insert a semicolon before `instanceof`, or before `internal`
      // when followed by `constructor` in a class declaration context.
      case 'i':
        if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD] &&
            !valid_symbols[STRING_CONTENT] &&
            check_modifier_then_constructor(lexer)) {
          return false;
        }
        // Note: lexer has advanced past the word. For "instanceof", scan_for_word
        // can no longer match. But since "instanceof" is not a Kotlin keyword
        // (Kotlin uses "is"), this is acceptable — ASI is inserted, which is
        // the correct behavior for any non-constructor identifier.
        return true;

      // Don't insert a semicolon before `public/private/protected constructor`
      // in class declaration context.
      case 'p':
        if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD] &&
            !valid_symbols[STRING_CONTENT] &&
            check_modifier_then_constructor(lexer)) {
          return false;
        }
        return true;

      // Don't insert a semicolon before `constructor` if the parser expects
      // a primary constructor (class declaration context). In class body
      // context, PRIMARY_CONSTRUCTOR_KEYWORD won't be valid, so ASI is
      // inserted normally before secondary constructors.
      // Guard against error recovery mode where all symbols are valid.
      // Instead of suppressing ASI, we emit the constructor keyword directly
      // since it's an external token and the internal lexer won't match it.
      case 'c':
        if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD] &&
            !valid_symbols[STRING_CONTENT]) {
          const char *kw = "constructor";
          bool matched = true;
          for (unsigned i = 0; i < 11; i++) {
            if (lexer->lookahead != kw[i]) { matched = false; break; }
            advance(lexer);
          }
          if (matched && !is_word_char(lexer->lookahead)) {
            lexer->result_symbol = PRIMARY_CONSTRUCTOR_KEYWORD;
            lexer->mark_end(lexer);
            return true;
          }
          // If constructor didn't match, we've advanced past some chars.
          // Can't reliably check 'catch' now. Just insert ASI.
          return true;
        }
        // Not in constructor context — check for 'catch'
        return !scan_for_word(lexer, "atch", 4);

      // Don't insert a semicolon before finally (continues try_expression),
      // and don't insert one before `field =` / `field: T =` when the parser
      // is in a position where an explicit backing field (KEEP-0430) can
      // follow a property declaration. The hint check comes first because
      // `field` and `finally` share the "fi" prefix; a failed backing-field
      // check still falls through to the "finally" check on the remaining
      // characters, which can only fail for words that are not "finally".
      case 'f':
        skip(lexer); // consume 'f'
        if (lexer->lookahead != 'i') return true;
        skip(lexer); // consume 'i'
        if (lexer->lookahead == 'e' &&
            valid_symbols[BACKING_FIELD_HINT] &&
            !valid_symbols[STRING_CONTENT] &&
            check_backing_field_shape(lexer)) {
          return false;
        }
        // "fi" consumed; "finally" leaves "nally" to match. Any other word
        // (including a `field` without the backing-field shape) fails here
        // and gets a semicolon.
        return !check_word(lexer, "nally", 5);

      // Don't insert a semicolon before an annotation that precedes 'constructor'
      // e.g. `class Foo\n@Bar\nconstructor(...)` — the @Bar is a constructor modifier
      case '@':
        if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD] &&
            !valid_symbols[STRING_CONTENT] &&
            check_annotation_then_constructor(lexer)) {
          return false;
        }
        return true;

      case ';':
        advance(lexer);
        lexer->mark_end(lexer);
        return true;

      default:
        return true;
  }
}


// Scan a dot in import identifiers. Matches '.' normally, but when the dot
// is followed by a newline and then the 'import' keyword, produces an
// AUTOMATIC_SEMICOLON (zero-width, before the dot) instead. This cleanly
// terminates the current import_header, preventing malformed imports
// (e.g. trailing dots) from bleeding into subsequent valid imports.
static bool scan_import_dot(TSLexer *lexer) {
  if (lexer->lookahead != '.') return false;

  // Mark end BEFORE consuming the dot — this is where ASI would go
  lexer->mark_end(lexer);

  advance(lexer);

  // Peek ahead: skip horizontal whitespace, check for newline
  bool found_newline = false;
  while (iswspace(lexer->lookahead)) {
    if (lexer->lookahead == '\n' || lexer->lookahead == '\r') {
      found_newline = true;
    }
    skip(lexer);
  }

  if (found_newline && lexer->lookahead == 'i' &&
      scan_for_word(lexer, "mport", 5)) {
    // Trailing dot followed by 'import' on next line — produce ASI
    // instead of the dot. mark_end was set before the dot, so the
    // semicolon is zero-width at that position.
    lexer->result_symbol = AUTOMATIC_SEMICOLON;
    return true;
  }

  // Normal dot — include it in the token
  lexer->result_symbol = IMPORT_DOT;
  lexer->mark_end(lexer);
  return true;
}

// Emit a zero-width boundary before an accessor-shaped `get (` or `set (`.
// Without a distinct lookahead, `get`/`set` can be shifted as the infix
// operator of a preceding delegate or explicit-backing-field expression.
// Newlines are deliberately not skipped: ASI owns the newline-separated case.
static bool scan_accessor_start(TSLexer *lexer) {
  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\f') {
    skip(lexer);
  }
  lexer->mark_end(lexer);

  const char *keyword;
  if (lexer->lookahead == 'g') {
    keyword = "get";
  } else if (lexer->lookahead == 's') {
    keyword = "set";
  } else {
    return false;
  }

  for (unsigned i = 0; i < 3; i++) {
    if (lexer->lookahead != keyword[i]) return false;
    skip(lexer);
  }
  if (is_word_char(lexer->lookahead)) return false;

  while (lexer->lookahead == ' ' || lexer->lookahead == '\t' ||
         lexer->lookahead == '\f') {
    skip(lexer);
  }
  if (lexer->lookahead != '(') return false;

  lexer->result_symbol = ACCESSOR_START;
  return true;
}

// After '(', scan its balanced group (tracking nested parens, braces,
// strings, character literals, and comments) and decide whether it belongs
// to a function type: either a '->' at brace depth 0 appears inside the
// group, or the group's close is followed by '->'. Annotation arguments are
// expressions, which never contain a bare '->' outside braces, so this
// separates `@Composable () -> Unit` and `@Composable (A.(B) -> Unit)` from
// `@Ann(args)` on a plain type (BrokkAi/bifrost-dev#2758). The token itself
// covers only the '(' (mark_end is called right after it); everything else
// is lookahead that tree-sitter rewinds.
static bool check_function_type_paren(TSLexer *lexer) {
  advance(lexer); // consume '('
  lexer->mark_end(lexer);
  unsigned paren_depth = 1;
  unsigned brace_depth = 0;
  for (;;) {
    int32_t c = lexer->lookahead;
    if (c == '\0' && lexer->eof(lexer)) return false;
    switch (c) {
      case '"':
        if (!skip_string_literal(lexer)) return false;
        continue;
      case '\'':
        if (!skip_char_literal(lexer)) return false;
        continue;
      case '/':
        advance(lexer);
        if (lexer->lookahead == '/') {
          while (lexer->lookahead != '\n' && lexer->lookahead != '\r' &&
                 !(lexer->lookahead == '\0' && lexer->eof(lexer))) {
            advance(lexer);
          }
        } else if (lexer->lookahead == '*') {
          advance(lexer);
          unsigned depth = 1;
          bool terminated = false;
          while (!terminated) {
            if (lexer->lookahead == '\0' && lexer->eof(lexer)) return false;
            if (lexer->lookahead == '*') {
              advance(lexer);
              if (lexer->lookahead == '/') {
                advance(lexer);
                terminated = --depth == 0;
              }
            } else if (lexer->lookahead == '/') {
              advance(lexer);
              if (lexer->lookahead == '*') {
                advance(lexer);
                depth++;
              }
            } else {
              advance(lexer);
            }
          }
        }
        // a bare '/' is just division; nothing special to skip
        continue;
      case '{':
        brace_depth++;
        advance(lexer);
        continue;
      case '}':
        if (brace_depth > 0) brace_depth--;
        advance(lexer);
        continue;
      case '(':
        paren_depth++;
        advance(lexer);
        continue;
      case ')':
        advance(lexer);
        if (--paren_depth == 0) {
          // Group closed: the function-type arrow must come next
          // (whitespace and comments may intervene).
          if (!skip_whitespace_and_comments(lexer)) return false;
          if (lexer->lookahead != '-') return false;
          advance(lexer);
          return lexer->lookahead == '>';
        }
        continue;
      case '-':
        advance(lexer);
        if (brace_depth == 0 && lexer->lookahead == '>') return true;
        continue;
      default:
        advance(lexer);
        continue;
    }
  }
}

bool tree_sitter_kotlin_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
  // BY_DELEGATION_HINT is declared in the grammar (optional, before `by` in
  // explicit_delegation and property_delegate) purely so it appears in
  // valid_symbols when the parser is in a delegation context. The scanner
  // never emits it; it's used only as a context flag in scan_automatic_semicolon.
  if (valid_symbols[AUTOMATIC_SEMICOLON]) {
    bool ret = scan_automatic_semicolon(lexer, valid_symbols);
    // if we fail to find an automatic semicolon, it's still possible that we may
    // want to lex a string or comment later
    if (ret) return ret;
  }

  if (valid_symbols[ACCESSOR_START] && !valid_symbols[STRING_CONTENT] &&
      scan_accessor_start(lexer)) {
    return true;
  }

  // Match dots in import identifiers, refusing dots that would cause
  // malformed imports to bleed into subsequent import statements.
  if (valid_symbols[IMPORT_DOT]) {
    if (scan_import_dot(lexer)) return true;
  }

  // Match 'constructor' keyword for primary constructors when on the same line
  // (the cross-newline case is handled inside scan_automatic_semicolon)
  if (valid_symbols[PRIMARY_CONSTRUCTOR_KEYWORD] && !valid_symbols[STRING_CONTENT]) {
    while (iswspace(lexer->lookahead)) skip(lexer);
    if (lexer->lookahead == 'c') {
      const char *kw = "constructor";
      bool matched = true;
      for (unsigned i = 0; i < 11; i++) {
        if (lexer->lookahead != kw[i]) { matched = false; break; }
        advance(lexer);
      }
      if (matched && !is_word_char(lexer->lookahead)) {
        lexer->result_symbol = PRIMARY_CONSTRUCTOR_KEYWORD;
        lexer->mark_end(lexer);
        return true;
      }
    }
  }

  // content, end, or interpolation start
  if (valid_symbols[STRING_CONTENT] || valid_symbols[INTERPOLATION_EXPRESSION_START] ||
      valid_symbols[INTERPOLATION_IDENTIFIER_START]) {
    if (scan_string_content(lexer, payload, valid_symbols)) return true;
  }

  // a string might follow after some whitespace, so we can't lookahead
  // until we get rid of it
  while (iswspace(lexer->lookahead)) skip(lexer);

  // A '(' after an annotation name usually opens the annotation's value
  // arguments, but in a type position it can instead open a function type's
  // parameter list or a parenthesized function type: `@Composable () -> Unit`
  // (BrokkAi/bifrost-dev#2758). Emit the distinct annotation-args paren only
  // when the group is NOT a function type; otherwise return false so the
  // internal lexer produces a plain '(' and the annotation-args path dies.
  // The valid_symbols gate confines this to post-annotation states, so
  // ordinary calls and parentheses are unaffected.
  if (lexer->lookahead == '(' && valid_symbols[ANNOTATION_ARGS_PAREN] &&
      !valid_symbols[STRING_CONTENT] &&
      !check_function_type_paren(lexer)) {
    lexer->result_symbol = ANNOTATION_ARGS_PAREN;
    return true;
  }

  if (valid_symbols[STRING_START] && scan_string_start(lexer, payload)) {
    lexer->result_symbol = STRING_START;
    return true;
  }

  if (valid_symbols[MULTILINE_COMMENT] && scan_multiline_comment(lexer)) {
    return true;
  }

  return false;
}

void *tree_sitter_kotlin_external_scanner_create() {
  Stack *stack = ts_calloc(1, sizeof(Stack));
  if (stack == NULL) abort();
  array_init(stack);
  return stack;
}

void tree_sitter_kotlin_external_scanner_destroy(void *payload) {
  Stack *stack = (Stack *)payload;
  array_delete(stack);
  ts_free(stack);
}

unsigned tree_sitter_kotlin_external_scanner_serialize(void *payload, char *buffer) {
  Stack *stack = (Stack *)payload;
  unsigned n = stack->size;
  if (n > TREE_SITTER_SERIALIZATION_BUFFER_SIZE) {
    n = TREE_SITTER_SERIALIZATION_BUFFER_SIZE;
  }
  if (n > 0) {
    // it's an undefined behavior to memcpy 0 bytes
    memcpy(buffer, stack->contents, n);
  }
  return n;
}

void tree_sitter_kotlin_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
  Stack *stack = (Stack *)payload;
  // Stack entries are 2 bytes each (delimiter + prefix_len).
  // Discard corrupted state with odd length.
  if (length > 0 && length % 2 == 0) {
    array_reserve(stack, length);
    memcpy(stack->contents, buffer, length);
    stack->size = length;
  } else {
    array_clear(stack);
  }
}
