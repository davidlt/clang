//===--- CGAtomic.cpp - Emit LLVM IR for atomic operations ----------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file contains the code for emitting atomic operations.
//
//===----------------------------------------------------------------------===//

#include "CodeGenFunction.h"
#include "CGCall.h"
#include "CodeGenModule.h"
#include "clang/AST/ASTContext.h"
#include "clang/CodeGen/CGFunctionInfo.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/Operator.h"

using namespace clang;
using namespace CodeGen;

namespace {
  class AtomicInfo {
    CodeGenFunction &CGF;
    QualType AtomicTy;
    QualType ValueTy;
    uint64_t AtomicSizeInBits;
    uint64_t ValueSizeInBits;
    CharUnits AtomicAlign;
    CharUnits ValueAlign;
    CharUnits LValueAlign;
    TypeEvaluationKind EvaluationKind;
    bool UseLibcall;
  public:
    AtomicInfo(CodeGenFunction &CGF, LValue &lvalue) : CGF(CGF) {
      assert(lvalue.isSimple());

      AtomicTy = lvalue.getType();
      ValueTy = AtomicTy->castAs<AtomicType>()->getValueType();
      EvaluationKind = CGF.getEvaluationKind(ValueTy);

      ASTContext &C = CGF.getContext();

      uint64_t ValueAlignInBits;
      uint64_t AtomicAlignInBits;
      TypeInfo ValueTI = C.getTypeInfo(ValueTy);
      ValueSizeInBits = ValueTI.Width;
      ValueAlignInBits = ValueTI.Align;

      TypeInfo AtomicTI = C.getTypeInfo(AtomicTy);
      AtomicSizeInBits = AtomicTI.Width;
      AtomicAlignInBits = AtomicTI.Align;

      assert(ValueSizeInBits <= AtomicSizeInBits);
      assert(ValueAlignInBits <= AtomicAlignInBits);

      AtomicAlign = C.toCharUnitsFromBits(AtomicAlignInBits);
      ValueAlign = C.toCharUnitsFromBits(ValueAlignInBits);
      if (lvalue.getAlignment().isZero())
        lvalue.setAlignment(AtomicAlign);

      UseLibcall =
        (AtomicSizeInBits > uint64_t(C.toBits(lvalue.getAlignment())) ||
         AtomicSizeInBits > C.getTargetInfo().getMaxAtomicInlineWidth());
    }

    QualType getAtomicType() const { return AtomicTy; }
    QualType getValueType() const { return ValueTy; }
    CharUnits getAtomicAlignment() const { return AtomicAlign; }
    CharUnits getValueAlignment() const { return ValueAlign; }
    uint64_t getAtomicSizeInBits() const { return AtomicSizeInBits; }
    uint64_t getValueSizeInBits() const { return AtomicSizeInBits; }
    TypeEvaluationKind getEvaluationKind() const { return EvaluationKind; }
    bool shouldUseLibcall() const { return UseLibcall; }

    /// Is the atomic size larger than the underlying value type?
    ///
    /// Note that the absence of padding does not mean that atomic
    /// objects are completely interchangeable with non-atomic
    /// objects: we might have promoted the alignment of a type
    /// without making it bigger.
    bool hasPadding() const {
      return (ValueSizeInBits != AtomicSizeInBits);
    }

    bool emitMemSetZeroIfNecessary(LValue dest) const;

    llvm::Value *getAtomicSizeValue() const {
      CharUnits size = CGF.getContext().toCharUnitsFromBits(AtomicSizeInBits);
      return CGF.CGM.getSize(size);
    }

    /// Cast the given pointer to an integer pointer suitable for
    /// atomic operations.
    llvm::Value *emitCastToAtomicIntPointer(llvm::Value *addr) const;

    /// Turn an atomic-layout object into an r-value.
    RValue convertTempToRValue(llvm::Value *addr,
                               AggValueSlot resultSlot,
                               SourceLocation loc) const;

    /// Copy an atomic r-value into atomic-layout memory.
    void emitCopyIntoMemory(RValue rvalue, LValue lvalue) const;

    /// Project an l-value down to the value field.
    LValue projectValue(LValue lvalue) const {
      llvm::Value *addr = lvalue.getAddress();
      if (hasPadding())
        addr = CGF.Builder.CreateStructGEP(addr, 0);

      return LValue::MakeAddr(addr, getValueType(), lvalue.getAlignment(),
                              CGF.getContext(), lvalue.getTBAAInfo());
    }

    /// Materialize an atomic r-value in atomic-layout memory.
    llvm::Value *materializeRValue(RValue rvalue) const;

  private:
    bool requiresMemSetZero(llvm::Type *type) const;
  };
}

static RValue emitAtomicLibcall(CodeGenFunction &CGF,
                                StringRef fnName,
                                QualType resultType,
                                CallArgList &args) {
  const CGFunctionInfo &fnInfo =
    CGF.CGM.getTypes().arrangeFreeFunctionCall(resultType, args,
            FunctionType::ExtInfo(), RequiredArgs::All);
  llvm::FunctionType *fnTy = CGF.CGM.getTypes().GetFunctionType(fnInfo);
  llvm::Constant *fn = CGF.CGM.CreateRuntimeFunction(fnTy, fnName);
  return CGF.EmitCall(fnInfo, fn, ReturnValueSlot(), args);
}

/// Does a store of the given IR type modify the full expected width?
static bool isFullSizeType(CodeGenModule &CGM, llvm::Type *type,
                           uint64_t expectedSize) {
  return (CGM.getDataLayout().getTypeStoreSize(type) * 8 == expectedSize);
}

/// Does the atomic type require memsetting to zero before initialization?
///
/// The IR type is provided as a way of making certain queries faster.
bool AtomicInfo::requiresMemSetZero(llvm::Type *type) const {
  // If the atomic type has size padding, we definitely need a memset.
  if (hasPadding()) return true;

  // Otherwise, do some simple heuristics to try to avoid it:
  switch (getEvaluationKind()) {
  // For scalars and complexes, check whether the store size of the
  // type uses the full size.
  case TEK_Scalar:
    return !isFullSizeType(CGF.CGM, type, AtomicSizeInBits);
  case TEK_Complex:
    return !isFullSizeType(CGF.CGM, type->getStructElementType(0),
                           AtomicSizeInBits / 2);

  // Padding in structs has an undefined bit pattern.  User beware.
  case TEK_Aggregate:
    return false;
  }
  llvm_unreachable("bad evaluation kind");
}

