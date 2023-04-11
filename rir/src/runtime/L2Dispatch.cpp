#include "Function.h"
#include "L2Dispatch.h"
#include "utils/Hast.h"
#include "runtime/DispatchTable.h"
#include <sstream>

#include <chrono>

using namespace std;

namespace rir {


static inline void printSpace(std::ostream& out, const int & space) {
	for (int i = 0; i < space; i++) {
		out << " ";
	}
}

std::string L2Dispatch::getInfo() {
	std::stringstream ss;

	if (_last == -1) {
		ss << "\"fallback\": " << "true" << ",";

	} else {
		ss << "\"fallback\": " << "false" << ",";

		unsigned total = 0, disabled = 0;
		for (int i = _last; i >= 0; i--) {
			auto currFun = getFunction(i);
			total++;
			std::string failReason;
			ss << "\"" << currFun << "(" << (currFun->disabled() ? "Disabled" : "Enabled") << ")\": " << currFun->matchSpeculativeContext(failReason);
			ss << ",";
			if (currFun->disabled()) {
				disabled++;
			}
		}
		ss << "\"total\": " << total << ",";
		ss << "\"disabled\": " << disabled;
	}
	return ss.str();
}

void L2Dispatch::print(std::ostream& out, const int & space) {
	assert(false && "--- L2 BROKEN FEATURE ---");
	// printSpace(out, space);
	// out << "=== L2 dispatcher (" << this << ") ===" << std::endl;
	// printSpace(out, space);
	// out << "Fallback: " << (getFallback()->disabled() ? "[disabled]" : "[]") << std::endl;
	// printSpace(out, space);
	// out << "Function List(_last=" << _last << ")" << std::endl;
	// for (int i = _last; i >= 0; i--) {
	// 	auto currFun = getFunction(i);
	// 	printSpace(out, space);
	// 	out << "(" << i << ")" << (currFun->disabled() ? "[disabled]" : "[]") << "[function=" << currFun << "]";
	// 	out << std::endl;

	// 	currFun->printSpeculativeContext(out,space + 2);
	// }

}
static bool l2FastcaseEnabled = getenv("L2_FASTCASE") ? getenv("L2_FASTCASE")[0] == '1' : true;

void L2Dispatch::insert(Function * f) {
	if (l2FastcaseEnabled)
		f->addFastcaseInvalidationConditions(&lastDispatch);
	int storageIdx = -1;
	for (int i = _last; i >= 0; i--) {
		Function * res = getFunction(i);
		if (res->disabled()) {
			if (lastDispatch.fun == res) {
				lastDispatch.fun = nullptr;
				lastDispatch.valid = false;
			}
			storageIdx = i;
			break;
		}
	}
	if (storageIdx == -1) {
		if (_last + 1 == functionListContainerSize()) {
			expandStorage();
		}
		assert(_last + 1 < functionListContainerSize());
		_last++;
		storageIdx = _last;
	}

	SET_VECTOR_ELT(getEntry(FN_LIST), storageIdx, f->container());
}

void L2Dispatch::expandStorage() {
	rir::Protect p;
	SEXP oldVec = getEntry(FN_LIST);
	int oldSize = Rf_length(oldVec);
	SEXP newVec;
	p(newVec = Rf_allocVector(VECSXP, oldSize + GROWTH_RATE));
	memcpy(DATAPTR(newVec), DATAPTR(oldVec), oldSize * sizeof(SEXP));
	setEntry(FN_LIST, newVec);
}

uint32_t L2Feedback::getTypeFeedbackVal() const {
	if (tag == 0) return fVal.typeFeedbackVal;
	else if (tag == 1) return *reinterpret_cast<uint32_t*>(fVal.typeFeedbackPtr);
	// std::cout << "getTypeFeedbackVal(tag=" << tag << ")" << std::endl;
	assert(false);
	return 0;
}

ObservedTest L2Feedback::getTestFeedbackVal() const {
	if (tag == 2) return fVal.testVal;
	else if(tag == 3) return *reinterpret_cast<ObservedTest*>(fPtr.pc);
	// std::cout << "getTestFeedbackVal(tag=" << tag << ")" << std::endl;
	assert(false);
	return ObservedTest();
}

int L2Feedback::getSrcIdxVal() const {
	if (tag == 4) return fVal.srcIdx;
	else if(tag == 5) {
		// std::cout << "getSrcIdxVal(tag=5): " << fPtr.pc << ", " << fPtr.code << std::endl;
		ObservedCallees * prof = (ObservedCallees *) fPtr.pc;
		// std::cout << "prof: " << prof->invalid << std::endl;
		if (prof->invalid) {
			return 0;
		}
		if (prof->numTargets > 0) {
			auto lastTargetIndex = prof->targets[prof->numTargets-1];
			SEXP currClos = fPtr.code->getExtraPoolEntry(lastTargetIndex);
			if (DispatchTable::check(BODY(currClos))) {
				return DispatchTable::unpack(BODY(currClos))->baseline()->body()->src;
			}
		}
		return 0;
	}
	// std::cout << "getSrcIdxVal(tag=" << tag << ")" << std::endl;
	assert(false);
	return 0;
}

L2Dispatch::L2Dispatch(Context context) :
	RirRuntimeObject(sizeof(L2Dispatch),ENTRIES_SIZE), lastDispatch({false, nullptr}) {

	_context = context;

	setFallback(R_NilValue);

	rir::Protect protecc;
	SEXP functionList;
	protecc(functionList = Rf_allocVector(VECSXP, GROWTH_RATE));
	setEntry(FN_LIST, functionList);
}

void L2Feedback::print(std::ostream& out, const int & space) const {
	for (int i = 0;i < space; i++) {
		out << " ";
	}

	if (tag < 2) {
		auto res = getTypeFeedbackVal();
		out << "<";
		reinterpret_cast<ObservedValues*>(&res)->print(out);
		out << ">";
		return;
	}


	if (tag < 4) {
		out << "<Branch[";
		switch (getTestFeedbackVal().seen) {
			case 0: out << "None"; break;
			case 1: out << "OnlyTrue"; break;
			case 2: out << "OnlyFalse"; break;
			case 3: out << "Both"; break;
		}
		out << "]>";
		return;
	}

	if (tag < 6) {
		out << "<CalleeAt[";
		out << getSrcIdxVal() << "]>";
		return;
	}
	out << "?>";
}

bool L2Feedback::operator==(const L2Feedback& other) {
	if (this->tag < 2) {
		return this->getTypeFeedbackVal() == other.getTypeFeedbackVal();
	}

	if (this->tag < 4) {
		if (other.tag == 6) return false;
		return this->getTestFeedbackVal().seen == other.getTestFeedbackVal().seen;
	}

	if (this->tag < 6) {
		if (other.tag == 6) return false;
		return this->getSrcIdxVal() == other.getSrcIdxVal();
	}

	return other.tag == 6;
}

Function * L2Dispatch::dispatch() {

	assert(_last != -1 && "Empty L2 dispatch");

	if (l2FastcaseEnabled && lastDispatch.valid) {
		if (EventLogger::logLevel >=3) {
			using namespace std::chrono;
			std::stringstream streamctx;
			std::stringstream streamname;

			auto start = std::chrono::high_resolution_clock::now();
			if (lastDispatch.fun) {
				//auto clos = Hast::hastMap[lastDispatch.fun->vtab->hast].clos;
				auto clos = lastDispatch.fun->vtab->tmpCallee;
				std::string hast = CHAR(PRINTNAME(lastDispatch.fun->vtab->hast));

                auto hastFull = hast;
                hastFull = hastFull + "_" +  std::to_string(lastDispatch.fun->vtab->offsetIdx);



				streamctx << lastDispatch.fun->context();
				streamname << lastDispatch.fun;
				if (lastDispatch.fun->l2Dispatcher) {
					if (lastDispatch.fun->disabled()) {
						EventLogger::logStats("l2FastCachedDisabled", streamname.str(), hastFull, 0, start, streamctx.str(), clos, 0,"");
					} else {
						EventLogger::logStats("l2FastCached", streamname.str(), hastFull, 0, start, streamctx.str(), clos, 0,"");
					}
				} else {
					if (lastDispatch.fun->disabled()) {
						EventLogger::logStats("l2FastJITDisabled", streamname.str(),hastFull,  0, start, streamctx.str(), clos, 0,"");
					} else {
						EventLogger::logStats("l2FastJIT", streamname.str(),hastFull,  0, start, streamctx.str(), clos, 0,"");
					}
				}
			} else {
				Function* funTmp = nullptr;
				if (getFallback())
					funTmp = getFallback();
				//SEXP clos = funTmp ? Hast::hastMap[funTmp->vtab->hast].clos : nullptr;
				auto clos =  funTmp ? funTmp->vtab->tmpCallee : nullptr;

				std::string hast = funTmp  ? CHAR(PRINTNAME(funTmp->vtab->hast)) : "NULL";

				auto hastFull = hast;
				if (funTmp)
					hastFull = hastFull+ "_" +  std::to_string(funTmp->vtab->offsetIdx);


				streamctx << "NULL";
				streamname << "NULL";
				EventLogger::logStats("l2FastBad", streamname.str(), hastFull, 0, start, streamctx.str(), clos, 0,"");
			}
		}
		// Alert: this CAN be null
		return lastDispatch.fun;
	}

	// if (lastDispatch && !lastDispatch->disabled()) {



	// 	if (EventLogger::logLevel >= 2) {
	// 		std::stringstream eventDataJSON;
	// 		eventDataJSON << "{"
	// 			<< "\"case\": " << "\"" << "fast" << "\"" << ","
	// 			<< "\"hast\": " << "\"" << (lastDispatch->vtab->hast ? CHAR(PRINTNAME(lastDispatch->vtab->hast)) : "NULL")  << "\"" << ","
	// 			<< "\"hastOffset\": " << "\"" << lastDispatch->vtab->offsetIdx << "\"" << ","
	// 			<< "\"function\": " << "\"" << lastDispatch << "\"" << ","
	// 			<< "\"context\": " << "\"" << lastDispatch->context() << "\"" << ","
	// 			<< "\"vtab\": " << "\"" << lastDispatch->vtab << "\"" << ","
	// 			<< "\"l2Info\": " << "{" << getInfo() << "}"
	// 			<< "}";

	// 		EventLogger::logUntimedEvent(
	// 			"l2Fast",
	// 			eventDataJSON.str()
	// 		);
	// 	}
	// 	return lastDispatch;
	// }

	std::string missReason = "";
	for (int i = _last; i >= 0; i--) {
		auto currFun = getFunction(i);
		std::string matchSpecFailureReason = "isDisabled=" + std::to_string(currFun->disabled());
		if (!currFun->disabled() && currFun->matchSpeculativeContext(matchSpecFailureReason)) {
			if (l2FastcaseEnabled) {
				lastDispatch.valid = true;
				lastDispatch.fun = currFun;
			}

			if (EventLogger::logLevel >= 2) {
				using namespace std::chrono;
				std::stringstream streamctx;
				streamctx << lastDispatch.fun->context();

				std::stringstream streamname;
				streamname << lastDispatch.fun;

				auto start = std::chrono::high_resolution_clock::now();
				//auto clos = Hast::hastMap[currFun->vtab->hast].clos;
				auto clos = currFun->vtab->tmpCallee;
				std::string hast = CHAR(PRINTNAME(currFun->vtab->hast));

                auto hastFull = hast  + "_" +  std::to_string(currFun->vtab->offsetIdx);



				EventLogger::logStats("l2Slow", streamname.str(),hastFull,  0, start, streamctx.str(), clos, 0,matchSpecFailureReason);
			}


			// if (EventLogger::logLevel >= 2) {
			// 	std::stringstream eventDataJSON;
			// 	eventDataJSON << "{"
			// 		<< "\"case\": " << "\"" << "slow" << "\"" << ","
			// 		<< "\"hast\": " << "\"" << (lastDispatch->vtab->hast ? CHAR(PRINTNAME(lastDispatch->vtab->hast)) : "NULL")  << "\"" << ","
			// 		<< "\"hastOffset\": " << "\"" << lastDispatch->vtab->offsetIdx << "\"" << ","
			// 		<< "\"function\": " << "\"" << lastDispatch << "\"" << ","
			// 		<< "\"context\": " << "\"" << lastDispatch->context() << "\"" << ","
			// 		<< "\"vtab\": " << "\"" << lastDispatch->vtab << "\"" << ","
			// 		<< "\"l2Info\": " << "{" << getInfo() << "}"
			// 		<< "}";

			// 	EventLogger::logUntimedEvent(
			// 		"l2Slow",
			// 		eventDataJSON.str()
			// 	);
			// }
			return currFun;
		}
		missReason += "|" + matchSpecFailureReason;
	}

	auto fallback = getFallback();

	if (EventLogger::logLevel >= 2) {
			using namespace std::chrono;
			std::stringstream streamctx;
			streamctx << fallback ? fallback->context() : Context(0ul);

			std::stringstream streamname;
			if (fallback) {
				streamname << fallback;
			} else {
				streamname << "NULL";
			}

			auto start = std::chrono::high_resolution_clock::now();

			Function* funTmp = nullptr;
			if (lastDispatch.fun)
				funTmp = lastDispatch.fun;
			if (fallback)
				funTmp = fallback;

			//SEXP clos = funTmp ? Hast::hastMap[funTmp->vtab->hast].clos : nullptr;
			auto clos = funTmp ? funTmp ->vtab->tmpCallee : nullptr;
			std::string hast = funTmp ?  CHAR(PRINTNAME(funTmp->vtab->hast)) : "NULL";

			auto hastFull = hast;
			if (funTmp)
			  hastFull = hastFull+ "_" +  std::to_string(funTmp->vtab->offsetIdx);


			EventLogger::logStats("l2Miss", streamname.str(),hastFull,  0, start, streamctx.str(), clos, 0,missReason);
	}


	// if (EventLogger::logLevel >= 2) {
	// 	std::stringstream eventDataJSON;
	// 	eventDataJSON << "{"
	// 		<< "\"case\": " << "\"" << "miss" << "\"" << ","
	// 		<< "\"hast\": " << "\"" << (fallback->vtab->hast ? CHAR(PRINTNAME(fallback->vtab->hast)) : "NULL")  << "\"" << ","
	// 		<< "\"hastOffset\": " << "\"" << fallback->vtab->offsetIdx << "\"" << ","
	// 		<< "\"function\": " << "\"" << fallback << "\"" << ","
	// 		<< "\"context\": " << "\"" << fallback->context() << "\"" << ","
	// 		<< "\"vtab\": " << "\"" << fallback->vtab << "\"" << ","
	// 		<< "\"l2Info\": " << "{" << getInfo() << "}"
	// 		<< "}";

	// 	EventLogger::logUntimedEvent(
	// 		"l2Miss",
	// 		eventDataJSON.str()
	// 	);
	// }



	if (l2FastcaseEnabled) {
		lastDispatch.valid = true;
		lastDispatch.fun = fallback;
	}

	return fallback;
}

} // namespace rir