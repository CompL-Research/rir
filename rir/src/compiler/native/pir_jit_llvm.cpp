#include "pir_jit_llvm.h"
#include "compiler/native/builtins.h"
#include "compiler/native/lower_function_llvm.h"
#include "compiler/native/pass_schedule_llvm.h"
#include "compiler/native/types_llvm.h"

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/LLJIT.h"
#include "llvm/ExecutionEngine/Orc/Mangling.h"
#include "llvm/ExecutionEngine/Orc/RTDyldObjectLinkingLayer.h"
#include "llvm/ExecutionEngine/SectionMemoryManager.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/TargetSelect.h"
#include "llvm/Support/raw_os_ostream.h"

namespace rir {
namespace pir {

size_t PirJitLLVM::nModules = 1;
bool PirJitLLVM::initialized = false;

namespace {

llvm::ExitOnError ExitOnErr;
std::unique_ptr<llvm::orc::LLJIT> JIT;
llvm::orc::ThreadSafeContext TSC;

} // namespace

#ifdef PIR_GDB_SUPPORT
void PirJitLLVM::DebugInfo::addCode(Code* c) {
    assert(!codeLoc.count(c));
    codeLoc[c] = line++;
    log << makeName(c) << "\n";
    Visitor::run(c->entry, [&](BB* bb) {
        assert(!BBLoc.count(bb));
        BBLoc[bb] = line++;
        bb->printPrologue(log.out, false);

        for (auto i : *bb) {
            assert(!instLoc.count(i));
            instLoc[i] = line++;
            log << "  ";
            i->print(log.out, false);
            log << "\n";
        }

        if (bb->printEpilogue(log.out, false))
            line++;
    });
    line++;
    log << "\n";
    log.flush();
}

llvm::DIType* PirJitLLVM::DebugInfo::getVoidPtrType(llvm::DIBuilder* builder) {
    if (!VoidPtrType) {
        VoidPtrType = builder->createNullPtrType();
    }
    return VoidPtrType;
}

llvm::DIType* PirJitLLVM::DebugInfo::getSEXPRECType(llvm::DIBuilder* builder) {
    if (!SEXPRECType) {
        // TODO: recursive struct??
        SEXPRECType = builder->createStructType(CU, "SEXPREC", CU->getFile(), 0,
                                                0, 0, llvm::DINode::FlagZero,
                                                nullptr, llvm::DINodeArray());
    }
    return SEXPRECType;
}

llvm::DIType* PirJitLLVM::DebugInfo::getSEXPType(llvm::DIBuilder* builder) {
    if (!SEXPType) {
        auto sexprec = getSEXPRECType(builder);
        SEXPType = builder->createPointerType(sexprec, 64);
    }
    return SEXPType;
}

llvm::DISubroutineType*
PirJitLLVM::DebugInfo::getNativeCodeType(llvm::DIBuilder* builder) {
    if (!NativeCodeType) {
        // NativeCode type is SEXP(Code*, void*, SEXP, SEXP)
        llvm::SmallVector<llvm::Metadata*, 5> EltTys;
        EltTys.push_back(getSEXPType(builder));
        EltTys.push_back(getVoidPtrType(builder));
        EltTys.push_back(getVoidPtrType(builder));
        EltTys.push_back(getSEXPType(builder));
        EltTys.push_back(getSEXPType(builder));

        NativeCodeType = builder->createSubroutineType(
            builder->getOrCreateTypeArray(EltTys));
    }
    return NativeCodeType;
}

llvm::DIType* PirJitLLVM::DebugInfo::getInstrType(llvm::DIBuilder* builder,
                                                  PirType t) {
    std::stringstream ss;
    ss << t;
    return builder->createUnspecifiedType(ss.str());
}

llvm::DIScope* PirJitLLVM::DebugInfo::getScope() {
    return LexicalBlocks.empty() ? CU : LexicalBlocks.back();
}

void PirJitLLVM::DebugInfo::emitLocation(llvm::IRBuilder<>& builder,
                                         size_t line) {
    llvm::DIScope* Scope = getScope();
    builder.SetCurrentDebugLocation(
        llvm::DILocation::get(Scope->getContext(), line, 0, Scope));
}
#endif // PIR_GDB_SUPPORT

#ifdef PIR_GDB_SUPPORT
PirJitLLVM::PirJitLLVM(const std::string& name)
    : DI(name)
#else
PirJitLLVM::PirJitLLVM()
#endif
{
    if (!initialized)
        initializeLLVM();
}

// We have to wait to query LLVM for native code addresses until all Code's
// (including promises) are added to the Module. Hence, in the destructor,
// we need to fixup all the native pointers.
PirJitLLVM::~PirJitLLVM() {
    if (M) {
#ifdef PIR_GDB_SUPPORT
        DIB->finalize(); // should this happen before addIRModule or after?
#endif
        finalizeAndFixup();
        nModules++;
    }
}

void PirJitLLVM::finalizeAndFixup() {
    // TODO: maybe later have TSM from the start and use locking
    //       to allow concurrent compilation?
    auto TSM = llvm::orc::ThreadSafeModule(std::move(M), TSC);
    ExitOnErr(JIT->addIRModule(std::move(TSM)));
    for (auto& fix : jitFixup) {
        auto symbol = ExitOnErr(JIT->lookup(fix.second.second));
        void* native = (void*)symbol.getAddress();
        fix.second.first->nativeCode = (NativeCode)native;
    }
}

void PirJitLLVM::compile(
    rir::Code* target, Code* code, const PromMap& promMap,
    const NeedsRefcountAdjustment& refcount,
    const std::unordered_set<Instruction*>& needsLdVarForUpdate,
    ClosureStreamLogger& log) {

    if (!M.get()) {
        M = std::make_unique<llvm::Module>("", *TSC.getContext());

#ifdef PIR_GDB_SUPPORT
        // Add the current debug info version into the module.
        M->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                         llvm::DEBUG_METADATA_VERSION);

        // Darwin only supports dwarf2.
        if (JIT->getTargetTriple().isOSDarwin())
            M->addModuleFlag(llvm::Module::Warning, "Dwarf Version", 2);

        // Construct the DIBuilder, we do this here because we need the module.
        DIB = std::make_unique<llvm::DIBuilder>(*M);

        // Create the compile unit for the module.
        DI.File = DIB->createFile(DI.FileName, ".");
        DI.CU = DIB->createCompileUnit(llvm::dwarf::DW_LANG_C, DI.File,
                                       "PIR Compiler", false, "", 0);
#endif // PIR_GDB_SUPPORT
    }

#ifdef PIR_GDB_SUPPORT
    DI.addCode(code);
#endif