bool AtomicInfo::emitMemSetZeroIfNecessary(LValue dest) const {
  llvm::Value *addr = dest.getAddress();
  if (!requiresMemSetZero(addr->getType()->getPointerElementType()))
    return false;

  CGF.Builder.CreateMemSet(addr, llvm::ConstantInt::get(CGF.Int8Ty, 0),
                           AtomicSizeInBits / 8,
                           dest.getAlignment().getQuantity());
  return true;
}

static void emitAtomicCmpXchg(CodeGenFunction &CGF, AtomicExpr *E, bool IsWeak,
                              llvm::Value *Dest, llvm::Value *Ptr,
                              llvm::Value *Val1, llvm::Value *Val2,
                              uint64_t Size, unsigned Align,
                              llvm::AtomicOrdering SuccessOrder,
                              llvm::AtomicOrdering FailureOrder) {
  // Note that cmpxchg doesn't support weak cmpxchg, at least at the moment.
  llvm::LoadInst *Expected = CGF.Builder.CreateLoad(Val1);
  Expected->setAlignment(Align);
  llvm::LoadInst *Desired = CGF.Builder.CreateLoad(Val2);
  Desired->setAlignment(Align);

  llvm::AtomicCmpXchgInst *Pair = CGF.Builder.CreateAtomicCmpXchg(
      Ptr, Expected, Desired, SuccessOrder, FailureOrder);
  Pair->setVolatile(E->isVolatile());
  Pair->setWeak(IsWeak);

  // Cmp holds the result of the compare-exchange operation: true on success,
  // false on failure.
  llvm::Value *Old = CGF.Builder.CreateExtractValue(Pair, 0);
  llvm::Value *Cmp = CGF.Builder.CreateExtractValue(Pair, 1);

  // This basic block is used to hold the store instruction if the operation
  // failed.
  llvm::BasicBlock *StoreExpectedBB =
      CGF.createBasicBlock("cmpxchg.store_expected", CGF.CurFn);

  // This basic block is the exit point of the operation, we should end up
  // here regardless of whether or not the operation succeeded.
  llvm::BasicBlock *ContinueBB =
      CGF.createBasicBlock("cmpxchg.continue", CGF.CurFn);

  // Update Expected if Expected isn't equal to Old, otherwise branch to the
  // exit point.
  CGF.Builder.CreateCondBr(Cmp, ContinueBB, StoreExpectedBB);

  CGF.Builder.SetInsertPoint(StoreExpectedBB);
  // Update the memory at Expected with Old's value.
  llvm::StoreInst *StoreExpected = CGF.Builder.CreateStore(Old, Val1);
  StoreExpected->setAlignment(Align);
  // Finally, branch to the exit point.
  CGF.Builder.CreateBr(ContinueBB);

  CGF.Builder.SetInsertPoint(ContinueBB);
  // Update the memory at Dest with Cmp's value.
  CGF.EmitStoreOfScalar(Cmp, CGF.MakeAddrLValue(Dest, E->getType()));
  return;
}

/// Given an ordering required on success, emit all possible cmpxchg
/// instructions to cope with the provided (but possibly only dynamically known)
/// FailureOrder.
static void emitAtomicCmpXchgFailureSet(CodeGenFunction &CGF, AtomicExpr *E,
                                        bool IsWeak, llvm::Value *Dest,
                                        llvm::Value *Ptr, llvm::Value *Val1,
                                        llvm::Value *Val2,
                                        llvm::Value *FailureOrderVal,
                                        uint64_t Size, unsigned Align,
                                        llvm::AtomicOrdering SuccessOrder) {
  llvm::AtomicOrdering FailureOrder;
  if (llvm::ConstantInt *FO = dyn_cast<llvm::ConstantInt>(FailureOrderVal)) {
    switch (FO->getSExtValue()) {
    default:
      FailureOrder = llvm::Monotonic;
      break;
    case AtomicExpr::AO_ABI_memory_order_consume:
    case AtomicExpr::AO_ABI_memory_order_acquire:
      FailureOrder = llvm::Acquire;
      break;
    case AtomicExpr::AO_ABI_memory_order_seq_cst:
      FailureOrder = llvm::SequentiallyConsistent;
      break;
    }
    if (FailureOrder >= SuccessOrder) {
      // Don't assert on undefined behaviour.
      FailureOrder =
        llvm::AtomicCmpXchgInst::getStrongestFailureOrdering(SuccessOrder);
    }
    emitAtomicCmpXchg(CGF, E, IsWeak, Dest, Ptr, Val1, Val2, Size, Align,
                      SuccessOrder, FailureOrder);
    return;
  }

  // Create all the relevant BB's
  llvm::BasicBlock *MonotonicBB = nullptr, *AcquireBB = nullptr,
                   *SeqCstBB = nullptr;
  MonotonicBB = CGF.createBasicBlock("monotonic_fail", CGF.CurFn);
  if (SuccessOrder != llvm::Monotonic && SuccessOrder != llvm::Release)
    AcquireBB = CGF.createBasicBlock("acquire_fail", CGF.CurFn);
  if (SuccessOrder == llvm::SequentiallyConsistent)
    SeqCstBB = CGF.createBasicBlock("seqcst_fail", CGF.CurFn);

  llvm::BasicBlock *ContBB = CGF.createBasicBlock("atomic.continue", CGF.CurFn);

  llvm::SwitchInst *SI = CGF.Builder.CreateSwitch(FailureOrderVal, MonotonicBB);

  // Emit all the different atomics

  // MonotonicBB is arbitrarily chosen as the default case; in practice, this
  // doesn't matter unless someone is crazy enough to use something that
  // doesn't fold to a constant for the ordering.
  CGF.Builder.SetInsertPoint(MonotonicBB);
  emitAtomicCmpXchg(CGF, E, IsWeak, Dest, Ptr, Val1, Val2,
                    Size, Align, SuccessOrder, llvm::Monotonic);
  CGF.Builder.CreateBr(ContBB);

  if (AcquireBB) {
    CGF.Builder.SetInsertPoint(AcquireBB);
    emitAtomicCmpXchg(CGF, E, IsWeak, Dest, Ptr, Val1, Val2,
                      Size, Align, SuccessOrder, llvm::Acquire);
    CGF.Builder.CreateBr(ContBB);
    SI->addCase(CGF.Builder.getInt32(AtomicExpr::AO_ABI_memory_order_consume),
                AcquireBB);
    SI->addCase(CGF.Builder.getInt32(AtomicExpr::AO_ABI_memory_order_acquire),
                AcquireBB);
  }
  if (SeqCstBB) {
    CGF.Builder.SetInsertPoint(SeqCstBB);
    emitAtomicCmpXchg(CGF, E, IsWeak, Dest, Ptr, Val1, Val2,
                      Size, Align, SuccessOrder, llvm::SequentiallyConsistent);
    CGF.Builder.CreateBr(ContBB);
    SI->addCase(CGF.Builder.getInt32(AtomicExpr::AO_ABI_memory_order_seq_cst),
                SeqCstBB);
  }

  CGF.Builder.SetInsertPoint(ContBB);
}

