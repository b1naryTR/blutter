#pragma once
#include <string>
#include "DartFnBase.h"
#include <vm/stub_code_list.h>

// Dart 3.13 merged the object store stubs into VM_STUB_CODE_LIST (served from
// dart::Roots). Keep their historical "<name>Stub" kinds so the analyzer's
// checks work unchanged; the loader fetches them with dart::StubCode::<name>().
// This list is the Dart 3.12 OBJECT_STORE_STUB_CODE_LIST names that still exist
// as 3.13 VM stubs (AllocateClosure* was replaced by AllocateClosure1..4).
#if defined(OBJECT_STORE_STUB_CODE_LIST)
#define BLUTTER_OBJECT_STORE_STUB_LIST(DO) OBJECT_STORE_STUB_CODE_LIST(DO)
#else
#define BLUTTER_MERGED_OBJECT_STORE_STUBS 1
#define BLUTTER_OBJECT_STORE_STUB_LIST(DO) \
	DO(_, DispatchTableNullError) \
	DO(_, LateInitializationErrorSharedWithFPURegs) \
	DO(_, LateInitializationErrorSharedWithoutFPURegs) \
	DO(_, NullErrorSharedWithFPURegs) \
	DO(_, NullErrorSharedWithoutFPURegs) \
	DO(_, NullArgErrorSharedWithFPURegs) \
	DO(_, NullArgErrorSharedWithoutFPURegs) \
	DO(_, NullCastErrorSharedWithFPURegs) \
	DO(_, NullCastErrorSharedWithoutFPURegs) \
	DO(_, RangeErrorSharedWithFPURegs) \
	DO(_, RangeErrorSharedWithoutFPURegs) \
	DO(_, WriteErrorSharedWithFPURegs) \
	DO(_, WriteErrorSharedWithoutFPURegs) \
	DO(_, FieldAccessErrorSharedWithFPURegs) \
	DO(_, FieldAccessErrorSharedWithoutFPURegs) \
	DO(_, AllocateMintSharedWithFPURegs) \
	DO(_, AllocateMintSharedWithoutFPURegs) \
	DO(_, StackOverflowSharedWithFPURegs) \
	DO(_, StackOverflowSharedWithoutFPURegs) \
	DO(_, AllocateArray) \
	DO(_, AllocateMint) \
	DO(_, AllocateDouble) \
	DO(_, AllocateFloat32x4) \
	DO(_, AllocateFloat64x2) \
	DO(_, AllocateInt32x4) \
	DO(_, AllocateInt8Array) \
	DO(_, AllocateUint8Array) \
	DO(_, AllocateUint8ClampedArray) \
	DO(_, AllocateInt16Array) \
	DO(_, AllocateUint16Array) \
	DO(_, AllocateInt32Array) \
	DO(_, AllocateUint32Array) \
	DO(_, AllocateInt64Array) \
	DO(_, AllocateUint64Array) \
	DO(_, AllocateFloat32Array) \
	DO(_, AllocateFloat64Array) \
	DO(_, AllocateFloat32x4Array) \
	DO(_, AllocateInt32x4Array) \
	DO(_, AllocateFloat64x2Array) \
	DO(_, AllocateContext) \
	DO(_, AllocateGrowableArray) \
	DO(_, AllocateObject) \
	DO(_, AllocateObjectParameterized) \
	DO(_, AllocateRecord) \
	DO(_, AllocateRecord2) \
	DO(_, AllocateRecord2Named) \
	DO(_, AllocateRecord3) \
	DO(_, AllocateRecord3Named) \
	DO(_, AllocateUnhandledException) \
	DO(_, CallClosureNoSuchMethod) \
	DO(_, CloneContext) \
	DO(_, CheckIsolateFieldAccess) \
	DO(_, DefaultTypeTest) \
	DO(_, DefaultNullableTypeTest) \
	DO(_, TopTypeTypeTest) \
	DO(_, NullableTypeParameterTypeTest) \
	DO(_, TypeParameterTypeTest) \
	DO(_, UnreachableTypeTest) \
	DO(_, SlowTypeTest) \
	DO(_, WriteBarrierWrappers) \
	DO(_, ArrayWriteBarrier) \
	DO(_, Throw) \
	DO(_, ReThrow) \
	DO(_, InitStaticField) \
	DO(_, InitLateStaticField) \
	DO(_, InitLateFinalStaticField) \
	DO(_, InitInstanceField) \
	DO(_, InitLateInstanceField) \
	DO(_, InitLateFinalInstanceField) \
	DO(_, InitSharedLateStaticField) \
	DO(_, Await) \
	DO(_, AwaitWithTypeCheck) \
	DO(_, CloneSuspendState) \
	DO(_, FfiAsyncCallbackSend) \
	DO(_, InitAsync) \
	DO(_, Resume) \
	DO(_, ReturnAsync) \
	DO(_, ReturnAsyncNotFuture) \
	DO(_, InitAsyncStar) \
	DO(_, YieldAsyncStar) \
	DO(_, ReturnAsyncStar) \
	DO(_, InitSyncStar) \
	DO(_, SuspendSyncStarAtStart) \
	DO(_, SuspendSyncStarAtYield) \
	DO(_, InstanceOf)
