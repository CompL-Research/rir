#include "pir_jit_llvm.h"
#include "api.h"
#include "compiler/native/builtins.h"
#include "compiler/native/lower_function_llvm.h"
#include "compiler/native/pass_schedule_llvm.h"
#include "compiler/native/types_llvm.h"
#include "utils/filesystem.h"
#include "R/Funtab.h"

#include "runtime/DispatchTable.h"

#include "llvm/ExecutionEngine/JITSymbol.h"
#include "llvm/ExecutionEngine/Orc/Core.h"
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
#include "llvm/Bitcode/BitcodeReader.h"

#include "patches.h"

#include <memory>

namespace rir {
namespace pir {

std::unique_ptr<llvm::orc::LLJIT> PirJitLLVM::JIT;

size_t PirJitLLVM::nModules = 1;
bool PirJitLLVM::initialized = false;

bool LLVMDebugInfo() {
    return DebugOptions::DefaultDebugOptions.flags.contains(
        DebugFlag::LLVMDebugInfo);
}

namespace {

llvm::ExitOnError ExitOnErr;
llvm::orc::ThreadSafeContext TSC;

std::string dbgFolder;

} // namespace

void PirJitLLVM::DebugInfo::addCode(Code* c) {
    assert(!codeLoc.count(c));
    codeLoc[c] = line++;
    *log << makeName(c) << "\n";
    Visitor::run(c->entry, [&](BB* bb) {
        assert(!BBLoc.count(bb));
        BBLoc[bb] = line++;
        bb->printPrologue(log->out, false);

        for (auto i : *bb) {
            assert(!instLoc.count(i));
            instLoc[i] = line++;
            *log << "  ";
            i->print(log->out, false);
            *log << "\n";
        }

        line++;
        bb->printEpilogue(log->out, false, /* always print newline */ true);
    });
    line++;
    *log << "\n";
    log->flush();
}

void PirJitLLVM::DebugInfo::initializeTypes(llvm::DIBuilder* builder) {

    UnspecifiedType = builder->createUnspecifiedType("unspecified");

    VoidType = nullptr;
    VoidPtrType = builder->createPointerType(VoidType, 64);

    IntType = builder->createBasicType("int", 32, llvm::dwarf::DW_ATE_signed);
    UIntType = builder->createBasicType("unsigned int", 32,
                                        llvm::dwarf::DW_ATE_unsigned);
    DoubleType =
        builder->createBasicType("double", 64, llvm::dwarf::DW_ATE_float);

    {
        uint32_t align = 0;

        auto sxpinfo_structType = builder->createStructType(
            CU, "sxpinfo_struct", File, 0, 64, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 13> sxpinfoElts = {
            builder->createMemberType(
                sxpinfo_structType, "type", File, 0, 5, align, 0,
                llvm::DINode::DIFlags::FlagBitField,
                builder->createTypedef(UIntType, "SEXPTYPE", File, 0, CU)),
            builder->createMemberType(
                sxpinfo_structType, "scalar", File, 0, 1, align, 5,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "obj", File, 0, 1, align, 6,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "alt", File, 0, 1, align, 7,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "gp", File, 0, 16, align, 8,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "mark", File, 0, 1, align, 24,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "debug", File, 0, 1, align, 25,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "trace", File, 0, 1, align, 26,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "spare", File, 0, 1, align, 27,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "gcgen", File, 0, 1, align, 28,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "gccls", File, 0, 3, align, 29,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "named", File, 0, 16, align, 32,
                llvm::DINode::DIFlags::FlagBitField, UIntType),
            builder->createMemberType(
                sxpinfo_structType, "extra", File, 0, 16, align, 48,
                llvm::DINode::DIFlags::FlagBitField, UIntType)};
        sxpinfo_structType->replaceElements(
            builder->getOrCreateArray(sxpinfoElts));

        auto SEXPRECTy = builder->createStructType(
            CU, "SEXPREC", File, 0, 448, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());

        auto SEXPTy = builder->createPointerType(SEXPRECTy, 64);

        auto primsxp_structType = builder->createStructType(
            CU, "primsxp_struct", File, 0, 32, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 1> primsxpElts = {
            builder->createMemberType(primsxp_structType, "offset", File, 0, 32,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      IntType)};
        primsxp_structType->replaceElements(
            builder->getOrCreateArray(primsxpElts));

        auto symsxp_structType = builder->createStructType(
            CU, "symsxp_struct", File, 0, 192, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 3> symsxpElts = {
            builder->createMemberType(symsxp_structType, "pname", File, 0, 64,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(symsxp_structType, "value", File, 0, 64,
                                      align, 64, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(symsxp_structType, "internal", File, 0,
                                      64, align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy)};
        symsxp_structType->replaceElements(
            builder->getOrCreateArray(symsxpElts));

        auto listsxp_structType = builder->createStructType(
            CU, "listsxp_struct", File, 0, 192, 64, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 3> listsxpElts = {
            builder->createMemberType(listsxp_structType, "carval", File, 0, 64,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(listsxp_structType, "cdrval", File, 0, 64,
                                      align, 64, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(listsxp_structType, "tagval", File, 0, 64,
                                      align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy)};
        listsxp_structType->replaceElements(
            builder->getOrCreateArray(listsxpElts));

        auto envsxp_structType = builder->createStructType(
            CU, "envsxp_struct", File, 0, 192, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 3> envsxpElts = {
            builder->createMemberType(envsxp_structType, "frame", File, 0, 64,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(envsxp_structType, "enclos", File, 0, 64,
                                      align, 64, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(envsxp_structType, "hashtab", File, 0, 64,
                                      align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy)};
        envsxp_structType->replaceElements(
            builder->getOrCreateArray(envsxpElts));

        auto closxp_structType = builder->createStructType(
            CU, "closxp_struct", File, 0, 192, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 3> closxpElts = {
            builder->createMemberType(closxp_structType, "formals", File, 0, 64,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(closxp_structType, "body", File, 0, 64,
                                      align, 64, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(closxp_structType, "env", File, 0, 64,
                                      align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy)};
        closxp_structType->replaceElements(
            builder->getOrCreateArray(closxpElts));

        auto promsxp_structType = builder->createStructType(
            CU, "promsxp_struct", File, 0, 192, align, (llvm::DINode::DIFlags)0,
            nullptr, llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 3> promsxpElts = {
            builder->createMemberType(promsxp_structType, "value", File, 0, 64,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(promsxp_structType, "expr", File, 0, 64,
                                      align, 64, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(promsxp_structType, "env", File, 0, 64,
                                      align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy)};
        promsxp_structType->replaceElements(
            builder->getOrCreateArray(promsxpElts));

        auto SEXPRECUnionTy = builder->createUnionType(
            SEXPRECTy, "", File, 0, 192, align, (llvm::DINode::DIFlags)0,
            llvm::DINodeArray());
        llvm::SmallVector<llvm::Metadata*, 6> SEXPRECUnionElts = {
            builder->createMemberType(SEXPRECUnionTy, "primsxp", File, 0, 32,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      primsxp_structType),
            builder->createMemberType(SEXPRECUnionTy, "symsxp", File, 0, 192,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      symsxp_structType),
            builder->createMemberType(SEXPRECUnionTy, "listsxp", File, 0, 192,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      listsxp_structType),
            builder->createMemberType(SEXPRECUnionTy, "envsxp", File, 0, 192,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      envsxp_structType),
            builder->createMemberType(SEXPRECUnionTy, "closxp", File, 0, 192,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      closxp_structType),
            builder->createMemberType(SEXPRECUnionTy, "promsxp", File, 0, 192,
                                      align, 0, (llvm::DINode::DIFlags)0,
                                      promsxp_structType)};
        SEXPRECUnionTy->replaceElements(
            builder->getOrCreateArray(SEXPRECUnionElts));

        llvm::SmallVector<llvm::Metadata*, 5> SEXPRECElts = {
            builder->createMemberType(SEXPRECTy, "sxpinfo", File, 0, 64, align,
                                      0, (llvm::DINode::DIFlags)0,
                                      sxpinfo_structType),
            builder->createMemberType(SEXPRECTy, "attrib", File, 0, 64, align,
                                      64, (llvm::DINode::DIFlags)0, SEXPTy),
            builder->createMemberType(SEXPRECTy, "gengc_next_node", File, 0, 64,
                                      align, 128, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(SEXPRECTy, "gengc_prev_node", File, 0, 64,
                                      align, 192, (llvm::DINode::DIFlags)0,
                                      SEXPTy),
            builder->createMemberType(SEXPRECTy, "u", File, 0, 192, align, 256,
                                      (llvm::DINode::DIFlags)0,
                                      SEXPRECUnionTy)};
        SEXPRECTy->replaceElements(builder->getOrCreateArray(SEXPRECElts));

        SEXPRECType = SEXPRECTy;
        SEXPType = builder->createTypedef(SEXPTy, "SEXP", File, 0, CU);
    }

    {
        // NativeCode type is SEXP(Code*, void*, SEXP, SEXP)
        llvm::SmallVector<llvm::Metadata*, 5> EltTys = {
            SEXPType, VoidPtrType, VoidPtrType, SEXPType, SEXPType};
        NativeCodeType = builder->createSubroutineType(
            builder->getOrCreateTypeArray(EltTys));
    }
}

llvm::DIScope* PirJitLLVM::DebugInfo::getScope() {
    return LexicalBlocks.empty() ? CU : LexicalBlocks.back();
}

void PirJitLLVM::DebugInfo::emitLocation(llvm::IRBuilder<>& builder,
                                         size_t line) {
    size_t col = 1;
    llvm::DIScope* Scope = getScope();
    builder.SetCurrentDebugLocation(
        llvm::DILocation::get(Scope->getContext(), line, col, Scope));
}

void PirJitLLVM::DebugInfo::clearLocation(llvm::IRBuilder<>& builder) {
    builder.SetCurrentDebugLocation(llvm::DebugLoc());
}

PirJitLLVM::PirJitLLVM(const std::string& name) : name(name) {
    if (!initialized)
        initializeLLVM();
}

// We have to wait to query LLVM for native code addresses until all Code's
// (including promises) are added to the Module. Hence, in the destructor,
// we need to fixup all the native pointers.
PirJitLLVM::~PirJitLLVM() {
    if (M) {
        // Should this happen before finalizeAndFixup or after?
        if (LLVMDebugInfo()) {
            DIB->finalize();
        }
        finalizeAndFixup();
        nModules++;
    }
}

void PirJitLLVM::finalizeAndFixup() {
    // TODO: maybe later have TSM from the start and use locking
    //       to allow concurrent compilation?
    auto TSM = llvm::orc::ThreadSafeModule(std::move(M), TSC);
    ExitOnErr(JIT->addIRModule(std::move(TSM)));
    for (auto& fix : jitFixup)
        fix.second.first->lazyCodeHandle(fix.second.second.str());
}

void PirJitLLVM::deserializeAndAddModule(std::string bcPath, std::string poolPath, std::vector<BC::PoolIdx> & bcIndices, size_t cPoolEntriesSize, size_t srcPoolEntriesSize, size_t ePoolEntriesSize, std::vector<std::string> & existingDefs, std::vector<unsigned> & promiseSrcEntries) {
    llvm::ErrorOr<std::unique_ptr<llvm::MemoryBuffer>> mb = llvm::MemoryBuffer::getFile(bcPath);
    rir::pir::PirJitLLVM jit("f");


    llvm::Expected<std::unique_ptr<llvm::Module>> llModuleHolder = llvm::parseBitcodeFile(mb->get()->getMemBufferRef(), jit.getContext());

    if (std::error_code ec = errorToErrorCode(llModuleHolder.takeError())) {
        std::stringstream errMsg;
        errMsg << "Error reading module from bitcode : " << bcPath << std::endl;
    }

    FILE *reader;
    reader = fopen(poolPath.c_str(),"r");

    R_inpstream_st inputStream;
    R_InitFileInPStream(&inputStream, reader, R_pstream_binary_format, NULL, R_NilValue);

    SEXP result = R_Unserialize(&inputStream);
    size_t totalEntriesInSerializedPool = (size_t)Rf_length(result);

    std::unordered_map<int64_t, int64_t> poolPatch;
    std::unordered_map<int64_t, int64_t> sPoolPatch;

    size_t streamIndex = 0;

    size_t cpIndex = 0;
    size_t srcIndex = 0;

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    int pIndex = 0;
    #endif



    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << "ConstantPool: [ ";
    #endif

    while (streamIndex < cPoolEntriesSize) {
        auto ele = VECTOR_ELT(result, streamIndex);

        if (TYPEOF(ele) == CLOSXP) {
            int h = 0;
            hash_ast(BODY(ele), h);

            // If closure with the same hast has been added to the CP already, just use that index
            if (rir::Code::cpHastPatch.count(h)) {
                poolPatch[streamIndex] = rir::Code::cpHastPatch[h];

                #if DESERIALIZED_PRINT_POOL_PATCHES == 1
                std::cout << "{ " << streamIndex << " to " << rir::Code::cpHastPatch[h] << ", TYPE: " << TYPEOF(ele) << " } ";
                #endif
            } else {
                // copool_x streamIndex ---becomes---> runtime_constant_pool_index
                auto runtimeCpIndex = Pool::insert(ele);
                poolPatch[streamIndex] = runtimeCpIndex;

                #if DESERIALIZED_PRINT_POOL_PATCHES == 1
                std::cout << "{ " << streamIndex << " to " << runtimeCpIndex << ", TYPE: " << TYPEOF(ele) << " } ";
                #endif

                // Store this in a map, so that we can prevent duplicate insertions into the pool
                rir::Code::cpHastPatch[h] = runtimeCpIndex;
            }

        } else {
            // copool_x streamIndex ---becomes---> runtime_constant_pool_index
            auto runtimeCpIndex = Pool::insert(ele);
            poolPatch[streamIndex] = runtimeCpIndex;

            #if DESERIALIZED_PRINT_POOL_PATCHES == 1
            std::cout << "{ " << streamIndex << " to " << runtimeCpIndex << ", TYPE: " << TYPEOF(ele) << " } ";
            #endif

        }
        streamIndex++;
        cpIndex++;
    }

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << " ]" << std::endl;
    #endif

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << "SourcePool: [ ";
    #endif

    while (streamIndex < cPoolEntriesSize + srcPoolEntriesSize) {
        auto ele = VECTOR_ELT(result, streamIndex);
        auto patchedIndex = src_pool_add(globalContext(), ele);

        #if DESERIALIZED_PRINT_POOL_PATCHES == 1
        std::cout << "{ " << srcIndex << " to " << patchedIndex << " from " << streamIndex << ", TYPE: " << TYPEOF(ele) << " } ";
        #endif

        sPoolPatch[srcIndex] = patchedIndex;

        streamIndex++;
        srcIndex++;

    }

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << " ]" << std::endl;
    #endif

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << "ExtraPool: [ ";
    #endif

    while (streamIndex < cPoolEntriesSize + srcPoolEntriesSize + ePoolEntriesSize) {
        auto ele = VECTOR_ELT(result, streamIndex);
        auto patchedIndex = Pool::insert(ele);

        poolPatch[cpIndex] = patchedIndex;

        #if DESERIALIZED_PRINT_POOL_PATCHES == 1
        std::cout << "{ " << cpIndex << " to " << patchedIndex << " from " << streamIndex << ", TYPE: " << TYPEOF(ele) << " } ";
        #endif

        bcIndices.push_back(patchedIndex);

        streamIndex++;
        cpIndex++;
    }

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << " ]" << std::endl;
    #endif

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << "Promise Src Entries: [ ";
    #endif

    while (streamIndex  < totalEntriesInSerializedPool) {
        auto ele = VECTOR_ELT(result, streamIndex);
        auto patchedIndex = src_pool_add(globalContext(), ele);
        promiseSrcEntries.push_back(patchedIndex);
        #if DESERIALIZED_PRINT_POOL_PATCHES == 1
        std::cout << "{ " << pIndex++ << " to " << patchedIndex << " from " << streamIndex << ", TYPE: " << TYPEOF(ele) << " } ";
        #endif

        streamIndex++;
    }

    #if DESERIALIZED_PRINT_POOL_PATCHES == 1
    std::cout << " ]" << std::endl;
    #endif


    // for (auto & ele : poolPatch) {
    //     std::cout << "Patching " << ele.first << " : " << ele.second << std::endl;
    // }

    #if API_PRINT_DESERIALIZED_MODULE_BEFORE_PATCH == 1
    llvm::raw_os_ostream dbg1(std::cout);
    dbg1 << *llModuleHolder.get();
    #endif

    fclose(reader);

    for (auto & fun : llModuleHolder.get()->getFunctionList()) {
        existingDefs.push_back(fun.getName().str());
    }

    for (auto & global : llModuleHolder.get()->getGlobalList()) {
        auto pre = global.getName().str().substr(0,6) == "copool";
        auto srp = global.getName().str().substr(0,6) == "srpool";
        if (pre) {
            auto con = global.getInitializer();

            if (auto * v = llvm::dyn_cast<llvm::ConstantDataArray>(con)) {
                std::vector<llvm::Constant*> patchedIndices;

                auto arrSize = v->getNumElements();

                for (unsigned int i = 0; i < arrSize; i++) {
                    auto val = v->getElementAsAPInt(i).getSExtValue();

                    // Offset relative to the serialized pool
                    llvm::Constant* replacementValue = llvm::ConstantInt::get(rir::pir::PirJitLLVM::getContext(), llvm::APInt(32, poolPatch[val]));
                    patchedIndices.push_back(replacementValue);

                }

                auto ty = llvm::ArrayType::get(rir::pir::t::Int, patchedIndices.size());
                auto newInit = llvm::ConstantArray::get(ty, patchedIndices);

                global.setInitializer(newInit);
            } else if (auto * v = llvm::dyn_cast<llvm::ConstantInt>(con)) {
                auto val = v->getSExtValue();
                // Offset relative to the serialized pool
                llvm::Constant* replacementValue = llvm::ConstantInt::get(rir::pir::PirJitLLVM::getContext(), llvm::APInt(32, poolPatch[val]));

                global.setInitializer(replacementValue);
            } else  if (auto * v = llvm::dyn_cast<llvm::ConstantAggregateZero>(con)) {
                std::vector<llvm::Constant*> patchedIndices;

                auto arrSize = v->getNumElements();

                for (unsigned int i = 0; i < arrSize; i++) {
                    auto val = llvm::APInt().getSExtValue();

                    // Offset relative to the serialized pool
                    llvm::Constant* replacementValue = llvm::ConstantInt::get(rir::pir::PirJitLLVM::getContext(), llvm::APInt(32, poolPatch[val]));
                    patchedIndices.push_back(replacementValue);

                }

                auto ty = llvm::ArrayType::get(rir::pir::t::Int, patchedIndices.size());
                auto newInit = llvm::ConstantArray::get(ty, patchedIndices);

                global.setInitializer(newInit);
            } else {
                if (!llvm::dyn_cast<llvm::ConstantStruct>(con)) {
                    llvm::raw_os_ostream os(std::cout);
                    global.getType()->print(os);
                    std::cout << global.getName().str() << " -> Unknown Type " << std::endl;
                }
            }
        }

        // All src pool references have a srpool prefix
        if (srp) {
            auto con = global.getInitializer();
            if (auto * v = llvm::dyn_cast<llvm::ConstantInt>(con)) {
                auto val = v->getSExtValue();
                // Offset relative to the serialized pool
                llvm::Constant* replacementValue = llvm::ConstantInt::get(rir::pir::PirJitLLVM::getContext(), llvm::APInt(32, sPoolPatch[val]));

                global.setInitializer(replacementValue);
            }
        }
    }

    #if API_PRINT_DESERIALIZED_MODULE_AFTER_PATCH == 1
    llvm::raw_os_ostream dbg2(std::cout);
    dbg2 << *llModuleHolder.get();
    #endif

    auto TSM = llvm::orc::ThreadSafeModule(std::move(llModuleHolder.get()), TSC);
    ExitOnErr(JIT->addIRModule(std::move(TSM)));

}

void PirJitLLVM::compile(
    rir::Code* target, ClosureVersion* closure, Code* code,
    const PromMap& promMap, const NeedsRefcountAdjustment& refcount,
    const std::unordered_set<Instruction*>& needsLdVarForUpdate,
    ClosureStreamLogger& log) {

    if (!M.get()) {
        M = std::make_unique<llvm::Module>("", *TSC.getContext());

        if (LLVMDebugInfo()) {

            DI = std::make_unique<DebugInfo>(dbgFolder, name);
            DIB = std::make_unique<llvm::DIBuilder>(*M);

            // Create a file stream log for this module
            DI->log = std::make_unique<FileLogStream>(DI->Folder + "/" +
                                                      DI->FileName);

            // Create the compile unit for the module.
            DI->File = DIB->createFile(DI->FileName, DI->Folder);
            DI->CU = DIB->createCompileUnit(llvm::dwarf::DW_LANG_C, DI->File,
                                            "PIR Compiler", false, "", 0);

            DI->initializeTypes(DIB.get());

            // Darwin only supports dwarf2.
            M->addModuleFlag(llvm::Module::Warning, "Dwarf Version",
                             JIT->getTargetTriple().isOSDarwin()
                                 ? 2
                                 : llvm::dwarf::DWARF_VERSION);

            // Add the current debug info version into the module.
            M->addModuleFlag(llvm::Module::Warning, "Debug Info Version",
                             llvm::DEBUG_METADATA_VERSION);
        }
    }

    if (LLVMDebugInfo()) {
        DI->addCode(code);
    }

    std::string mangledName = JIT->mangle(makeName(code));

    LowerFunctionLLVM funCompiler(
        target, mangledName, closure, code, promMap, refcount,
        needsLdVarForUpdate,
        // declare
        [&](Code* c, const std::string& name, llvm::FunctionType* signature) {
            assert(!funs.count(c));
            auto f = llvm::Function::Create(
                signature, llvm::Function::ExternalLinkage, name, *M);
            if (LLVMDebugInfo()) {
                llvm::AttrBuilder ab;
                ab.addAttribute(llvm::Attribute::get(*TSC.getContext(),
                                                     "frame-pointer", "all"));
                ab.addAttribute(llvm::Attribute::NoInline);
                ab.addAttribute(llvm::Attribute::NoMerge);
                ab.addAttribute(llvm::Attribute::NoRedZone);
                // ab.addAttribute(llvm::Attribute::OptimizeNone);
                ab.addAttribute(llvm::Attribute::UWTable);
                f->setAttributes(
                    llvm::AttributeList::get(*TSC.getContext(), ~0U, ab));
            }
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
        DI.get(), DIB.get());

    llvm::DISubprogram* SP = nullptr;
    if (LLVMDebugInfo()) {
        llvm::DIScope* FContext = DI->File;
        unsigned ScopeLine = 0;
        SP = DIB->createFunction(
            FContext, makeName(code), mangledName, DI->File,
            DI->getCodeLoc(code), DI->NativeCodeType, ScopeLine,
            llvm::DINode::FlagPrototyped,
            llvm::DISubprogram::toSPFlags(true /* isLocalToUnit */,
                                          true /* isDefinition */,
                                          false /* isOptimized */));

        funCompiler.fun->setSubprogram(SP);
        DI->LexicalBlocks.push_back(SP);
    }

    funCompiler.compile();

    assert(jitFixup.count(code) == 0);

    if (LLVMDebugInfo()) {
        DI->LexicalBlocks.pop_back();
        DIB->finalizeSubprogram(SP);
    }

#ifndef NDEBUG
    if (llvm::verifyFunction(*funCompiler.fun, &llvm::errs())) {
        assert(false &&
               "Error in llvm::verifyFunction() called from pir_jit_llvm.cpp");
    }
#endif

    if (funCompiler.pirTypeFeedback)
        target->pirTypeFeedback(funCompiler.pirTypeFeedback);
    if (funCompiler.hasArgReordering())
        target->arglistOrder(ArglistOrder::New(funCompiler.getArgReordering()));
    jitFixup.emplace(code, std::make_pair(target, funCompiler.fun->getName()));

    log.LLVMBitcode([&](std::ostream& out, bool tty) {
        bool debug = true;
        llvm::raw_os_ostream ro(out);
        if (debug) {
            // For debugging, print the whole module to see the debuginfo
            // Also comment out insn_assert in lower_function_llvm.cpp to get
            // smaller listings...
            ro << *M;
        } else {
            funCompiler.fun->print(ro, nullptr);
        }
    });
}

llvm::LLVMContext& PirJitLLVM::getContext() { return *TSC.getContext(); }

void PirJitLLVM::initializeLLVM() {
    static int opaqueTrue = 1;
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
            .setObjectLinkingLayerCreator(
                [&](ExecutionSession& ES, const Triple& TT) {
                    auto GetMemMgr = []() {
                        return std::make_unique<SectionMemoryManager>();
                    };
                    auto ObjLinkingLayer =
                        std::make_unique<RTDyldObjectLinkingLayer>(
                            ES, std::move(GetMemMgr));

                    if (LLVMDebugInfo()) {
                        // Register the event debug listeners for gdb and perf.
                        ObjLinkingLayer->registerJITEventListener(
                            *JITEventListener::createGDBRegistrationListener());
#ifdef PIR_USE_PERF
                        ObjLinkingLayer->registerJITEventListener(
                            *JITEventListener::createPerfJITEventListener());
#endif

                        // Make sure the debug info sections aren't stripped.
                        ObjLinkingLayer->setProcessAllSections(true);
                    }

                    return ObjLinkingLayer;
                })
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
            [MainName = JIT->mangleAndIntern("main")](
                const SymbolStringPtr& Name) { return Name != MainName; })));

    // TODO this is a bit of a hack but it works: the address is stored in the
    // name. symbols starting with "ept_" are external pointers, the ones
    // starting with "efn_" are external function pointers. these must exist in
    // the host process.
    class ExtSymbolGenerator : public llvm::orc::DefinitionGenerator {
      public:
        Error tryToGenerate(LookupState& LS, LookupKind K, JITDylib& JD,
                            JITDylibLookupFlags JDLookupFlags,
                            const SymbolLookupSet& LookupSet) override {
            orc::SymbolMap NewSymbols;
            for (auto s : LookupSet) {
                auto& Name = s.first;
                auto n = (*Name).str();
                auto ept = n.substr(0, 4) == "ept_";
                auto efn = n.substr(0, 4) == "efn_";

                auto dcs = n.substr(0, 4) == "dcs_"; // Direct constant symbols
                auto sym = n.substr(0, 4) == "sym_"; // Symbols lookups
                auto gcb = n.substr(0, 4) == "gcb_"; // Builtins

                auto spe = n.substr(0, 4) == "spe_"; // Special symbols

                auto msg = n.substr(0, 4) == "msg_"; // Message ptr

                auto real = n.substr(0, 7) ==  "cpreal_"; // constant pool real
                // auto lang = n.substr(0, 4) ==  "lan_"; // constant pool langsxp

                auto gcode = n.substr(0, 4) == "cod_"; // callable pointer to builtin

                auto hast = n.substr(0, 5) == "hast_"; // replace this symbol with the start of the corresponding Code *

                auto epe = n.substr(0, 4) == "epe_"; // extra pool entry

                // auto base = n.substr(0, 5) == "base_"; // baseLibraryEntry

                auto spef = n.substr(0, 5) == "spef_"; // specialsxp function

                if (ept || efn) {
                    auto addrStr = n.substr(4);
                    auto addr = std::strtoul(addrStr.c_str(), nullptr, 16);
                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(addr)),
                        JITSymbolFlags::Exported |
                            (efn ? JITSymbolFlags::Callable
                                 : JITSymbolFlags::None));
                } else if (dcs) {
                    auto id = std::stoi(n.substr(4));
                    SEXP ptr = R_NilValue;
                    switch (id) {
                        case 100:
                            ptr = R_GlobalEnv;
                            break;
                        case 101:
                            ptr = R_BaseEnv;
                            break;
                        case 102:
                            ptr = R_BaseNamespace;
                            break;
                        case 103:
                            ptr = R_TrueValue;
                            break;
                        case 104:
                            ptr = R_NilValue;
                            break;
                        case 105:
                            ptr = R_FalseValue;
                            break;
                        case 106:
                            ptr = R_UnboundValue;
                            break;
                        case 107:
                            ptr = R_MissingArg;
                            break;
                        case 108:
                            ptr = R_LogicalNAValue;
                            break;
                        case 109:
                            ptr = R_EmptyEnv;
                            break;
                        case 110:
                            ptr = R_RestartToken;
                            break;
                        case 111:
                            ptr = R_DimSymbol;
                            break;
                    }
                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(ptr)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));
                } else if (sym) {
                    auto constantName = n.substr(4);
                    SEXP con = Rf_install(constantName.c_str());
                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(con)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));
                } else if (gcb) {
                    auto id = std::stoi(n.substr(4));
                    SEXP ptr;
                    assert(R_FunTab[id].eval % 10 == 1 && "Only use for BUILTINSXP");
                    if (R_FunTab[id].eval % 100 / 10 == 0)
                        ptr = Rf_install(R_FunTab[id].name)->u.symsxp.value;
                    else
                        ptr = Rf_install(R_FunTab[id].name)->u.symsxp.internal;

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(ptr)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));
                } else if (spe) {
                    auto constantName = n.substr(4);
                    uintptr_t addr = 0;

                    if (constantName.compare("BCNodeStackTop") == 0) {
                        addr = reinterpret_cast<uintptr_t>(&R_BCNodeStackTop);
                    } else if (constantName.compare("Visible") == 0) {
                        addr = reinterpret_cast<uintptr_t>(&R_Visible);
                    } else if (constantName.compare("opaqueTrue") == 0) {
                        addr = reinterpret_cast<uintptr_t>(&opaqueTrue);
                    } else if (constantName.compare("constantPool") == 0) {
                        addr = reinterpret_cast<uintptr_t>(globalContext());
                    } else if (constantName.compare("returnedValue") == 0) {
                        addr = reinterpret_cast<uintptr_t>(&R_ReturnedValue);
                    }

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(addr),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));
                } else if (msg) {
                    size_t msgSize = strlen(n.substr(4).c_str());

                    char * p = (char *) malloc( sizeof(char) * msgSize );

                    for (size_t i = 0; i < msgSize; i++) {
                        p[i] = n.substr(4).c_str()[i];
                    }

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(reinterpret_cast<uintptr_t>(p)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));

                } else if (real) {
                    auto real_num = n.substr(7);
                    double real = std::stod(real_num);
                    SEXP ptr;
                    PROTECT(ptr = Rf_ScalarReal(real));

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(reinterpret_cast<uintptr_t>(ptr)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));

                }
                else

                // if (lang) {
                //     auto hastAndPath = n.substr(4);
                //     int start = 0;
                //     int end = hastAndPath.find("_");

                //     auto hast = std::stoi(hastAndPath.substr(start, end - start));
                //     start = end + 1;
                //     end = hastAndPath.find("_", start);
                //     auto path = std::stoi(hastAndPath.substr(start, end - start));

                //     auto found_ast = rir::Code::hastMap[hast];

                //     SEXP el = VECTOR_ELT(globalContext()->src.list, found_ast->src);

                //     NewSymbols[Name] = JITEvaluatedSymbol(
                //         static_cast<JITTargetAddress>(reinterpret_cast<uintptr_t>(get_from_path(path, el))),
                //         JITSymbolFlags::Exported | (JITSymbolFlags::None));


                // } else

                if (gcode) {
                    auto id = std::stoi(n.substr(4));
                    SEXP ptr;
                    assert(R_FunTab[id].eval % 10 == 1 && "Only use for BUILTINSXP");
                    if (R_FunTab[id].eval % 100 / 10 == 0)
                        ptr = Rf_install(R_FunTab[id].name)->u.symsxp.value;
                    else
                        ptr = Rf_install(R_FunTab[id].name)->u.symsxp.internal;

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(getBuiltin(ptr))),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));

                } else if (hast) {
                    auto id = std::stoi(n.substr(5));
                    if (rir::Code::hastMap.count(id) == 0) {
                        std::cout << "hast symbol not found: " << id << std::endl;
                    }
                    auto addr = ((rir::DispatchTable *)rir::Code::hastMap[id])->baseline()->body();
                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(addr)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));
                } else if (epe) {
                    auto firstDel = n.find('_');
                    auto secondDel = n.find('_', firstDel + 1);
                    auto thirdDel = n.find('_', secondDel + 1);

                    auto hast = std::stoi(n.substr(firstDel + 1, secondDel - firstDel - 1));
                    auto extraPoolOffset = std::stoi(n.substr(secondDel + 1, thirdDel - secondDel - 1));
                    auto context = std::stoul(n.substr(thirdDel + 1));

                    Context c(context);

                    if (rir::Code::hastMap.count(hast) == 0) {
                        std::cout << "hast symbol not found: " << hast << std::endl;
                    }

                    rir::DispatchTable * dtable = ((rir::DispatchTable *)rir::Code::hastMap[hast]);

                    rir::Code * code = rir::Code::New(0);

                    for (size_t i = 1; i < dtable->size(); ++i) {
                        auto e = dtable->get(i);
                        if (e->context() == c) {
                            code = e->body();
                        }
                    }

                    if (code) {
                        auto res = DATAPTR(code->getExtraPoolEntry(extraPoolOffset));

                        NewSymbols[Name] = JITEvaluatedSymbol(
                            static_cast<JITTargetAddress>(
                                reinterpret_cast<uintptr_t>(res)),
                            JITSymbolFlags::Exported | (JITSymbolFlags::None));
                    }

                }
                // else if (base) {
                //     auto firstDel = n.find('_');
                //     auto secondDel = n.find('_', firstDel + 1);

                //     auto baseIndex = std::stoi(n.substr(firstDel + 1, secondDel - firstDel - 1));
                //     auto funName = BaseLibs::libBaseName.at(baseIndex);
                //     auto sym = Rf_install(funName.c_str());
                //     auto fun = Rf_findFun(sym, R_GlobalEnv);

                //     NewSymbols[Name] = JITEvaluatedSymbol(
                //         static_cast<JITTargetAddress>(
                //             reinterpret_cast<uintptr_t>(fun)),
                //         JITSymbolFlags::Exported | (JITSymbolFlags::None));


                // }

                else if (spef) {
                    auto firstDel = n.find('_');
                    auto secondDel = n.find('_', firstDel + 1);

                    auto index = std::stoi(n.substr(firstDel + 1, secondDel - firstDel - 1));
                    auto sym = Rf_install(R_FunTab[index].name);
                    // std::cout << "patching spef" << std::endl;
                    // printAST(0,sym);
                    auto fun = Rf_findFun(sym,R_GlobalEnv);

                    NewSymbols[Name] = JITEvaluatedSymbol(
                        static_cast<JITTargetAddress>(
                            reinterpret_cast<uintptr_t>(fun)),
                        JITSymbolFlags::Exported | (JITSymbolFlags::None));

                } else {
                    std::cout << "unknown symbol " << n << "\n";
                }
            }
            if (NewSymbols.empty())
                return Error::success();

            return JD.define(absoluteSymbols(std::move(NewSymbols)));
        };
    };

    builtinsDL.addGenerator(std::make_unique<ExtSymbolGenerator>());

    if (LLVMDebugInfo()) {
        if (getenv("PIR_GDB_FOLDER")) {
            dbgFolder = getenv("PIR_GDB_FOLDER");
            clearOrCreateDirectory(dbgFolder.c_str());
        } else {
            dbgFolder = createTmpDirectory();
            std::ofstream of("./PIR_GDB_FOLDER");
            of << dbgFolder << "\n";
        }
    }

    initialized = true;
}

} // namespace pir
} // namespace rir