static void EmitAtomicOp(CodeGenFunction &CGF, AtomicExpr *E, llvm::Value *Dest,
                         llvm::Value *Ptr, llvm::Value *Val1, llvm::Value *Val2,
                         llvm::Value *IsWeak, llvm::Value *FailureOrder,
                         uint64_t Size, unsigned Align,
                         llvm::AtomicOrdering Order) {
  llvm::AtomicRMWInst::BinOp Op = llvm::AtomicRMWInst::Add;
  llvm::Instruction::BinaryOps PostOp = (llvm::Instruction::BinaryOps)0;

  switch (E->getOp()) {
  case AtomicExpr::AO__c11_atomic_init:
    llvm_unreachable("Already handled!");

  case AtomicExpr::AO__c11_atomic_compare_exchange_strong:
    emitAtomicCmpXchgFailureSet(CGF, E, false, Dest, Ptr, Val1, Val2,
                                FailureOrder, Size, Align, Order);
    return;
  case AtomicExpr::AO__c11_atomic_compare_exchange_weak:
    emitAtomicCmpXchgFailureSet(CGF, E, true, Dest, Ptr, Val1, Val2,
                                FailureOrder, Size, Align, Order);
    return;
  case AtomicExpr::AO__atomic_compare_exchange:
  case AtomicExpr::AO__atomic_compare_exchange_n: {
    if (llvm::ConstantInt *IsWeakC = dyn_cast<llvm::ConstantInt>(IsWeak)) {
      emitAtomicCmpXchgFailureSet(CGF, E, IsWeakC->getZExtValue(), Dest, Ptr,
                                  Val1, Val2, FailureOrder, Size, Align, Order);
    } else {
      // Create all the relevant BB's
      llvm::BasicBlock *StrongBB =
          CGF.createBasicBlock("cmpxchg.strong", CGF.CurFn);
      llvm::BasicBlock *WeakBB = CGF.createBasicBlock("cmxchg.weak", CGF.CurFn);
      llvm::BasicBlock *ContBB =
          CGF.createBasicBlock("cmpxchg.continue", CGF.CurFn);

      llvm::SwitchInst *SI = CGF.Builder.CreateSwitch(IsWeak, WeakBB);
      SI->addCase(CGF.Builder.getInt1(false), StrongBB);

      CGF.Builder.SetInsertPoint(StrongBB);
      emitAtomicCmpXchgFailureSet(CGF, E, false, Dest, Ptr, Val1, Val2,
                                  FailureOrder, Size, Align, Order);
      CGF.Builder.CreateBr(ContBB);

      CGF.Builder.SetInsertPoint(WeakBB);
      emitAtomicCmpXchgFailureSet(CGF, E, true, Dest, Ptr, Val1, Val2,
                                  FailureOrder, Size, Align, Order);
      CGF.Builder.CreateBr(ContBB);

      CGF.Builder.SetInsertPoint(ContBB);
    }
    return;
  }
  case AtomicExpr::AO__c11_atomic_load:
  case AtomicExpr::AO__atomic_load_n:
  case AtomicExpr::AO__atomic_load: {
    llvm::LoadInst *Load = CGF.Builder.CreateLoad(Ptr);
    Load->setAtomic(Order);
    Load->setAlignment(Size);
    Load->setVolatile(E->isVolatile());
    llvm::StoreInst *StoreDest = CGF.Builder.CreateStore(Load, Dest);
    StoreDest->setAlignment(Align);
    return;
  }

  case AtomicExpr::AO__c11_atomic_store:
  case AtomicExpr::AO__atomic_store:
  case AtomicExpr::AO__atomic_store_n: {
    assert(!Dest && "Store does not return a value");
    llvm::LoadInst *LoadVal1 = CGF.Builder.CreateLoad(Val1);
    LoadVal1->setAlignment(Align);
    llvm::StoreInst *Store = CGF.Builder.CreateStore(LoadVal1, Ptr);
    Store->setAtomic(Order);
    Store->setAlignment(Size);
    Store->setVolatile(E->isVolatile());
    return;
  }

  case AtomicExpr::AO__c11_atomic_exchange:
  case AtomicExpr::AO__atomic_exchange_n:
  case AtomicExpr::AO__atomic_exchange:
    Op = llvm::AtomicRMWInst::Xchg;
    break;

  case AtomicExpr::AO__atomic_add_fetch:
    PostOp = llvm::Instruction::Add;
    // Fall through.
  case AtomicExpr::AO__c11_atomic_fetch_add:
  case AtomicExpr::AO__atomic_fetch_add:
    Op = llvm::AtomicRMWInst::Add;
    break;

  case AtomicExpr::AO__atomic_sub_fetch:
    PostOp = llvm::Instruction::Sub;
    // Fall through.
  case AtomicExpr::AO__c11_atomic_fetch_sub:
  case AtomicExpr::AO__atomic_fetch_sub:
    Op = llvm::AtomicRMWInst::Sub;
    break;

  case AtomicExpr::AO__atomic_and_fetch:
    PostOp = llvm::Instruction::And;
    // Fall through.
  case AtomicExpr::AO__c11_atomic_fetch_and:
  case AtomicExpr::AO__atomic_fetch_and:
    Op = llvm::AtomicRMWInst::And;
    break;

  case AtomicExpr::AO__atomic_or_fetch:
    PostOp = llvm::Instruction::Or;
    // Fall through.
  case AtomicExpr::AO__c11_atomic_fetch_or:
  case AtomicExpr::AO__atomic_fetch_or:
    Op = llvm::AtomicRMWInst::Or;
    break;

  case AtomicExpr::AO__atomic_xor_fetch:
    PostOp = llvm::Instruction::Xor;
    // Fall through.
  case AtomicExpr::AO__c11_atomic_fetch_xor:
  case AtomicExpr::AO__atomic_fetch_xor:
    Op = llvm::AtomicRMWInst::Xor;
    break;

  case AtomicExpr::AO__atomic_nand_fetch:
    PostOp = llvm::Instruction::And;
    // Fall through.
  case AtomicExpr::AO__atomic_fetch_nand:
    Op = llvm::AtomicRMWInst::Nand;
    break;
  }

  llvm::LoadInst *LoadVal1 = CGF.Builder.CreateLoad(Val1);
  LoadVal1->setAlignment(Align);
  llvm::AtomicRMWInst *RMWI =
      CGF.Builder.CreateAtomicRMW(Op, Ptr, LoadVal1, Order);
  RMWI->setVolatile(E->isVolatile());

  // For __atomic_*_fetch operations, perform the operation again to
  // determine the value which was written.
  llvm::Value *Result = RMWI;
  if (PostOp)
    Result = CGF.Builder.CreateBinOp(PostOp, RMWI, LoadVal1);
  if (E->getOp() == AtomicExpr::AO__atomic_nand_fetch)
    Result = CGF.Builder.CreateNot(Result);
  llvm::StoreInst *StoreDest = CGF.Builder.CreateStore(Result, Dest);
  StoreDest->setAlignment(Align);
}

