// Copyright 2020 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/dslx/scanner.h"

#include "absl/status/statusor.h"
#include "absl/strings/match.h"

namespace xls::dslx {

absl::StatusOr<int64_t> Token::GetValueAsInt64() const {
  absl::optional<std::string> value = GetValue();
  if (!value) {
    return absl::InvalidArgumentError(
        "Token does not have a (string) value; cannot convert to int64_t.");
  }
  int64_t result;
  if (absl::SimpleAtoi(*value, &result)) {
    return result;
  }
  return absl::InvalidArgumentError("Could not convert value to int64_t: " +
                                    *value);
}

std::string Token::ToErrorString() const {
  if (kind_ == TokenKind::kKeyword) {
    return absl::StrFormat("keyword:%s", KeywordToString(GetKeyword()));
  }
  return TokenKindToString(kind_);
}

std::string Token::ToString() const {
  if (kind() == TokenKind::kKeyword) {
    return KeywordToString(GetKeyword());
  }
  if (GetValue().has_value()) {
    return *GetValue();
  }
  return TokenKindToString(kind_);
}

std::string Token::ToRepr() const {
  if (kind_ == TokenKind::kKeyword) {
    return absl::StrFormat("Token(%s, %s)", span_.ToRepr(),
                           KeywordToString(GetKeyword()));
  }
  if (GetValue().has_value()) {
    return absl::StrFormat("Token(%s, %s, \"%s\")", TokenKindToString(kind_),
                           span_.ToRepr(), *GetValue());
  }
  return absl::StrFormat("Token(%s, %s)", TokenKindToString(kind_),
                         span_.ToRepr());
}

char Scanner::PopChar() {
  XLS_CHECK(!AtEof()) << "Cannot pop character when at EOF.";
  char c = PeekChar();
  index_ += 1;
  if (c == '\n') {
    lineno_ += 1;
    colno_ = 0;
  } else {
    colno_ += 1;
  }
  return c;
}

void Scanner::DropChar(int64_t count) {
  for (int64_t i = 0; i < count; ++i) {
    (void)PopChar();
  }
}

bool Scanner::TryDropChar(char target) {
  if (!AtCharEof() && PeekChar() == target) {
    DropChar();
    return true;
  }
  return false;
}

absl::StatusOr<Token> Scanner::PopComment(const Pos& start_pos) {
  std::string chars;
  while (!AtCharEof() && !TryDropChar('\n')) {
    chars.append(1, PopChar());
  }
  return Token(TokenKind::kComment, Span(start_pos, GetPos()), chars);
}

absl::StatusOr<Token> Scanner::PopWhitespace(const Pos& start_pos) {
  XLS_CHECK(AtWhitespace());
  std::string chars;
  while (!AtCharEof() && AtWhitespace()) {
    chars.append(1, PopChar());
  }
  return Token(TokenKind::kWhitespace, Span(start_pos, GetPos()), chars);
}

/* static */ absl::optional<Keyword> Scanner::GetKeyword(absl::string_view s) {
  static const auto* mapping = new absl::flat_hash_map<std::string, Keyword>{
#define MAKE_ITEM(__enum, unused, __str, ...) {__str, Keyword::__enum},
      XLS_DSLX_KEYWORDS(MAKE_ITEM)
#undef MAKE_ITEM
  };
  auto it = mapping->find(s);
  if (it == mapping->end()) {
    return absl::nullopt;
  }
  return it->second;
}

absl::StatusOr<Token> Scanner::ScanIdentifierOrKeyword(char startc,
                                                       const Pos& start_pos) {
  // The leading character is `startc` so we scan out trailing identifiers.
  auto is_trailing_identifier_char = [](char c) {
    return std::isalpha(c) || std::isdigit(c) || c == '_' || c == '!' ||
           c == '\'';
  };
  std::string s = ScanWhile(startc, is_trailing_identifier_char);
  Span span(start_pos, GetPos());
  if (absl::optional<Keyword> keyword = GetKeyword(s)) {
    return Token(span, *keyword);
  }
  return Token(TokenKind::kIdentifier, span, std::move(s));
}

absl::StatusOr<absl::optional<Token>> Scanner::TryPopWhitespaceOrComment() {
  const Pos start_pos = GetPos();
  if (AtCharEof()) {
    return Token(TokenKind::kEof, Span(start_pos, start_pos));
  }
  if (AtWhitespace()) {
    XLS_ASSIGN_OR_RETURN(Token token, PopWhitespace(start_pos));
    return token;
  }
  if (PeekChar() == '/' && PeekChar2OrNull() == '/') {
    DropChar(2);
    XLS_ASSIGN_OR_RETURN(Token token, PopComment(start_pos));
    return token;
  }
  return absl::nullopt;
}

absl::StatusOr<Token> Scanner::ScanNumber(char startc, const Pos& start_pos) {
  bool negative = startc == '-';
  if (negative) {
    startc = PopChar();
  }

  std::string s;
  if (startc == '0' && TryDropChar('x')) {  // Hex radix.
    s = ScanWhile("0x", [](char c) {
      return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') ||
             ('A' <= c && c <= 'F') || c == '_';
    });
    if (s == "0x") {
      return ScanError(Span(GetPos(), GetPos()),
                       "Expected hex characters following 0x prefix.");
    }
  } else if (startc == '0' && TryDropChar('b')) {  // Bin prefix.
    s = ScanWhile("0b",
                  [](char c) { return ('0' <= c && c <= '1') || c == '_'; });
    if (s == "0b") {
      return ScanError(Span(GetPos(), GetPos()),
                       "Expected binary characters following 0b prefix");
    }
    if (!AtEof() && '0' <= PeekChar() && PeekChar() <= '9') {
      return ScanError(
          Span(GetPos(), GetPos()),
          absl::StrFormat("Invalid digit for binary number: '%c'", PeekChar()));
    }
  } else {
    s = ScanWhile(startc, [](char c) { return std::isdigit(c); });
    if (absl::StartsWith(s, "0") && s.size() != 1) {
      return ScanError(
          Span(GetPos(), GetPos()),
          "Invalid radix for number, expect 0b or 0x because of leading 0.");
    }
    XLS_CHECK(!s.empty())
        << "Must have seen numerical digits to attempt to scan a number.";
  }
  if (negative) {
    s = "-" + s;
  }
  return Token(TokenKind::kNumber, Span(start_pos, GetPos()), s);
}

std::string KeywordToString(Keyword keyword) {
  switch (keyword) {
#define MAKE_CASE(__enum, unused, __str, ...) \
  case Keyword::__enum:                       \
    return __str;
    XLS_DSLX_KEYWORDS(MAKE_CASE)
#undef MAKE_CASE
  }
  return absl::StrFormat("<invalid Keyword(%d)>", static_cast<int>(keyword));
}

absl::StatusOr<Keyword> KeywordFromString(absl::string_view s) {
#define MAKE_CASE(__enum, unused, __str, ...) \
  if (s == __str) {                           \
    return Keyword::__enum;                   \
  }
  XLS_DSLX_KEYWORDS(MAKE_CASE)
#undef MAKE_CASE
  return absl::InvalidArgumentError(
      absl::StrFormat("Not a valid keyword: \"%s\"", s));
}

std::string TokenKindToString(TokenKind kind) {
  switch (kind) {
#define MAKE_CASE(__enum, unused, __str, ...) \
  case TokenKind::__enum:                     \
    return __str;
    XLS_DSLX_TOKEN_KINDS(MAKE_CASE)
#undef MAKE_CASE
  }
  return absl::StrFormat("<invalid TokenKind(%d)>", static_cast<int>(kind));
}

absl::StatusOr<TokenKind> TokenKindFromString(absl::string_view s) {
#define MAKE_CASE(__enum, unused, __str, ...) \
  if (s == __str) {                           \
    return TokenKind::__enum;                 \
  }
  XLS_DSLX_TOKEN_KINDS(MAKE_CASE)
#undef MAKE_CASE
  return absl::InvalidArgumentError(
      absl::StrFormat("Not a token kind: \"%s\"", s));
}

bool Scanner::AtWhitespace() const {
  switch (PeekChar()) {
    case ' ':
    case '\r':
    case '\n':
    case '\t':
    case '\xa0':
      return true;
    default:
      return false;
  }
}

void Scanner::DropCommentsAndLeadingWhitespace() {
  while (!AtCharEof()) {
    if (AtWhitespace()) {
      DropChar();
    } else if (PeekChar() == '/' && PeekChar2OrNull() == '/') {
      DropChar(2);  // Get rid of leading "//"
      while (!AtCharEof()) {
        if (PopChar() == '\n') {
          break;
        }
      }
    } else {
      break;
    }
  }
}

absl::StatusOr<Token> Scanner::ScanChar(const Pos& start_pos) {
  const char open_quote = PopChar();
  XLS_CHECK_EQ(open_quote, '\'');
  if (AtCharEof()) {
    return ScanError(Span(GetPos(), GetPos()),
                     "Expected character after single quote, saw end of file.");
  }
  char c = PopChar();
  if (AtCharEof() || !TryDropChar('\'')) {
    std::string msg = absl::StrFormat(
        "Expected closing single quote for character literal; got %s",
        AtCharEof() ? std::string("end of file") : std::string(1, PeekChar()));
    return ScanError(Span(GetPos(), GetPos()), msg);
  }
  return Token(TokenKind::kCharacter, Span(start_pos, GetPos()),
               std::string(1, c));
}

absl::StatusOr<Token> Scanner::Pop() {
  if (include_whitespace_and_comments_) {
    XLS_ASSIGN_OR_RETURN(absl::optional<Token> tok,
                         TryPopWhitespaceOrComment());
    if (tok) {
      return *tok;
    }
  } else {
    DropCommentsAndLeadingWhitespace();
  }

  // Record the position the token starts at.
  const Pos start_pos = GetPos();
  auto mk_span = [&]() -> Span { return Span(start_pos, GetPos()); };

  // After dropping whitespace this may be EOF.
  if (AtCharEof()) {
    return Token(TokenKind::kEof, mk_span());
  }

  // Peek at one character for prefix scanning.
  const char startc = PeekChar();
  absl::optional<Token> result;
  switch (startc) {
    case '\'': {
      XLS_ASSIGN_OR_RETURN(result, ScanChar(start_pos));
      break;
    }
    case '#':
      DropChar();
      result = Token(TokenKind::kHash, mk_span());
      break;
    case '!':
      DropChar();
      if (TryDropChar('=')) {
        result = Token(TokenKind::kBangEquals, mk_span());
      } else {
        result = Token(TokenKind::kBang, mk_span());
      }
      break;
    case '=':
      DropChar();
      if (TryDropChar('=')) {
        result = Token(TokenKind::kDoubleEquals, mk_span());
      } else if (TryDropChar('>')) {
        result = Token(TokenKind::kFatArrow, mk_span());
      } else {
        result = Token(TokenKind::kEquals, mk_span());
      }
      break;
    case '+':
      DropChar();
      if (TryDropChar('+')) {
        result = Token(TokenKind::kDoublePlus, mk_span());
      } else if (TryDropChar(':')) {
        result = Token(TokenKind::kPlusColon, mk_span());
      } else {
        result = Token(TokenKind::kPlus, mk_span());
      }
      break;
    case '<':
      DropChar();
      if (TryDropChar('<')) {
        result = Token(TokenKind::kDoubleOAngle, mk_span());
      } else if (TryDropChar('=')) {
        result = Token(TokenKind::kOAngleEquals, mk_span());
      } else {
        result = Token(TokenKind::kOAngle, mk_span());
      }
      break;
    case '>':
      DropChar();
      if (TryDropChar('>')) {
        if (TryDropChar('>')) {
          result = Token(TokenKind::kTripleCAngle, mk_span());
        } else {
          result = Token(TokenKind::kDoubleCAngle, mk_span());
        }
      } else if (TryDropChar('=')) {
        result = Token(TokenKind::kCAngleEquals, mk_span());
      } else {
        result = Token(TokenKind::kCAngle, mk_span());
      }
      break;
    case '.':
      DropChar();
      if (TryDropChar('.')) {
        if (TryDropChar('.')) {
          result = Token(TokenKind::kEllipsis, mk_span());
        } else {
          result = Token(TokenKind::kDoubleDot, mk_span());
        }
      } else {
        result = Token(TokenKind::kDot, mk_span());
      }
      break;
    case ':':
      DropChar();
      if (TryDropChar(':')) {
        result = Token(TokenKind::kDoubleColon, mk_span());
      } else {
        result = Token(TokenKind::kColon, mk_span());
      }
      break;
    case '|':
      DropChar();
      if (TryDropChar('|')) {
        result = Token(TokenKind::kDoubleBar, mk_span());
      } else {
        result = Token(TokenKind::kBar, mk_span());
      }
      break;
    case '&':
      DropChar();
      if (TryDropChar('&')) {
        result = Token(TokenKind::kDoubleAmpersand, mk_span());
      } else {
        result = Token(TokenKind::kAmpersand, mk_span());
      }
      break;
    // clang-format off
    case '(': DropChar(); result = Token(TokenKind::kOParen, mk_span()); break;  // NOLINT
    case ')': DropChar(); result = Token(TokenKind::kCParen, mk_span()); break;  // NOLINT
    case '[': DropChar(); result = Token(TokenKind::kOBrack, mk_span()); break;  // NOLINT
    case ']': DropChar(); result = Token(TokenKind::kCBrack, mk_span()); break;  // NOLINT
    case '{': DropChar(); result = Token(TokenKind::kOBrace, mk_span()); break;  // NOLINT
    case '}': DropChar(); result = Token(TokenKind::kCBrace, mk_span()); break;  // NOLINT
    case ',': DropChar(); result = Token(TokenKind::kComma, mk_span()); break;  // NOLINT
    case ';': DropChar(); result = Token(TokenKind::kSemi, mk_span()); break;  // NOLINT
    case '*': DropChar(); result = Token(TokenKind::kStar, mk_span()); break;  // NOLINT
    case '^': DropChar(); result = Token(TokenKind::kHat, mk_span()); break;  // NOLINT
    case '/': DropChar(); result = Token(TokenKind::kSlash, mk_span()); break;  // NOLINT
    // clang-format on
    default:
      if (std::isalpha(startc) || startc == '_') {
        XLS_ASSIGN_OR_RETURN(result,
                             ScanIdentifierOrKeyword(PopChar(), start_pos));
      } else if (std::isdigit(startc) ||
                 (startc == '-' && std::isdigit((PeekChar2OrNull())))) {
        XLS_ASSIGN_OR_RETURN(result, ScanNumber(PopChar(), start_pos));
      } else if (startc == '-') {  // Minus handling is after the "number" path.
        DropChar();
        if (TryDropChar('>')) {
          result = Token(TokenKind::kArrow, mk_span());
        } else {
          result = Token(TokenKind::kMinus, mk_span());
        }
      } else {
        return ScanError(Span(GetPos(), GetPos()),
                         absl::StrFormat("Unrecognized character: '%c' (%#x)",
                                         startc, startc));
      }
  }

  XLS_CHECK(result.has_value());
  return std::move(result).value();
}

const absl::flat_hash_set<Keyword>& GetTypeKeywords() {
  static const absl::flat_hash_set<Keyword>* singleton = ([] {
    auto* s = new absl::flat_hash_set<Keyword>;
#define ADD_TO_SET(__enum, ...) s->insert(Keyword::__enum);
    XLS_DSLX_TYPE_KEYWORDS(ADD_TO_SET)
#undef ADD_TO_SET
    return s;
  })();
  return *singleton;
}

}  // namespace xls::dslx
