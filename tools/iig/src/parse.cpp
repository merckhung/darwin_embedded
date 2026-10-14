/*
 * parse.cpp - parser for the .iig subset (see ast.h).
 *
 * Strategy: scan the raw text for top-level `class ... { ... };` definitions
 * (forward declarations pass through). Everything between class definitions is
 * kept verbatim as passthrough chunks; class bodies are re-lexed into method
 * declarations. Comments never nest and .iig class bodies contain no function
 * bodies, so a depth-counting scanner is sufficient.
 */
#include "ast.h"

#include <cctype>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <map>

namespace iig {

uint64_t
iigHash(const std::string &s)
{
  uint64_t h = 0xcbf29ce484222325ULL;
  for (unsigned char c : s) {
    h ^= c;
    h *= 0x100000001b3ULL;
  }
  return h;
}

namespace {

/* Return a copy of text with comments blanked (newlines kept so that line
 * numbers and chunk offsets stay valid). */
std::string
blankComments(const std::string &t)
{
  std::string out = t;
  size_t i = 0;
  while (i < out.size()) {
    if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '/') {
      while (i < out.size() && out[i] != '\n') out[i++] = ' ';
    } else if (out[i] == '/' && i + 1 < out.size() && out[i + 1] == '*') {
      out[i] = out[i + 1] = ' ';
      i += 2;
      while (i < out.size() && !(out[i] == '*' && i + 1 < out.size() && out[i + 1] == '/')) {
        if (out[i] != '\n') out[i] = ' ';
        i++;
      }
      if (i < out.size()) { out[i] = out[i + 1] = ' '; i += 2; }
    } else if (out[i] == '"') {
      i++;
      while (i < out.size() && out[i] != '"') {
        if (out[i] == '\\') i++;
        i++;
      }
      i++;
    } else {
      i++;
    }
  }
  return out;
}

bool
isIdentChar(char c)
{
  return isalnum((unsigned char)c) || c == '_';
}

/* Tokenize a declaration into identifiers/punctuation. */
std::vector<std::string>
tokenize(const std::string &s)
{
  std::vector<std::string> toks;
  size_t i = 0;
  while (i < s.size()) {
    if (isspace((unsigned char)s[i])) { i++; continue; }
    if (isIdentChar(s[i])) {
      size_t j = i;
      while (j < s.size() && isIdentChar(s[j])) j++;
      toks.push_back(s.substr(i, j - i));
      i = j;
    } else {
      toks.push_back(std::string(1, s[i]));
      i++;
    }
  }
  return toks;
}

std::string
joinType(const std::vector<std::string> &toks, size_t begin, size_t end)
{
  std::string out;
  for (size_t i = begin; i < end; i++) {
    const std::string &t = toks[i];
    if (!out.empty() && isIdentChar(t[0]) &&
        (isIdentChar(out.back()) || out.back() == '*' || out.back() == '&'))
      out += ' ';
    /* keep "Foo *" spacing conventional */
    if (!out.empty() && (t == "*" || t == "&") && out.back() != '*' && out.back() != '&')
      out += ' ';
    out += t;
  }
  return out;
}

/* Split s on top-level commas (ignores nesting in () <> []). */
std::vector<std::string>
splitTopLevel(const std::string &s, char sep)
{
  std::vector<std::string> parts;
  int depth = 0;
  size_t start = 0;
  for (size_t i = 0; i < s.size(); i++) {
    char c = s[i];
    if (c == '(' || c == '<' || c == '[') depth++;
    else if (c == ')' || c == '>' || c == ']') depth--;
    else if (c == sep && depth == 0) {
      parts.push_back(s.substr(start, i - start));
      start = i + 1;
    }
  }
  parts.push_back(s.substr(start));
  return parts;
}

std::string
trim(const std::string &s)
{
  size_t a = s.find_first_not_of(" \t\r\n");
  if (a == std::string::npos) return "";
  size_t b = s.find_last_not_of(" \t\r\n");
  return s.substr(a, b - a + 1);
}

bool
parseParam(const std::string &raw, Param &p, std::string &error)
{
  std::string s = trim(raw);
  if (s.empty() || s == "void") return false; /* no parameter */

  /* default value */
  auto eq = splitTopLevel(s, '=');
  if (eq.size() == 2) {
    p.defaultValue = trim(eq[1]);
    s = trim(eq[0]);
  } else if (eq.size() > 2) {
    error = "unparsable parameter: " + raw;
    return false;
  }

  auto toks = tokenize(s);
  /* strip annotations: TARGET (bare), TYPE (...), the latter takes a
   * parenthesized argument (e.g. "OSAction *action TYPE (Cls::Method)") that
   * must be consumed as a unit or its tokens get mistaken for the parameter
   * name/type. */
  for (size_t i = 0; i < toks.size();) {
    if (toks[i] == "TARGET") {
      p.isTarget = true;
      toks.erase(toks.begin() + i);
    } else if (toks[i] == "PORTMAKESEND") {
      p.portDisposition = "MACH_MSG_TYPE_MAKE_SEND";
      toks.erase(toks.begin() + i);
    } else if (toks[i] == "PORTCOPYSEND") {
      p.portDisposition = "MACH_MSG_TYPE_COPY_SEND";
      toks.erase(toks.begin() + i);
    } else if (toks[i] == "TYPE" && i + 1 < toks.size() && toks[i + 1] == "(") {
      size_t j = i + 1;
      int depth = 0;
      do {
        if (toks[j] == "(") depth++;
        else if (toks[j] == ")") depth--;
        j++;
      } while (j < toks.size() && depth > 0);
      toks.erase(toks.begin() + i, toks.begin() + j);
    } else i++;
  }
  if (toks.empty()) { error = "empty parameter: " + raw; return false; }

  /* The name precedes an array declarator; its bound may be an identifier. */
  size_t declaratorEnd = toks.size();
  for (size_t i = 0; i < toks.size(); i++) {
    if (toks[i] == "[") { declaratorEnd = i; break; }
  }
  size_t nameIdx = toks.size();
  for (size_t i = declaratorEnd; i-- > 0;) {
    if (isIdentChar(toks[i][0]) && !isdigit((unsigned char)toks[i][0])) {
      nameIdx = i;
      break;
    }
  }
  if (nameIdx == toks.size()) { error = "no parameter name in: " + raw; return false; }
  p.name = toks[nameIdx];
  /* array parameter "T * const foo[32]" decays to one more pointer level,
   * "T ** const" (matches Apple's iig output) */
  bool isArray = false;
  for (size_t i = nameIdx + 1; i < toks.size();) {
    if (toks[i] == "[") {
      isArray = true;
      toks.erase(toks.begin() + i); /* '[' */
      if (i < toks.size() && toks[i] != "]") {
        p.arrayCountExpression = toks[i];
        p.arrayCount = atoi(toks[i].c_str());
        toks.erase(toks.begin() + i);
      }
      if (i < toks.size() && toks[i] == "]") toks.erase(toks.begin() + i);
    } else i++;
  }
  /* type is everything except the name token */
  std::vector<std::string> typeToks;
  for (size_t i = 0; i < toks.size(); i++)
    if (i != nameIdx) typeToks.push_back(toks[i]);
  if (isArray) {
    /* the added '*' goes after existing stars, before trailing qualifiers */
    size_t at = typeToks.size();
    for (size_t i = 0; i < typeToks.size(); i++)
      if (typeToks[i] == "*") at = i + 1;
    typeToks.insert(typeToks.begin() + at, "*");
  }
  p.type = joinType(typeToks, 0, typeToks.size());
  return true;
}

bool
parseMethod(const std::string &decl, Method &m, std::string &error)
{
  /* split at the parameter list parens */
  size_t open = decl.find('(');
  if (open == std::string::npos) return false; /* not a method (e.g. field) */
  int depth = 1;
  size_t close = open + 1;
  while (close < decl.size() && depth) {
    if (decl[close] == '(') depth++;
    else if (decl[close] == ')') depth--;
    close++;
  }
  if (depth) { error = "unbalanced parens: " + decl; return false; }
  close--; /* at ')' */

  std::string head = decl.substr(0, open);
  std::string args = decl.substr(open + 1, close - open - 1);
  std::string tail = decl.substr(close + 1);

  auto headToks = tokenize(head);
  if (headToks.empty()) { error = "no method name: " + decl; return false; }
  m.name = headToks.back();
  headToks.pop_back();
  for (size_t i = 0; i < headToks.size();) {
    if (headToks[i] == "static") { m.isStatic = true; headToks.erase(headToks.begin() + i); }
    else if (headToks[i] == "virtual") { m.isVirtual = true; headToks.erase(headToks.begin() + i); }
    else i++;
  }
  if (headToks.empty()) { error = "no return type: " + decl; return false; }
  m.returnType = joinType(headToks, 0, headToks.size());

  for (auto &part : splitTopLevel(args, ',')) {
    Param p;
    std::string perr;
    if (parseParam(part, p, perr))
      m.params.push_back(p);
    else if (!perr.empty()) { error = perr; return false; }
  }

  /* trailing annotations */
  auto tailToks = tokenize(tail);
  for (size_t i = 0; i < tailToks.size(); i++) {
    const std::string &t = tailToks[i];
    if (t == "LOCAL") m.isLocal = true;
    else if (t == "LOCALONLY") m.isLocalOnly = true;
    else if (t == "const") m.isConst = true;
    else if (t == "KERNELONLY" || t == "KERNEL") m.isKernelOnly = true;
    else if (t == "INVOKEREPLY") m.isInvokeReply = true;
    else if (t == "override") m.isOverride = true;
    else if (t == "TYPE" || t == "QUEUENAME") {
      /* consume "( tokens )" */
      std::string arg;
      size_t j = i + 1;
      if (j < tailToks.size() && tailToks[j] == "(") {
        int d = 1;
        j++;
        while (j < tailToks.size() && d) {
          if (tailToks[j] == "(") d++;
          else if (tailToks[j] == ")") { if (--d == 0) break; }
          arg += tailToks[j++];
        }
      }
      if (t == "TYPE") m.actionType = arg;
      else m.queueName = arg;
      i = j;
    } else if (t == "=" && i + 1 < tailToks.size() && tailToks[i + 1] == "0") {
      m.isPureVirtual = true;
      i++;
    }
    /* anything else (availability macros etc.) is ignored */
  }
  return true;
}

} /* namespace */