// This function emits any expression (scalar, complex, or aggregate)
// into a temporary alloca.
static llvm::Value *
EmitValToTemp(CodeGenFunction &CGF, Expr *E) {
  llvm::Value *DeclPtr = CGF.CreateMemTemp(E->getType(), ".atomictmp");
  CGF.EmitAnyExprToMem(E, DeclPtr, E->getType().getQualifiers(),
                       /*Init*/ true);
  return DeclPtr;
}

static void
AddDirectArgument(CodeGenFunction &CGF, CallArgList &Args,
                  bool UseOptimizedLibcall, llvm::Value *Val, QualType ValTy,
                  SourceLocation Loc, CharUnits SizeInChars) {
  if (UseOptimizedLibcall) {
    // Load value and pass it to the function directly.
    unsigned Align = CGF.getContext().getTypeAlignInChars(ValTy).getQuantity();
    int64_t SizeInBits = CGF.getContext().toBits(SizeInChars);
    ValTy =
        CGF.getContext().getIntTypeForBitwidth(SizeInBits, /*Signed=*/false);
    llvm::Type *IPtrTy = llvm::IntegerType::get(CGF.getLLVMContext(),
                                                SizeInBits)->getPointerTo();
    Val = CGF.EmitLoadOfScalar(CGF.Builder.CreateBitCast(Val, IPtrTy), false,
                               Align, CGF.getContext().getPointerType(ValTy),
                               Loc);
    // Coerce the value into an appropriately sized integer type.
    Args.add(RValue::get(Val), ValTy);
  } else {
    // Non-optimized functions always take a reference.
    Args.add(RValue::get(CGF.EmitCastToVoidPtr(Val)),
                         CGF.getContext().VoidPtrTy);
  }
}

