#include "CodeCache.h"
#include <cstdlib>
namespace rir {
bool CodeCache::serializer = std::getenv("CC_SERIALIZER") ? std::getenv("CC_SERIALIZER")[0] == '1' : false;
bool CodeCache::useBitcodes = std::getenv("USE_BITCODE") ? std::getenv("USE_BITCODE")[0] == '1' : false;
bool CodeCache::safeSerializer = getenv("SAFE_SERIALIZER") ? getenv("SAFE_SERIALIZER")[0] == '1' : false ;
bool CodeCache::safeDeserializer = getenv("SAFE_DESERIALIZER") ? getenv("SAFE_DESERIALIZER")[0] == '1' : false ;
// bool CodeCache::captureCompileStats = std::getenv("CAPTURE_ALL_COMPILE_STATS") ? true : false;

}