    std::string mangledName = JIT->mangle(makeName(code));

    LowerFunctionLLVM funCompiler(
        mangledName, code, promMap, refcount, needsLdVarForUpdate,
        // declare
        [&](Code* c, const std::string& name, llvm::FunctionType* signature) {
            assert(!funs.count(c));
            auto f = llvm::Function::Create(
                signature, llvm::Function::ExternalLinkage, name, *M);
            funs[c] = f;
            return f;
        },
        // getModule
        [&]() -> llvm::Module& { return *M; },
        // getFunction
        [&](Code* c) -> llvm::Function* {
            auto r = funs.find(c);
            if (r != funs.end())
                return r->second;
            return nullptr;
        },
        // getBuiltin
        [&](const rir::pir::NativeBuiltin& b) -> llvm::Function* {
            auto l = builtins.find(b.name);
            if (l != builtins.end()) {
                assert(l->second.second == b.fun);
                return l->second.first;
            }

            assert(b.llvmSignature);
            auto f = llvm::Function::Create(
                b.llvmSignature, llvm::Function::ExternalLinkage, b.name, *M);
            for (auto a : b.attrs)
                f->addFnAttr(a);

            builtins[b.name] = {f, b.fun};
            return f;
        }
#ifdef PIR_GDB_SUPPORT
        ,
        &DI, DIB.get()
#endif
    );

#ifdef PIR_GDB_SUPPORT
    llvm::DIScope* FContext = DI.File;
    unsigned ScopeLine = 0;
    llvm::DISubprogram* SP = DIB->createFunction(
        FContext, makeName(code), mangledName, DI.File, DI.getCodeLoc(code),
        DI.getNativeCodeType(DIB.get()), ScopeLine,
        llvm::DINode::FlagPrototyped,
        llvm::DISubprogram::toSPFlags(true /* isLocalToUnit */,
                                      true /* isDefinition */,
                                      false /* isOptimized */));

    funCompiler.fun->setSubprogram(SP);
    DI.LexicalBlocks.push_back(SP);
#endif // PIR_GDB_SUPPORT

    funCompiler.compile();

    llvm::verifyFunction(*funCompiler.fun);
    assert(jitFixup.count(code) == 0);

#ifdef PIR_GDB_SUPPORT
    DI.LexicalBlocks.pop_back();
    DIB->finalizeSubprogram(SP);
#endif