#endif

// forward declaration
class DartAbstractType;

class DartStub : public DartFnBase
{
public:
	enum Kind : int32_t {
#define DO(member, name) name ## Stub,
		BLUTTER_OBJECT_STORE_STUB_LIST(DO)
#undef DO
		BuildNonGenericMethodExtractorStub,
		BuildGenericMethodExtractorStub,
#define DO(name) name ## VMStub,
		VM_STUB_CODE_LIST(DO)
#undef DO
		SharedStub,
		AllocateUserObjectStub,
		TypeCheckStub,
		UnknownStub,
	};

	DartStub(const dart::CodePtr ptr, Kind kind, uint64_t addr, int64_t size, std::string name) :
		DartFnBase(addr, size, std::move(name)), ptr(ptr), kind(kind) {}
	DartStub() = delete;
	DartStub(const DartStub&) = default;
	DartStub(DartStub&&) = delete;
	DartStub& operator=(const DartStub&) = delete;
	virtual ~DartStub() {}

	virtual std::string FullName() const { return name + "Stub"; }
	virtual bool IsStub() const { return true; }

	// some stub might contain multiple of duplicated stubs
	// this functionality is needed only for DartStub (no subclasses)
	DartStub* Split(uint64_t another_ep_addr) {
		const auto updateSize = another_ep_addr - ep_addr;
		auto newStub = new DartStub(ptr, kind, another_ep_addr, size - updateSize, name);
		size = updateSize;
		return newStub;
	}

	const dart::CodePtr ptr;
	const Kind kind;
};

// only for non-predefined class
// these stubs just call AllocateObjectStub with their types
//   and call AllocateObjectParameterizedStub if there is a type parameter
class DartAllocateStub : public DartStub {
public:
	DartAllocateStub(const dart::CodePtr ptr, uint64_t addr, int64_t size, uint32_t cid, std::string name) :
		DartStub(ptr, AllocateUserObjectStub, addr, size, std::move(name)), cid(cid) {}
	DartAllocateStub() = delete;
	DartAllocateStub(const DartAllocateStub&) = delete;
	DartAllocateStub(DartAllocateStub&&) = delete;
	DartAllocateStub& operator=(const DartAllocateStub&) = delete;

	//virtual std::string Name() const { return "Allocate" + name + "Stub"; }
	virtual std::string FullName() const { return "Allocate" + name + "Stub"; }
	virtual uint32_t ReturnType() const { return cid; }

private:
	const uint32_t cid; // class id
};

// TypeTestStub
class DartTypeStub : public DartStub {
public:
	DartTypeStub(const dart::CodePtr ptr, const uint64_t addr, int64_t size, const DartAbstractType& abType, std::string name) :
		DartStub(ptr, TypeCheckStub, addr, size, std::move(name)), abType(abType) {}
	DartTypeStub() = delete;
	DartTypeStub(const DartTypeStub&) = delete;
	DartTypeStub(DartTypeStub&&) = delete;
	DartTypeStub& operator=(const DartTypeStub&) = delete;

	//virtual std::string Name() const { return "IsType_" + name + "_Stub"; }
	virtual std::string FullName() const { return "IsType_" + name + "_Stub"; }

	// With the Record type in Dart 3.0, Test stub can be Type or RecordType
	// So, we have to use AbstractType
	const DartAbstractType& abType;
};

class DartNativeFn : public DartFnBase
{
public:
	DartNativeFn(uint64_t addr, int64_t size, std::string name) :
		DartFnBase(addr, size, std::move(name)) {}
	DartNativeFn() = delete;
	DartNativeFn(const DartNativeFn&) = delete;
	DartNativeFn(DartNativeFn&&) = delete;
	DartNativeFn& operator=(const DartNativeFn&) = delete;
};