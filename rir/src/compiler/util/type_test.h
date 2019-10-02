#ifndef PIR_TYPE_TEST
#define PIR_TYPE_TEST

#include "../pir/instruction.h"

namespace rir {
namespace pir {

class TypeTest {
  public:
    struct Info {
        PirType result;
        Instruction* test;
        bool expectation;
        rir::Code* srcCode;
        Opcode* origin;
    };
    static void Create(Value* i, const Instruction::TypeFeedback& feedback,
                       const std::function<void(Info)>& action,
                       const std::function<void()>& failed) {
        auto possible = i->type & feedback.type;

        if (possible.isVoid() || i->type.isA(possible))
            return failed();

        if (possible.isA(PirType(RType::integer).orPromiseWrapped()) ||
            possible.isA(PirType(RType::real).orPromiseWrapped()) ||
            possible.isA(PirType(RType::logical).orPromiseWrapped())) {
            return action({possible, new IsType(possible, i), true,
                           feedback.srcCode, feedback.origin});
        }

        if (!i->type.maybeLazy() && !possible.maybeObj()) {
            return action({i->type.notObject(), new IsObject(i), false,
                           feedback.srcCode, feedback.origin});
        }

        failed();
    }
};

} // namespace pir
} // namespace rir

#endif