RValue CodeGenFunction::EmitAtomicExpr(AtomicExpr *E, llvm::Value *Dest) {
  QualType AtomicTy = E->getPtr()->getType()->getPointeeType();
  QualType MemTy = AtomicTy;
  if (const AtomicType *AT = AtomicTy->getAs<AtomicType>())
    MemTy = AT->getValueType();
  CharUnits sizeChars = getContext().getTypeSizeInChars(AtomicTy);
  uint64_t Size = sizeChars.getQuantity();
  CharUnits alignChars = getContext().getTypeAlignInChars(AtomicTy);
  unsigned Align = alignChars.getQuantity();
  unsigned MaxInlineWidthInBits =
    getTarget().getMaxAtomicInlineWidth();
  bool UseLibcall = (Size != Align ||
                     getContext().toBits(sizeChars) > MaxInlineWidthInBits);

  llvm::Value *IsWeak = nullptr, *OrderFail = nullptr, *Val1 = nullptr,
              *Val2 = nullptr;
  llvm::Value *Ptr = EmitScalarExpr(E->getPtr());

  if (E->getOp() == AtomicExpr::AO__c11_atomic_init) {
    assert(!Dest && "Init does not return a value");
    LValue lvalue = LValue::MakeAddr(Ptr, AtomicTy, alignChars, getContext());
    EmitAtomicInit(E->getVal1(), lvalue);
    return RValue::get(nullptr);
  }

  llvm::Value *Order = EmitScalarExpr(E->getOrder());

  switch (E->getOp()) {
  case AtomicExpr::AO__c11_atomic_init:
    llvm_unreachable("Already handled!");

  case AtomicExpr::AO__c11_atomic_load:
  case AtomicExpr::AO__atomic_load_n:
    break;

  case AtomicExpr::AO__atomic_load:
    Dest = EmitScalarExpr(E->getVal1());
    break;

  case AtomicExpr::AO__atomic_store:
    Val1 = EmitScalarExpr(E->getVal1());
    break;

  case AtomicExpr::AO__atomic_exchange:
    Val1 = EmitScalarExpr(E->getVal1());
    Dest = EmitScalarExpr(E->getVal2());
    break;

  case AtomicExpr::AO__c11_atomic_compare_exchange_strong:
  case AtomicExpr::AO__c11_atomic_compare_exchange_weak:
  case AtomicExpr::AO__atomic_compare_exchange_n:
  case AtomicExpr::AO__atomic_compare_exchange:
    Val1 = EmitScalarExpr(E->getVal1());
    if (E->getOp() == AtomicExpr::AO__atomic_compare_exchange)
      Val2 = EmitScalarExpr(E->getVal2());
    else
      Val2 = EmitValToTemp(*this, E->getVal2());
    OrderFail = EmitScalarExpr(E->getOrderFail());
    if (E->getNumSubExprs() == 6)
      IsWeak = EmitScalarExpr(E->getWeak());
    break;

  case AtomicExpr::AO__c11_atomic_fetch_add:
  case AtomicExpr::AO__c11_atomic_fetch_sub:
    if (MemTy->isPointerType()) {
      // For pointer arithmetic, we're required to do a bit of math:
      // adding 1 to an int* is not the same as adding 1 to a uintptr_t.
      // ... but only for the C11 builtins. The GNU builtins expect the
      // user to multiply by sizeof(T).
      QualType Val1Ty = E->getVal1()->getType();
      llvm::Value *Val1Scalar = EmitScalarExpr(E->getVal1());
      CharUnits PointeeIncAmt =
          getContext().getTypeSizeInChars(MemTy->getPointeeType());
      Val1Scalar = Builder.CreateMul(Val1Scalar, CGM.getSize(PointeeIncAmt));
      Val1 = CreateMemTemp(Val1Ty, ".atomictmp");
      EmitStoreOfScalar(Val1Scalar, MakeAddrLValue(Val1, Val1Ty));
      break;
    }
    // Fall through.
  case AtomicExpr::AO__atomic_fetch_add:
  case AtomicExpr::AO__atomic_fetch_sub:
  case AtomicExpr::AO__atomic_add_fetch:
  case AtomicExpr::AO__atomic_sub_fetch:
  case AtomicExpr::AO__c11_atomic_store:
  case AtomicExpr::AO__c11_atomic_exchange:
  case AtomicExpr::AO__atomic_store_n:
  case AtomicExpr::AO__atomic_exchange_n:
  case AtomicExpr::AO__c11_atomic_fetch_and:
  case AtomicExpr::AO__c11_atomic_fetch_or:
  case AtomicExpr::AO__c11_atomic_fetch_xor:
  case AtomicExpr::AO__atomic_fetch_and:
  case AtomicExpr::AO__atomic_fetch_or:
  case AtomicExpr::AO__atomic_fetch_xor:
  case AtomicExpr::AO__atomic_fetch_nand:
  case AtomicExpr::AO__atomic_and_fetch:
  case AtomicExpr::AO__atomic_or_fetch:
  case AtomicExpr::AO__atomic_xor_fetch:
  case AtomicExpr::AO__atomic_nand_fetch:
    Val1 = EmitValToTemp(*this, E->getVal1());
    break;
  }

  if (!E->getType()->isVoidType() && !Dest)
    Dest = CreateMemTemp(E->getType(), ".atomicdst");

  // Use a library call.  See: http://gcc.gnu.org/wiki/Atomic/GCCMM/LIbrary .
  if (UseLibcall) {
    bool UseOptimizedLibcall = false;
    switch (E->getOp()) {
    case AtomicExpr::AO__c11_atomic_fetch_add:
    case AtomicExpr::AO__atomic_fetch_add:
    case AtomicExpr::AO__c11_atomic_fetch_and:
    case AtomicExpr::AO__atomic_fetch_and:
    case AtomicExpr::AO__c11_atomic_fetch_or:
    case AtomicExpr::AO__atomic_fetch_or:
    case AtomicExpr::AO__c11_atomic_fetch_sub:
    case AtomicExpr::AO__atomic_fetch_sub:
    case AtomicExpr::AO__c11_atomic_fetch_xor:
    case AtomicExpr::AO__atomic_fetch_xor:
      // For these, only library calls for certain sizes exist.
      UseOptimizedLibcall = true;
      break;
    default:
      // Only use optimized library calls for sizes for which they exist.
      if (Size == 1 || Size == 2 || Size == 4 || Size == 8)
        UseOptimizedLibcall = true;
      break;
    }

    CallArgList Args;
    if (!UseOptimizedLibcall) {
      // For non-optimized library calls, the size is the first parameter
      Args.add(RValue::get(llvm::ConstantInt::get(SizeTy, Size)),
               getContext().getSizeType());
    }
    // Atomic address is the first or second parameter
    Args.add(RValue::get(EmitCastToVoidPtr(Ptr)), getContext().VoidPtrTy);

    std::string LibCallName;
    QualType LoweredMemTy =
      MemTy->isPointerType() ? getContext().getIntPtrType() : MemTy;
    QualType RetTy;
    bool HaveRetTy = false;
    switch (E->getOp()) {
    // There is only one libcall for compare an exchange, because there is no
    // optimisation benefit possible from a libcall version of a weak compare
    // and exchange.
    // bool __atomic_compare_exchange(size_t size, void *mem, void *expected,
    //                                void *desired, int success, int failure)
    // bool __atomic_compare_exchange_N(T *mem, T *expected, T desired,
    //                                  int success, int failure)
    case AtomicExpr::AO__c11_atomic_compare_exchange_weak:
    case AtomicExpr::AO__c11_atomic_compare_exchange_strong:
    case AtomicExpr::AO__atomic_compare_exchange:
    case AtomicExpr::AO__atomic_compare_exchange_n:
      LibCallName = "__atomic_compare_exchange";
      RetTy = getContext().BoolTy;
      HaveRetTy = true;
      Args.add(RValue::get(EmitCastToVoidPtr(Val1)), getContext().VoidPtrTy);
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val2, MemTy,
                        E->getExprLoc(), sizeChars);
      Args.add(RValue::get(Order), getContext().IntTy);
      Order = OrderFail;
      break;
    // void __atomic_exchange(size_t size, void *mem, void *val, void *return,
    //                        int order)
    // T __atomic_exchange_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_exchange:
    case AtomicExpr::AO__atomic_exchange_n:
    case AtomicExpr::AO__atomic_exchange:
      LibCallName = "__atomic_exchange";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, MemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // void __atomic_store(size_t size, void *mem, void *val, int order)
    // void __atomic_store_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_store:
    case AtomicExpr::AO__atomic_store:
    case AtomicExpr::AO__atomic_store_n:
      LibCallName = "__atomic_store";
      RetTy = getContext().VoidTy;
      HaveRetTy = true;
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, MemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // void __atomic_load(size_t size, void *mem, void *return, int order)
    // T __atomic_load_N(T *mem, int order)
    case AtomicExpr::AO__c11_atomic_load:
    case AtomicExpr::AO__atomic_load:
    case AtomicExpr::AO__atomic_load_n:
      LibCallName = "__atomic_load";
      break;
    // T __atomic_fetch_add_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_fetch_add:
    case AtomicExpr::AO__atomic_fetch_add:
      LibCallName = "__atomic_fetch_add";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, LoweredMemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // T __atomic_fetch_and_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_fetch_and:
    case AtomicExpr::AO__atomic_fetch_and:
      LibCallName = "__atomic_fetch_and";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, MemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // T __atomic_fetch_or_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_fetch_or:
    case AtomicExpr::AO__atomic_fetch_or:
      LibCallName = "__atomic_fetch_or";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, MemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // T __atomic_fetch_sub_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_fetch_sub:
    case AtomicExpr::AO__atomic_fetch_sub:
      LibCallName = "__atomic_fetch_sub";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, LoweredMemTy,
                        E->getExprLoc(), sizeChars);
      break;
    // T __atomic_fetch_xor_N(T *mem, T val, int order)
    case AtomicExpr::AO__c11_atomic_fetch_xor:
    case AtomicExpr::AO__atomic_fetch_xor:
      LibCallName = "__atomic_fetch_xor";
      AddDirectArgument(*this, Args, UseOptimizedLibcall, Val1, MemTy,
                        E->getExprLoc(), sizeChars);
      break;
    default: return EmitUnsupportedRValue(E, "atomic library call");
    }

    // Optimized functions have the size in their name.
    if (UseOptimizedLibcall)
      LibCallName += "_" + llvm::utostr(Size);
    // By default, assume we return a value of the atomic type.
    if (!HaveRetTy) {
      if (UseOptimizedLibcall) {
        // Value is returned directly.
        // The function returns an appropriately sized integer type.
        RetTy = getContext().getIntTypeForBitwidth(
            getContext().toBits(sizeChars), /*Signed=*/false);
      } else {
        // Value is returned through parameter before the order.
        RetTy = getContext().VoidTy;
        Args.add(RValue::get(EmitCastToVoidPtr(Dest)),
                 getContext().VoidPtrTy);
      }
    }
    // order is always the last parameter
    Args.add(RValue::get(Order),
             getContext().IntTy);

    const CGFunctionInfo &FuncInfo =
        CGM.getTypes().arrangeFreeFunctionCall(RetTy, Args,
            FunctionType::ExtInfo(), RequiredArgs::All);
    llvm::FunctionType *FTy = CGM.getTypes().GetFunctionType(FuncInfo);
    llvm::Constant *Func = CGM.CreateRuntimeFunction(FTy, LibCallName);
    RValue Res = EmitCall(FuncInfo, Func, ReturnValueSlot(), Args);
    if (!RetTy->isVoidType()) {
      if (UseOptimizedLibcall) {
        if (HaveRetTy)
          return Res;
        llvm::StoreInst *StoreDest = Builder.CreateStore(
            Res.getScalarVal(),
            Builder.CreateBitCast(Dest, FTy->getReturnType()->getPointerTo()));
        StoreDest->setAlignment(Align);
      }
    }
    if (E->getType()->isVoidType())
      return RValue::get(nullptr);
    return convertTempToRValue(Dest, E->getType(), E->getExprLoc());
  }

  bool IsStore = E->getOp() == AtomicExpr::AO__c11_atomic_store ||
                 E->getOp() == AtomicExpr::AO__atomic_store ||
                 E->getOp() == AtomicExpr::AO__atomic_store_n;
  bool IsLoad = E->getOp() == AtomicExpr::AO__c11_atomic_load ||
                E->getOp() == AtomicExpr::AO__atomic_load ||
                E->getOp() == AtomicExpr::AO__atomic_load_n;

  llvm::Type *IPtrTy =
      llvm::IntegerType::get(getLLVMContext(), Size * 8)->getPointerTo();
  llvm::Value *OrigDest = Dest;
  Ptr = Builder.CreateBitCast(Ptr, IPtrTy);
  if (Val1) Val1 = Builder.CreateBitCast(Val1, IPtrTy);
  if (Val2) Val2 = Builder.CreateBitCast(Val2, IPtrTy);
  if (Dest && !E->isCmpXChg()) Dest = Builder.CreateBitCast(Dest, IPtrTy);

  if (isa<llvm::ConstantInt>(Order)) {
    int ord = cast<llvm::ConstantInt>(Order)->getZExtValue();
    switch (ord) {
    case AtomicExpr::AO_ABI_memory_order_relaxed:
      EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                   Size, Align, llvm::Monotonic);
      break;
    case AtomicExpr::AO_ABI_memory_order_consume:
    case AtomicExpr::AO_ABI_memory_order_acquire:
      if (IsStore)
        break; // Avoid crashing on code with undefined behavior
      EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                   Size, Align, llvm::Acquire);
      break;
    case AtomicExpr::AO_ABI_memory_order_release:
      if (IsLoad)
        break; // Avoid crashing on code with undefined behavior
      EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                   Size, Align, llvm::Release);
      break;
    case AtomicExpr::AO_ABI_memory_order_acq_rel:
      if (IsLoad || IsStore)
        break; // Avoid crashing on code with undefined behavior
      EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                   Size, Align, llvm::AcquireRelease);
      break;
    case AtomicExpr::AO_ABI_memory_order_seq_cst:
      EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                   Size, Align, llvm::SequentiallyConsistent);
      break;
    default: // invalid order
      // We should not ever get here normally, but it's hard to
      // enforce that in general.
      break;
    }
    if (E->getType()->isVoidType())
      return RValue::get(nullptr);
    return convertTempToRValue(OrigDest, E->getType(), E->getExprLoc());
  }

  // Long case, when Order isn't obviously constant.

  // Create all the relevant BB's
  llvm::BasicBlock *MonotonicBB = nullptr, *AcquireBB = nullptr,
                   *ReleaseBB = nullptr, *AcqRelBB = nullptr,
                   *SeqCstBB = nullptr;
  MonotonicBB = createBasicBlock("monotonic", CurFn);
  if (!IsStore)
    AcquireBB = createBasicBlock("acquire", CurFn);
  if (!IsLoad)
    ReleaseBB = createBasicBlock("release", CurFn);
  if (!IsLoad && !IsStore)
    AcqRelBB = createBasicBlock("acqrel", CurFn);
  SeqCstBB = createBasicBlock("seqcst", CurFn);
  llvm::BasicBlock *ContBB = createBasicBlock("atomic.continue", CurFn);

  // Create the switch for the split
  // MonotonicBB is arbitrarily chosen as the default case; in practice, this
  // doesn't matter unless someone is crazy enough to use something that
  // doesn't fold to a constant for the ordering.
  Order = Builder.CreateIntCast(Order, Builder.getInt32Ty(), false);
  llvm::SwitchInst *SI = Builder.CreateSwitch(Order, MonotonicBB);

  // Emit all the different atomics
  Builder.SetInsertPoint(MonotonicBB);
  EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
               Size, Align, llvm::Monotonic);
  Builder.CreateBr(ContBB);
  if (!IsStore) {
    Builder.SetInsertPoint(AcquireBB);
    EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                 Size, Align, llvm::Acquire);
    Builder.CreateBr(ContBB);
    SI->addCase(Builder.getInt32(AtomicExpr::AO_ABI_memory_order_consume),
                AcquireBB);
    SI->addCase(Builder.getInt32(AtomicExpr::AO_ABI_memory_order_acquire),
                AcquireBB);
  }
  if (!IsLoad) {
    Builder.SetInsertPoint(ReleaseBB);
    EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                 Size, Align, llvm::Release);
    Builder.CreateBr(ContBB);
    SI->addCase(Builder.getInt32(AtomicExpr::AO_ABI_memory_order_release),
                ReleaseBB);
  }
  if (!IsLoad && !IsStore) {
    Builder.SetInsertPoint(AcqRelBB);
    EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
                 Size, Align, llvm::AcquireRelease);
    Builder.CreateBr(ContBB);
    SI->addCase(Builder.getInt32(AtomicExpr::AO_ABI_memory_order_acq_rel),
                AcqRelBB);
  }
  Builder.SetInsertPoint(SeqCstBB);
  EmitAtomicOp(*this, E, Dest, Ptr, Val1, Val2, IsWeak, OrderFail,
               Size, Align, llvm::SequentiallyConsistent);
  Builder.CreateBr(ContBB);
  SI->addCase(Builder.getInt32(AtomicExpr::AO_ABI_memory_order_seq_cst),
              SeqCstBB);

  // Cleanup and return
  Builder.SetInsertPoint(ContBB);
  if (E->getType()->isVoidType())
    return RValue::get(nullptr);
  return convertTempToRValue(OrigDest, E->getType(), E->getExprLoc());
}

