#ifndef RIR_SER_FLAG_H
#define RIR_SER_FLAG_H

#define GROWTHRATE 5

namespace rir {
    class SerializerFlags {
    public:
        static bool serializerEnabled;
        static bool bitcodeDebuggingData;
        static bool captureCompileStats;
        static unsigned loadedFunctions;
    };
}

#endif
