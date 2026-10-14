/*
 * ast.h - parsed representation of a .iig file.
 *
 * The .iig dialect is C++ class declarations plus direction annotations:
 *   class [NATIVE] [KERNEL] Foo : public Super { ... };
 *   class EXTENDS (Foo) FooPrivate { ... };   // merges into Foo
 * Method annotations:
 *   (none)    - RPC method implemented in the kernel (for KERNEL classes);
 *                callers get a marshaling wrapper, kernel supplies Name_Impl.
 *   LOCAL     - implemented by the user-space driver; kernel calls out.
 *   LOCALONLY - no RPC at all; hand-implemented on both sides.
 *   TYPE(Cls::Method) - method is an OSAction callback of that type.
 *   TARGET    - parameter annotation: the OSAction that carries the target.
 *   QUEUENAME(name) - dispatch queue hint (parsed, unused by codegen).
 */
#ifndef IIG_AST_H
#define IIG_AST_H

#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace iig {

struct Param {
  std::string type;        /* canonical type text, e.g. "IOService *" */
  std::string name;
  std::string defaultValue; /* without '=', empty if none */
  bool isTarget = false;    /* TARGET annotation */
  int arrayCount = 0;       /* "T * const foo[32]" -> 32; 0 = not an array */
  std::string arrayCountExpression;
  std::string portDisposition; /* MACH_MSG_TYPE_* for PORTMAKESEND/COPYSEND */
};

struct Method {
  std::string returnType;   /* e.g. "kern_return_t", "void" */
  std::string name;
  std::vector<Param> params;
  bool isStatic = false;
  bool isVirtual = false;
  bool isOverride = false;
  bool isPureVirtual = false;
  bool isConst = false;
  bool isLocal = false;      /* LOCAL */
  bool isLocalOnly = false;  /* LOCALONLY */
  bool isKernelOnly = false; /* KERNELONLY */
  bool isInvokeReply = false; /* INVOKEREPLY */
  std::string actionType;    /* TYPE(x) argument, empty if none */
  std::string queueName;     /* QUEUENAME(x) argument */
  bool fromExtends = false;  /* declared in the EXTENDS private class */

  bool isRpc() const { return !isLocalOnly; }
  const Param *targetParam() const {
    for (auto &p : params) if (p.isTarget) return &p;
    return nullptr;
  }
};

struct Class {
  std::string name;
  std::string superName;    /* "IOService" etc., empty for EXTENDS */
  std::string extendsName;  /* EXTENDS(x) target, empty otherwise */
  bool isNative = false;    /* NATIVE: iig owns the kernel class too */
  bool isKernel = false;    /* KERNEL */
  std::vector<Method> methods;
  int firstLine = 0, lastLine = 0;   /* 1-based source range */
};

/* A top-level chunk of the input: either passthrough text or a class. */
struct Chunk {
  enum Kind { Text, ClassDecl } kind = Text;
  std::string text;         /* raw source text of this chunk */
  size_t classIndex = 0;    /* into File::classes when kind == ClassDecl */
};

struct ArrayTypedef {
  std::string elementType;
  int count = 0;
};

struct File {
  std::string path;         /* input path as given */
  std::string basename;     /* "IOHIDDevice.iig" */
  std::vector<Class> classes;
  std::vector<Chunk> chunks;
  /* lines between "@iig implementation" and "@iig end": impl-only text
   * (e.g. extra #includes), emitted into the .iig.cpp, not the header */
  std::string implText;
  std::string frameworkName; /* --framework-name; "" = include by quotes */
  /* "typedef char Foo[N];" declarations seen in this file, e.g.
   * IODispatchQueueName[256], parameters of these types get bounded-string
   * marshaling (embedded fixed buffer + strlcpy/strnlen), not scalar copy. */
  std::map<std::string, int> charArrayTypedefs;
  std::map<std::string, ArrayTypedef> arrayTypedefs;
  std::map<std::string, int> enumConstants;
};

bool parseIigFile(const std::string &path, const std::string &text, File &out,
          std::string &error);

void generateHeader(const File &file, std::string &out);
void generateImpl(const File &file, std::string &out);

/* 64-bit FNV-1a; used for msgids (Apple's hash is unknown, ours is
 * self-consistent, which is all the in-kernel dispatch path needs). */
uint64_t iigHash(const std::string &s);

} /* namespace iig */

#endif /* IIG_AST_H */