llvm::Value *AtomicInfo::emitCastToAtomicIntPointer(llvm::Value *addr) const {
  unsigned addrspace =
    cast<llvm::PointerType>(addr->getType())->getAddressSpace();
  llvm::IntegerType *ty =
    llvm::IntegerType::get(CGF.getLLVMContext(), AtomicSizeInBits);
  return CGF.Builder.CreateBitCast(addr, ty->getPointerTo(addrspace));
}

RValue AtomicInfo::convertTempToRValue(llvm::Value *addr,
                                       AggValueSlot resultSlot,
                                       SourceLocation loc) const {
  if (EvaluationKind == TEK_Aggregate)
    return resultSlot.asRValue();

  // Drill into the padding structure if we have one.
  if (hasPadding())
    addr = CGF.Builder.CreateStructGEP(addr, 0);

  // Otherwise, just convert the temporary to an r-value using the
  // normal conversion routine.
  return CGF.convertTempToRValue(addr, getValueType(), loc);
}

/// Emit a load from an l-value of atomic type.  Note that the r-value
/// we produce is an r-value of the atomic *value* type.
RValue CodeGenFunction::EmitAtomicLoad(LValue src, SourceLocation loc,
                                       AggValueSlot resultSlot) {
  AtomicInfo atomics(*this, src);

  // Check whether we should use a library call.
  if (atomics.shouldUseLibcall()) {
    llvm::Value *tempAddr;
    if (!resultSlot.isIgnored()) {
      assert(atomics.getEvaluationKind() == TEK_Aggregate);
      tempAddr = resultSlot.getAddr();
    } else {
      tempAddr = CreateMemTemp(atomics.getAtomicType(), "atomic-load-temp");
    }

    // void __atomic_load(size_t size, void *mem, void *return, int order);
    CallArgList args;
    args.add(RValue::get(atomics.getAtomicSizeValue()),
             getContext().getSizeType());
    args.add(RValue::get(EmitCastToVoidPtr(src.getAddress())),
             getContext().VoidPtrTy);
    args.add(RValue::get(EmitCastToVoidPtr(tempAddr)),
             getContext().VoidPtrTy);
    args.add(RValue::get(llvm::ConstantInt::get(
                 IntTy, AtomicExpr::AO_ABI_memory_order_seq_cst)),
             getContext().IntTy);
    emitAtomicLibcall(*this, "__atomic_load", getContext().VoidTy, args);

    // Produce the r-value.
    return atomics.convertTempToRValue(tempAddr, resultSlot, loc);
  }

  // Okay, we're doing this natively.
  llvm::Value *addr = atomics.emitCastToAtomicIntPointer(src.getAddress());
  llvm::LoadInst *load = Builder.CreateLoad(addr, "atomic-load");
  load->setAtomic(llvm::SequentiallyConsistent);

  // Other decoration.
  load->setAlignment(src.getAlignment().getQuantity());
  if (src.isVolatileQualified())
    load->setVolatile(true);
  if (src.getTBAAInfo())
    CGM.DecorateInstruction(load, src.getTBAAInfo());

  // Okay, turn that back into the original value type.
  QualType valueType = atomics.getValueType();
  llvm::Value *result = load;

  // If we're ignoring an aggregate return, don't do anything.
  if (atomics.getEvaluationKind() == TEK_Aggregate && resultSlot.isIgnored())
    return RValue::getAggregate(nullptr, false);

  // The easiest way to do this this is to go through memory, but we
  // try not to in some easy cases.
  if (atomics.getEvaluationKind() == TEK_Scalar && !atomics.hasPadding()) {
    llvm::Type *resultTy = CGM.getTypes().ConvertTypeForMem(valueType);
    if (isa<llvm::IntegerType>(resultTy)) {
      assert(result->getType() == resultTy);
      result = EmitFromMemory(result, valueType);
    } else if (isa<llvm::PointerType>(resultTy)) {
      result = Builder.CreateIntToPtr(result, resultTy);
    } else {
      result = Builder.CreateBitCast(result, resultTy);
    }
    return RValue::get(result);
  }

  // Create a temporary.  This needs to be big enough to hold the
  // atomic integer.
  llvm::Value *temp;
  bool tempIsVolatile = false;
  CharUnits tempAlignment;
  if (atomics.getEvaluationKind() == TEK_Aggregate) {
    assert(!resultSlot.isIgnored());
    temp = resultSlot.getAddr();
    tempAlignment = atomics.getValueAlignment();
    tempIsVolatile = resultSlot.isVolatile();
  } else {
    temp = CreateMemTemp(atomics.getAtomicType(), "atomic-load-temp");
    tempAlignment = atomics.getAtomicAlignment();
  }

  // Slam the integer into the temporary.
  llvm::Value *castTemp = atomics.emitCastToAtomicIntPointer(temp);
  Builder.CreateAlignedStore(result, castTemp, tempAlignment.getQuantity())
    ->setVolatile(tempIsVolatile);

  return atomics.convertTempToRValue(temp, resultSlot, loc);
}



