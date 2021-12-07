#ifndef API_H_
#define API_H_

#include "R/r.h"
#include "compiler/log/debug.h"
#include "runtime/Context.h"
#include "runtime/Function.h"

#include <stdint.h>

#define REXPORT extern "C"

extern int R_ENABLE_JIT;

struct FunctionMeta {
  rir::Context c;
  std::string nativeHandle;
  rir::FunctionSignature fs;
  std::vector<rir::BC::PoolIdx> extraPoolIndices;
  std::vector<std::string> existingDefs;
  std::vector<unsigned> promiseSrcEntries;
};

class DeserializerData {
  public:
  static std::unordered_map<int, std::vector<FunctionMeta>> deserializedHastMap;
};

REXPORT SEXP rirInvocationCount(SEXP what);
REXPORT SEXP pirCompileWrapper(SEXP closure, SEXP name, SEXP debugFlags,
                               SEXP debugStyle);
REXPORT SEXP rirCompile(SEXP what, SEXP env);
REXPORT SEXP pirTests();
REXPORT SEXP pirCheck(SEXP f, SEXP check, SEXP env);
REXPORT SEXP pirSetDebugFlags(SEXP debugFlags);
SEXP pirCompile(SEXP closure, const rir::Context& assumptions,
                const std::string& name, const rir::pir::DebugOptions& debug);
extern SEXP rirOptDefaultOpts(SEXP closure, const rir::Context&, SEXP name);
extern SEXP rirOptDefaultOptsDryrun(SEXP closure, const rir::Context&,
                                    SEXP name);

void hash_ast(SEXP ast, int & hast);
void printAST(int space, SEXP ast);
void printAST(int space, int val);

REXPORT SEXP rirSerialize(SEXP data, SEXP file);
REXPORT SEXP rirDeserialize(SEXP file);

REXPORT SEXP rirSetUserContext(SEXP f, SEXP udc);
REXPORT SEXP rirCreateSimpleIntContext();


#endif // API_H_
