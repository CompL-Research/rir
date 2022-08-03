#ifndef RIR_DISPATCH_TABLE_H
#define RIR_DISPATCH_TABLE_H

#include "Function.h"
#include "R/Serialize.h"
#include "RirRuntimeObject.h"
#include "utils/random.h"
#include "utils/serializerData.h"

#include "compiler/pir/module.h"
#include "compiler/log/stream_logger.h"
#include "compiler/compiler.h"
#include "compiler/backend.h"
#include "utils/BitcodeLinkUtility.h"

#include "R/Protect.h"

#include "runtime/L2Dispatch.h"
namespace rir {

#define DISPATCH_TABLE_MAGIC (unsigned)0xd7ab1e00

typedef SEXP DispatchTableEntry;

/*
 * A dispatch table (vtable) for functions.
 *
 */
#pragma pack(push)
#pragma pack(1)
struct DispatchTable
    : public RirRuntimeObject<DispatchTable, DISPATCH_TABLE_MAGIC> {

    size_t size() const { return size_; }

    Function* get(size_t i) const {
        assert(i < capacity());

        // If there exists a L2 dispatch table at this index,
        // then check if there is a possible dispatch available
        SEXP funContainer = getEntry(i);

        if (L2Dispatch::check(funContainer)) {
            L2Dispatch * l2vt = L2Dispatch::unpack(funContainer);
            return l2vt->dispatch();
        }
        return Function::unpack(getEntry(i));
    }

    Function* best() const {
        if (size() > 1)
            return get(1);
        return get(0);
    }
    Function* baseline() const {
        auto f = Function::unpack(getEntry(0));
        assert(f->signature().envCreation ==
               FunctionSignature::Environment::CallerProvided);
        return f;
    }

    Function* dispatch(Context a) const {
        if (!a.smaller(userDefinedContext_)) {
#ifdef DEBUG_DISPATCH
            std::cout << "DISPATCH trying: " << a
                      << " vs annotation: " << userDefinedContext_ << "\n";
#endif
            Rf_error("Provided context does not satisfy user defined context");
        }

        for (size_t i = 1; i < size(); ++i) {
#ifdef DEBUG_DISPATCH
            std::cout << "DISPATCH trying: " << a << " vs " << get(i)->context()
                      << "\n";
#endif
            auto e = get(i);
            if (a.smaller(e->context()) && !e->disabled())
                return e;
        }
        return baseline();
    }

    void baseline(Function* f) {
        assert(f->signature().optimization ==
               FunctionSignature::OptimizationLevel::Baseline);
        if (size() == 0)
            size_++;
        else
            assert(baseline()->signature().optimization ==
                   FunctionSignature::OptimizationLevel::Baseline);
        setEntry(0, f->container());
    }

    bool contains(const Context& assumptions) const {
        for (size_t i = 0; i < size(); ++i)
            if (get(i)->context() == assumptions)
                return !get(i)->disabled();
        return false;
    }

    void remove(Code* funCode) {
        size_t i = 1;
        for (; i < size(); ++i) {
            if (get(i)->body() == funCode)
                break;
        }
        if (i == size())
            return;
        for (; i < size() - 1; ++i) {
            setEntry(i, getEntry(i + 1));
        }
        setEntry(i, nullptr);
        size_--;
    }

    void tryLinking(SEXP currHastSym, const unsigned long & con, const int & nargs);

    void insert(Function* fun) {
        assert(fun->signature().optimization !=
               FunctionSignature::OptimizationLevel::Baseline);
        int idx = negotiateSlot(fun->context());
        SEXP idxContainer = getEntry(idx);

        if (idxContainer == R_NilValue) {
            setEntry(idx, fun->container());
            return;
        } else {
            if (Function::check(idxContainer)) {
                // Already existing container, do what is meant to be done
                if (idx != 0) {
                    // Remember deopt counts across recompilation to avoid
                    // deopt loops
                    Function * old = Function::unpack(idxContainer);
                    fun->addDeoptCount(old->deoptCount());
                    setEntry(idx, fun->container());
                    assert(get(idx) == fun);
                }
            } else if (L2Dispatch::check(idxContainer)) {
                L2Dispatch * l2vt = L2Dispatch::unpack(idxContainer);
                l2vt->insert(fun);
            } else {
                Rf_error("Dispatch table insertion error, corrupted slot!!");
            }
        }

        if (hast) {
            tryLinking(hast, fun->context().toI(), fun->signature().numArguments);
        }
    }

    void insertL2(Function* fun) {
        assert(fun->signature().optimization !=
               FunctionSignature::OptimizationLevel::Baseline);
        int idx = negotiateSlot(fun->context());

        SEXP idxContainer = getEntry(idx);

        if (idxContainer == R_NilValue) {
            Protect p;
            L2Dispatch * l2vt = L2Dispatch::create(fun, p);
            setEntry(idx, l2vt->container());
        } else {
            if (Function::check(idxContainer)) {
                Protect p;
                Function * old = Function::unpack(idxContainer);
                L2Dispatch * l2vt = L2Dispatch::create(old, p);
                setEntry(idx, l2vt->container());
                l2vt->insert(fun);
            } else if (L2Dispatch::check(idxContainer)) {
                L2Dispatch * l2vt = L2Dispatch::unpack(idxContainer);
                l2vt->insert(fun);
            } else {
                Rf_error("Dispatch table L2insertion error, corrupted slot!!");
            }
        }

        if (hast) {
            tryLinking(hast, fun->context().toI(), fun->signature().numArguments);
        }
    }

    // Function slot negotiation
    int negotiateSlot(const Context& assumptions) {
        assert(size() > 0);
        size_t i;
        for (i = size() - 1; i > 0; --i) {
            auto old = get(i);
            if (old->context() == assumptions) {
                // We already gave this context, dont delete it, just return the index
                return i;
            }
            if (!(assumptions < get(i)->context())) {
                break;
            }
        }
        i++;
        assert(!contains(assumptions));
        if (size() == capacity()) {
#ifdef DEBUG_DISPATCH
            std::cout << "Tried to insert into a full Dispatch table. Have: \n";
            for (size_t i = 0; i < size(); ++i) {
                auto e = getEntry(i);
                std::cout << "* " << Function::unpack(e)->context() << "\n";
            }
            std::cout << "\n";
            std::cout << "Tried to insert: " << assumptions << "\n";
            Rf_error("dispatch table overflow");
#endif
            // Evict one element and retry
            auto pos = 1 + (Random::singleton()() % (size() - 1));
            size_--;
            while (pos < size()) {
                setEntry(pos, getEntry(pos + 1));
                pos++;
            }
            return negotiateSlot(assumptions);
        }

        for (size_t j = size(); j > i; --j)
            setEntry(j, getEntry(j - 1));
        size_++;

        // Slot i is now available for insertion of context now
        setEntry(i, R_NilValue);
        return i;

#ifdef DEBUG_DISPATCH
        std::cout << "Added version to DT, new order is: \n";
        for (size_t i = 0; i < size(); ++i) {
            auto e = getEntry(i);
            std::cout << "* " << Function::unpack(e)->context() << "\n";
        }
        std::cout << "\n";
        for (size_t i = 0; i < size() - 1; ++i) {
            assert(get(i)->context() < get(i + 1)->context());
            assert(get(i)->context() != get(i + 1)->context());
            assert(!(get(i + 1)->context() < get(i)->context()));
        }
        assert(contains(fun->context()));
#endif
    }
    // insert function ordered by increasing number of assumptions
//     void insert(Function* fun) {
//         // TODO: we might need to grow the DT here!
//         assert(size() > 0);
//         assert(fun->signature().optimization !=
//                FunctionSignature::OptimizationLevel::Baseline);
//         auto assumptions = fun->context();
//         size_t i;
//         for (i = size() - 1; i > 0; --i) {
//             auto old = get(i);
//             if (old->context() == assumptions) {
//                 if (i != 0) {
//                     // Remember deopt counts across recompilation to avoid
//                     // deopt loops
//                     fun->addDeoptCount(old->deoptCount());
//                     setEntry(i, fun->container());
//                     assert(get(i) == fun);
//                 }
//                 // hast only exists for parent closures, no hast depends on inner function's vtables
//                 if (hast) {
//                     tryLinking(hast, assumptions.toI(), fun->signature().numArguments);
//                 }
//                 return;
//             }
//             if (!(assumptions < get(i)->context())) {
//                 break;
//             }
//         }
//         i++;
//         assert(!contains(fun->context()));
//         if (size() == capacity()) {
// #ifdef DEBUG_DISPATCH
//             std::cout << "Tried to insert into a full Dispatch table. Have: \n";
//             for (size_t i = 0; i < size(); ++i) {
//                 auto e = getEntry(i);
//                 std::cout << "* " << Function::unpack(e)->context() << "\n";
//             }
//             std::cout << "\n";
//             std::cout << "Tried to insert: " << assumptions << "\n";
//             Rf_error("dispatch table overflow");
// #endif
//             // Evict one element and retry
//             auto pos = 1 + (Random::singleton()() % (size() - 1));
//             size_--;
//             while (pos < size()) {
//                 setEntry(pos, getEntry(pos + 1));
//                 pos++;
//             }
//             // hast only exists for parent closures, no hast depends on inner function's vtables
//             if (hast) {
//                 tryLinking(hast, assumptions.toI(), fun->signature().numArguments);
//             }
//             return insert(fun);
//         }

//         for (size_t j = size(); j > i; --j)
//             setEntry(j, getEntry(j - 1));
//         size_++;
//         setEntry(i, fun->container());

// #ifdef DEBUG_DISPATCH
//         std::cout << "Added version to DT, new order is: \n";
//         for (size_t i = 0; i < size(); ++i) {
//             auto e = getEntry(i);
//             std::cout << "* " << Function::unpack(e)->context() << "\n";
//         }
//         std::cout << "\n";
//         for (size_t i = 0; i < size() - 1; ++i) {
//             assert(get(i)->context() < get(i + 1)->context());
//             assert(get(i)->context() != get(i + 1)->context());
//             assert(!(get(i + 1)->context() < get(i)->context()));
//         }
//         assert(contains(fun->context()));
// #endif
//         // hast only exists for parent closures, no hast depends on inner function's vtables
//         if (hast) {
//             tryLinking(hast, assumptions.toI(), fun->signature().numArguments);
//         }
//     }

    static DispatchTable* create(size_t capacity = 20) {
        size_t sz =
            sizeof(DispatchTable) + (capacity * sizeof(DispatchTableEntry));
        SEXP s = Rf_allocVector(EXTERNALSXP, sz);
        return new (INTEGER(s)) DispatchTable(capacity);
    }

    size_t capacity() const { return info.gc_area_length; }

    static DispatchTable* deserialize(SEXP refTable, R_inpstream_t inp) {
        DispatchTable* table = create();
        PROTECT(table->container());
        AddReadRef(refTable, table->container());
        table->size_ = InInteger(inp);
        for (size_t i = 0; i < table->size(); i++) {
            table->setEntry(i,
                            Function::deserialize(refTable, inp)->container());
        }
        UNPROTECT(1);
        return table;
    }

    void serialize(SEXP refTable, R_outpstream_t out) const {
        HashAdd(container(), refTable);
        OutInteger(out, 1);
        baseline()->serialize(refTable, out);
    }

    Context userDefinedContext() const { return userDefinedContext_; }
    DispatchTable* newWithUserContext(Context udc) {

        auto clone = create(this->capacity());
        clone->setEntry(0, this->getEntry(0));

        auto j = 1;
        for (size_t i = 1; i < size(); i++) {
            if (get(i)->context().smaller(udc)) {
                clone->setEntry(j, getEntry(i));
                j++;
            }
        }

        clone->size_ = j;
        clone->userDefinedContext_ = udc;
        return clone;
    }

    Context combineContextWith(Context anotherContext) {
        return userDefinedContext_ | anotherContext;
    }

    // bool disableFurtherSpecialization = false;
    SEXP hast = nullptr;

    Context mask = Context(0ul);

  private:
    DispatchTable() = delete;
    explicit DispatchTable(size_t cap)
        : RirRuntimeObject(
              // GC area starts at the end of the DispatchTable
              sizeof(DispatchTable),
              // GC area is just the pointers in the entry array
              cap) {}

    size_t size_ = 0;
    Context userDefinedContext_;
};
#pragma pack(pop)
} // namespace rir

#endif