/// Copy an r-value into memory as part of storing to an atomic type.
/// This needs to create a bit-pattern suitable for atomic operations.
void AtomicInfo::emitCopyIntoMemory(RValue rvalue, LValue dest) const {
  // If we have an r-value, the rvalue should be of the atomic type,
  // which means that the caller is responsible for having zeroed
  // any padding.  Just do an aggregate copy of that type.
  if (rvalue.isAggregate()) {
    CGF.EmitAggregateCopy(dest.getAddress(),
                          rvalue.getAggregateAddr(),
                          getAtomicType(),
                          (rvalue.isVolatileQualified()
                           || dest.isVolatileQualified()),
                          dest.getAlignment());
    return;
  }

  // Okay, otherwise we're copying stuff.

  // Zero out the buffer if necessary.
  emitMemSetZeroIfNecessary(dest);

  // Drill past the padding if present.
  dest = projectValue(dest);

  // Okay, store the rvalue in.
  if (rvalue.isScalar()) {
    CGF.EmitStoreOfScalar(rvalue.getScalarVal(), dest, /*init*/ true);
  } else {
    CGF.EmitStoreOfComplex(rvalue.getComplexVal(), dest, /*init*/ true);
  }
}


/// Materialize an r-value into memory for the purposes of storing it
/// to an atomic type.
llvm::Value *AtomicInfo::materializeRValue(RValue rvalue) const {
  // Aggregate r-values are already in memory, and EmitAtomicStore
  // requires them to be values of the atomic type.
  if (rvalue.isAggregate())
    return rvalue.getAggregateAddr();

  // Otherwise, make a temporary and materialize into it.
  llvm::Value *temp = CGF.CreateMemTemp(getAtomicType(), "atomic-store-temp");
  LValue tempLV = CGF.MakeAddrLValue(temp, getAtomicType(), getAtomicAlignment());
  emitCopyIntoMemory(rvalue, tempLV);
  return temp;
}