    if (funCompiler.pirTypeFeedback)
        target->pirTypeFeedback(funCompiler.pirTypeFeedback);
    if (funCompiler.hasArgReordering())
        target->arglistOrder(ArglistOrder::New(funCompiler.getArgReordering()));
    // can we use llvm::StringRefs?
    jitFixup.emplace(code,
                     std::make_pair(target, funCompiler.fun->getName().str()));

    log.LLVMBitcode([&](std::ostream& out, bool tty) {
        auto f = funCompiler.fun;
        llvm::raw_os_ostream ro(out);
        f->print(ro, nullptr);
    });
}

llvm::LLVMContext& PirJitLLVM::getContext() { return *TSC.getContext(); }

void PirJitLLVM::initializeLLVM() {
    if (initialized)
        return;

    using namespace llvm;
    using namespace llvm::orc;

    // Initialize LLVM
    InitializeNativeTarget();
    InitializeNativeTargetAsmPrinter();
    InitializeNativeTargetAsmParser();
    ExitOnErr.setBanner("PIR LLVM error: ");

    // Set some TargetMachine options
    auto JTMB = ExitOnErr(JITTargetMachineBuilder::detectHost());
    JTMB.getOptions().EnableMachineOutliner = true;
    JTMB.getOptions().EnableFastISel = true;

    // Create an LLJIT instance with custom TargetMachine builder and
    // ObjectLinkingLayer
    assert(!JIT.get());
    JIT = ExitOnErr(
        LLJITBuilder()
            .setJITTargetMachineBuilder(std::move(JTMB))
#ifdef PIR_GDB_SUPPORT
            .setObjectLinkingLayerCreator(
                [&](ExecutionSession& ES, const Triple& TT) {
                    auto GetMemMgr = []() {
                        return std::make_unique<SectionMemoryManager>();
                    };
                    auto ObjLinkingLayer =
                        std::make_unique<RTDyldObjectLinkingLayer>(
                            ES, std::move(GetMemMgr));

                    // Register the event listener.
                    ObjLinkingLayer->registerJITEventListener(
                        *JITEventListener::createGDBRegistrationListener());

                    // Make sure the debug info sections aren't stripped.
                    ObjLinkingLayer->setProcessAllSections(true);

                    return ObjLinkingLayer;
                })
#endif // PIR_GDB_SUPPORT
            .create());

    // Create one global ThreadSafeContext
    assert(!TSC.getContext());
    TSC = std::make_unique<LLVMContext>();

    // Set what passes to run
    JIT->getIRTransformLayer().setTransform(PassScheduleLLVM());

    // Initialize types specific to PIR and builtins
    initializeTypes(*TSC.getContext());
    NativeBuiltins::initializeBuiltins();

    // Initialize a JITDylib for builtins - these are implemented in C++ and
    // compiled when building Ř, we need to define symbols for them and
    // initialize these to the static addresses of each builtin; they are in
    // a separate dylib because they are shared by all the modules in the
    // main dylib
    auto& builtinsDL = ExitOnErr(JIT->createJITDylib("builtins"));
    JIT->getMainJITDylib().addToLinkOrder(builtinsDL);

    // Build a map of builtin names to the builtins' addresses and populate the
    // builtins dylib
    SymbolMap builtinSymbols(
        static_cast<size_t>(NativeBuiltins::Id::NUM_BUILTINS));
    NativeBuiltins::eachBuiltin([&](const NativeBuiltin& blt) {
        auto res = builtinSymbols.try_emplace(
            JIT->mangleAndIntern(blt.name),
            JITEvaluatedSymbol(pointerToJITTargetAddress(blt.fun),
                               JITSymbolFlags::Exported |
                                   JITSymbolFlags::Callable));
        assert(res.second && "duplicate builtin?");
    });
    ExitOnErr(builtinsDL.define(absoluteSymbols(builtinSymbols)));

    // Add a generator that will look for symbols in the host process.
    // This is added to the builtins dylib so that the builtins have
    // precedence
    builtinsDL.addGenerator(
        ExitOnErr(DynamicLibrarySearchGenerator::GetForCurrentProcess(
            JIT->getDataLayout().getGlobalPrefix(),
            // [](const SymbolStringPtr&) { return true; })));
            [MainName = JIT->mangleAndIntern("main")](
                const SymbolStringPtr& Name) { return Name != MainName; })));

    initialized = true;
}

} // namespace pir
} // namespace rir