bool
parseIigFile(const std::string &path, const std::string &text, File &out,
       std::string &error)
{
  out.path = path;
  size_t slash = path.find_last_of('/');
  out.basename = (slash == std::string::npos) ? path : path.substr(slash + 1);

  /* Seed typedefs that are declared in commonly included .iig files. This
   * generator processes one file at a time, so local scanning alone loses
   * inherited ABI shapes such as IODispatchQueueName. */
  out.charArrayTypedefs["IODispatchQueueName"] = 256;
  out.charArrayTypedefs["IOServiceName"] = 128;
  out.charArrayTypedefs["IOPropertyName"] = 128;
  out.charArrayTypedefs["IORegistryPlaneName"] = 128;
  out.arrayTypedefs["IODispatchQueueName"] = { "char", 256 };
  out.arrayTypedefs["IOServiceName"] = { "char", 128 };
  out.arrayTypedefs["IOPropertyName"] = { "char", 128 };
  out.arrayTypedefs["IORegistryPlaneName"] = { "char", 128 };
  out.enumConstants["kIOUserClientScalarArrayCountMax"] = 16;
  out.enumConstants["kIOUserClientAsyncReferenceCountMax"] = 16;
  out.enumConstants["kIOUserClientAsyncArgumentsCountMax"] = 16;
  out.arrayTypedefs["IOUserClientScalarArray"] = { "uint64_t", 16 };
  out.arrayTypedefs["IOUserClientAsyncReferenceArray"] = { "uint64_t", 16 };
  out.arrayTypedefs["IOUserClientAsyncArgumentsArray"] = { "uint64_t", 16 };

  /* Fixed-array typedefs used for bounded strings and inline POD buffers.
   * Scanned over raw text since these are simple top-level declarations, not
   * classes. */
  {
    std::string clean0 = blankComments(text);
    size_t lineStart = 0;
    while (lineStart < clean0.size()) {
      size_t lineEnd = clean0.find('\n', lineStart);
      if (lineEnd == std::string::npos) lineEnd = clean0.size();
      auto define = tokenize(clean0.substr(lineStart, lineEnd - lineStart));
      if (define.size() == 4 && define[0] == "#" && define[1] == "define") {
        char *end = nullptr;
        long value = strtol(define[3].c_str(), &end, 0);
        if (end && *end == '\0' && value > 0 && value <= 0x7fffffff)
          out.enumConstants[define[2]] = static_cast<int>(value);
      }
      lineStart = lineEnd + 1;
    }
    size_t p = 0;
    while ((p = clean0.find("typedef", p)) != std::string::npos) {
      size_t semi = clean0.find(';', p);
      if (semi == std::string::npos) break;
      auto toks = tokenize(clean0.substr(p, semi - p));
      if (toks.size() >= 6 && toks[0] == "typedef" &&
          toks[3] == "[" && toks[5] == "]") {
        int count = atoi(toks[4].c_str());
        auto it = out.enumConstants.find(toks[4]);
        if (!count && it != out.enumConstants.end()) count = it->second;
        if (count > 0) {
          out.arrayTypedefs[toks[2]] = { toks[1], count };
          if (toks[1] == "char") out.charArrayTypedefs[toks[2]] = count;
        }
      }
      p = semi + 1;
    }
  }

  /* pull out "@iig implementation" ... "@iig end" regions: their content is
   * impl-only (extra includes); blank them in the header text (newlines kept
   * so line numbers stay stable) */
  std::string src = text;
  size_t ib;
  while ((ib = src.find("@iig implementation")) != std::string::npos) {
    size_t ie = src.find("@iig end", ib);
    if (ie == std::string::npos) { error = "unterminated @iig implementation"; return false; }
    size_t contentBegin = src.find('\n', ib);
    out.implText += "/* @iig implementation */";
    out.implText += src.substr(contentBegin, ie - contentBegin);
    out.implText += "/* @iig end */\n";
    for (size_t i = ib; i < ie + 8; i++)
      if (src[i] != '\n') src[i] = ' ';
  }


  std::string clean = blankComments(src);

  size_t pos = 0;        /* scan position in clean/text */
  size_t chunkStart = 0; /* start of the current passthrough chunk */

  auto lineOf = [&](size_t off) {
    int line = 1;
    for (size_t i = 0; i < off && i < src.size(); i++)
      if (src[i] == '\n') line++;
    return line;
  };

  while (pos < clean.size()) {
    /* find the next top-level "class" keyword */
    size_t c = pos;
    bool found = false;
    while ((c = clean.find("class", c)) != std::string::npos) {
      bool leftOk = (c == 0) || !isIdentChar(clean[c - 1]);
      bool rightOk = (c + 5 >= clean.size()) || !isIdentChar(clean[c + 5]);
      if (leftOk && rightOk) { found = true; break; }
      c += 5;
    }
    if (!found) break;

    /* definition or forward declaration? look for '{' before ';' */
    size_t brace = clean.find_first_of("{;", c);
    if (brace == std::string::npos || clean[brace] == ';') {
      pos = (brace == std::string::npos) ? clean.size() : brace + 1;
      continue;
    }

    /* find matching close brace and trailing ';' */
    int depth = 1;
    size_t end = brace + 1;
    while (end < clean.size() && depth) {
      if (clean[end] == '{') depth++;
      else if (clean[end] == '}') depth--;
      end++;
    }
    if (depth) { error = "unbalanced braces in class at offset " + std::to_string(c); return false; }
    size_t semi = clean.find(';', end);
    if (semi == std::string::npos) { error = "missing ';' after class"; return false; }

    Class cls;
    cls.firstLine = lineOf(c);
    cls.lastLine = lineOf(semi);

    /* head: between "class" and '{' */
    std::string head = clean.substr(c + 5, brace - (c + 5));
    auto headToks = tokenize(head);
    size_t hi = 0;
    while (hi < headToks.size()) {
      const std::string &t = headToks[hi];
      if (t == "NATIVE") { cls.isNative = true; hi++; }
      else if (t == "KERNEL") { cls.isKernel = true; hi++; }
      else if (t == "EXTENDS") {
        /* EXTENDS (Name) */
        if (hi + 3 < headToks.size() && headToks[hi + 1] == "(") {
          cls.extendsName = headToks[hi + 2];
          hi += 4;
        } else { error = "malformed EXTENDS"; return false; }
      } else break;
    }
    if (hi >= headToks.size()) { error = "class with no name"; return false; }
    cls.name = headToks[hi++];
    /* ": public Super" */
    if (hi < headToks.size() && headToks[hi] == ":") {
      hi++;
      while (hi < headToks.size() && (headToks[hi] == "public" ||
           headToks[hi] == "private" || headToks[hi] == "protected"))
        hi++;
      if (hi < headToks.size()) cls.superName = headToks[hi];
    }

    /* body: declarations separated by ';'. Preprocessor conditionals inside
     * a class body (e.g. "#if DRIVERKIT_PRIVATE" gating OSAction::Create)
     * are blanked out here - iig-lite does not track conditional guards,
     * so gated declarations are always emitted (matches the vendored xnu
     * corpus, which was generated with DRIVERKIT_PRIVATE defined: Create
     * and SetDispatchQueue/etc. appear unconditionally in it too). Left
     * unblanked, a directive line glues onto the next declaration's first
     * token stream and corrupts it (no ';' separates them). */
    std::string body = clean.substr(brace + 1, (end - 1) - (brace + 1));
    {
      size_t i = 0;
      while (i < body.size()) {
        if (body[i] == '\n') {
          size_t j = i + 1;
          while (j < body.size() && isspace((unsigned char)body[j]) && body[j] != '\n') j++;
          if (j < body.size() && body[j] == '#') {
            size_t lineEnd = body.find('\n', j);
            if (lineEnd == std::string::npos) lineEnd = body.size();
            for (size_t k = j; k < lineEnd; k++) body[k] = ' ';
            i = lineEnd;
            continue;
          }
        }
        i++;
      }
      /* directive on the body's very first line (no leading '\n') */
      size_t j = 0;
      while (j < body.size() && isspace((unsigned char)body[j]) && body[j] != '\n') j++;
      if (j < body.size() && body[j] == '#') {
        size_t lineEnd = body.find('\n', j);
        if (lineEnd == std::string::npos) lineEnd = body.size();
        for (size_t k = j; k < lineEnd; k++) body[k] = ' ';
      }
    }
    for (auto &decl : splitTopLevel(body, ';')) {
      std::string d = trim(decl);
      /* strip access labels prefixed to a declaration */
      for (;;) {
        bool again = false;
        for (const char *lbl : { "public:", "protected:", "private:" }) {
          size_t l = strlen(lbl);
          /* labels may have internal whitespace ("public :") --
           * normalize via token check instead */
          (void)l;
        }
        auto toks = tokenize(d);
        if (toks.size() >= 2 && toks[1] == ":" &&
          (toks[0] == "public" || toks[0] == "protected" || toks[0] == "private")) {
          size_t colon = d.find(':');
          d = trim(d.substr(colon + 1));
          again = true;
        }
        if (!again) break;
      }
      if (d.empty()) continue;
      Method m;
      std::string merr;
      m.fromExtends = !cls.extendsName.empty();
      if (parseMethod(d, m, merr)) {
        for (auto &param : m.params) {
          if (!param.arrayCountExpression.empty() && param.arrayCount == 0) {
            auto constant = out.enumConstants.find(param.arrayCountExpression);
            if (constant == out.enumConstants.end()) {
              error = "unresolved array bound: " + param.arrayCountExpression;
              return false;
            }
            param.arrayCount = constant->second;
          }
        }
        cls.methods.push_back(m);
      }
      else if (!merr.empty()) { error = merr; return false; }
      /* non-method declarations (fields, using, typedefs) are skipped */
    }

    /* record chunks */
    if (c > chunkStart) {
      Chunk t;
      t.kind = Chunk::Text;
      t.text = src.substr(chunkStart, c - chunkStart);
      out.chunks.push_back(t);
    }
    Chunk cc;
    cc.kind = Chunk::ClassDecl;
    cc.text = src.substr(c, semi + 1 - c);
    cc.classIndex = out.classes.size();
    out.chunks.push_back(cc);
    out.classes.push_back(cls);

    pos = chunkStart = semi + 1;
  }
  if (chunkStart < src.size()) {
    Chunk t;
    t.kind = Chunk::Text;
    t.text = src.substr(chunkStart);
    out.chunks.push_back(t);
  }

  /* fold EXTENDS classes into their targets; Apple's iig lists the private
   * extension's methods first */
  for (auto &cls : out.classes) {
    if (cls.extendsName.empty()) continue;
    for (auto &target : out.classes) {
      if (target.name == cls.extendsName) {
        target.methods.insert(target.methods.begin(),
                              cls.methods.begin(), cls.methods.end());
        break;
      }
    }
  }
  return true;
}

} /* namespace iig */