/// Emit a store to an l-value of atomic type.
///
/// Note that the r-value is expected to be an r-value *of the atomic
/// type*; this means that for aggregate r-values, it should include
/// storage for any padding that was necessary.
void CodeGenFunction::EmitAtomicStore(RValue rvalue, LValue dest, bool isInit) {
  // If this is an aggregate r-value, it should agree in type except
  // maybe for address-space qualification.
  assert(!rvalue.isAggregate() ||
         rvalue.getAggregateAddr()->getType()->getPointerElementType()
           == dest.getAddress()->getType()->getPointerElementType());

  AtomicInfo atomics(*this, dest);

  // If this is an initialization, just put the value there normally.
  if (isInit) {
    atomics.emitCopyIntoMemory(rvalue, dest);
    return;
  }

  // Check whether we should use a library call.
  if (atomics.shouldUseLibcall()) {
    // Produce a source address.
    llvm::Value *srcAddr = atomics.materializeRValue(rvalue);

    // void __atomic_store(size_t size, void *mem, void *val, int order)
    CallArgList args;
    args.add(RValue::get(atomics.getAtomicSizeValue()),
             getContext().getSizeType());
    args.add(RValue::get(EmitCastToVoidPtr(dest.getAddress())),
             getContext().VoidPtrTy);
    args.add(RValue::get(EmitCastToVoidPtr(srcAddr)),
             getContext().VoidPtrTy);
    args.add(RValue::get(llvm::ConstantInt::get(
                 IntTy, AtomicExpr::AO_ABI_memory_order_seq_cst)),
             getContext().IntTy);
    emitAtomicLibcall(*this, "__atomic_store", getContext().VoidTy, args);
    return;
  }

  // Okay, we're doing this natively.
  llvm::Value *intValue;

  // If we've got a scalar value of the right size, try to avoid going
  // through memory.
  if (rvalue.isScalar() && !atomics.hasPadding()) {
    llvm::Value *value = rvalue.getScalarVal();
    if (isa<llvm::IntegerType>(value->getType())) {
      intValue = value;
    } else {
      llvm::IntegerType *inputIntTy =
        llvm::IntegerType::get(getLLVMContext(), atomics.getValueSizeInBits());
      if (isa<llvm::PointerType>(value->getType())) {
        intValue = Builder.CreatePtrToInt(value, inputIntTy);
      } else {
        intValue = Builder.CreateBitCast(value, inputIntTy);
      }
    }

  // Otherwise, we need to go through memory.
  } else {
    // Put the r-value in memory.
    llvm::Value *addr = atomics.materializeRValue(rvalue);

    // Cast the temporary to the atomic int type and pull a value out.
    addr = atomics.emitCastToAtomicIntPointer(addr);
    intValue = Builder.CreateAlignedLoad(addr,
                                 atomics.getAtomicAlignment().getQuantity());
  }

  // Do the atomic store.
  llvm::Value *addr = atomics.emitCastToAtomicIntPointer(dest.getAddress());
  llvm::StoreInst *store = Builder.CreateStore(intValue, addr);

  // Initializations don't need to be atomic.
  if (!isInit) store->setAtomic(llvm::SequentiallyConsistent);

  // Other decoration.
  store->setAlignment(dest.getAlignment().getQuantity());
  if (dest.isVolatileQualified())
    store->setVolatile(true);
  if (dest.getTBAAInfo())
    CGM.DecorateInstruction(store, dest.getTBAAInfo());
}

void CodeGenFunction::EmitAtomicInit(Expr *init, LValue dest) {
  AtomicInfo atomics(*this, dest);

  switch (atomics.getEvaluationKind()) {
  case TEK_Scalar: {
    llvm::Value *value = EmitScalarExpr(init);
    atomics.emitCopyIntoMemory(RValue::get(value), dest);
    return;
  }

  case TEK_Complex: {
    ComplexPairTy value = EmitComplexExpr(init);
    atomics.emitCopyIntoMemory(RValue::getComplex(value), dest);
    return;
  }

  case TEK_Aggregate: {
    // Fix up the destination if the initializer isn't an expression
    // of atomic type.
    bool Zeroed = false;
    if (!init->getType()->isAtomicType()) {
      Zeroed = atomics.emitMemSetZeroIfNecessary(dest);
      dest = atomics.projectValue(dest);
    }

    // Evaluate the expression directly into the destination.
    AggValueSlot slot = AggValueSlot::forLValue(dest,
                                        AggValueSlot::IsNotDestructed,
                                        AggValueSlot::DoesNotNeedGCBarriers,
                                        AggValueSlot::IsNotAliased,
                                        Zeroed ? AggValueSlot::IsZeroed :
                                                 AggValueSlot::IsNotZeroed);

    EmitAggExpr(init, slot);
    return;
  }
  }
  llvm_unreachable("bad evaluation kind");
}
