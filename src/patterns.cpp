
#include "patterns.h"

#include <llvm/IR/CallSite.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/Instructions.h>

#include <llvm/Support/raw_ostream.h>

using namespace llvm;

bool isAllocVectorOfKnownType(Value *inst, unsigned& type) {
  CallSite cs(inst);
  if (!cs) {
    return false;
  }
  Function *tgt = cs.getCalledFunction();
  if (!tgt || tgt->getName() != "Rf_allocVector") {
    return false;
  }
  Value *arg = cs.getArgument(0);
  if (!ConstantInt::classof(arg)) {
    return false;
  }
  
  ConstantInt *ctype = cast<ConstantInt>(arg);
  type = ctype->getZExtValue();
  return true;
}

bool isBitCastOfVar(Value *inst, AllocaInst*& var, Type*& type) {

  if (!BitCastInst::classof(inst)) {
    return false;
  }
  BitCastInst* bc = cast<BitCastInst>(inst);
  
  Value *lvar = bc->getOperand(0);
  if (!LoadInst::classof(lvar)) {
    return false;
  }
  Value *avar = cast<LoadInst>(lvar)->getPointerOperand();
  if (!AllocaInst::classof(avar)) {
    return false;
  }
  
  var = cast<AllocaInst>(avar);
  type = cast<Type>(bc->getDestTy());
  return true;
}

bool isTypeCheck(Value *inst, bool& positive, AllocaInst*& var, unsigned& type) {

  // %33 = load %struct.SEXPREC** %2, align 8, !dbg !21240 ; [#uses=1 type=%struct.SEXPREC*] [debug line = 1097:0]
  // %34 = getelementptr inbounds %struct.SEXPREC* %33, i32 0, i32 0, !dbg !21240 ; [#uses=1 type=%struct.sxpinfo_struct*] [debug line = 1097:0]
  // %35 = bitcast %struct.sxpinfo_struct* %34 to i32*, !dbg !21240 ; [#uses=1 type=i32*] [debug line = 1097:0]
  // %36 = load i32* %35, align 4, !dbg !21240       ; [#uses=1 type=i32] [debug line = 1097:0]
  // %37 = and i32 %36, 31, !dbg !21240              ; [#uses=1 type=i32] [debug line = 1097:0]
  // %38 = icmp eq i32 %37, 22, !dbg !21240          ; [#uses=1 type=i1] [debug line = 1097:0]

  if (!CmpInst::classof(inst)) {
    return false;
  }
  CmpInst *ci = cast<CmpInst>(inst);
  if (!ci->isEquality()) {
    return false;
  }
  
  positive = ci->isTrueWhenEqual();
  
  ConstantInt* ctype;
  BinaryOperator* andv;
  
  if (ConstantInt::classof(ci->getOperand(0)) && BinaryOperator::classof(ci->getOperand(1))) {
    ctype = cast<ConstantInt>(ci->getOperand(0));
    andv = cast<BinaryOperator>(ci->getOperand(1));
  } else if (ConstantInt::classof(ci->getOperand(1)) && BinaryOperator::classof(ci->getOperand(0))) {
    ctype = cast<ConstantInt>(ci->getOperand(1));
    andv = cast<BinaryOperator>(ci->getOperand(0));  
  } else {
    return false;
  }
  
  if (andv->getOpcode() != Instruction::And) {
    return false;
  }
  
  type = ctype->getZExtValue();
  
  LoadInst* bitsLoad;
  ConstantInt* cmask;
  
  if (LoadInst::classof(andv->getOperand(0)) && ConstantInt::classof(andv->getOperand(1))) {
    bitsLoad = cast<LoadInst>(andv->getOperand(0));
    cmask = cast<ConstantInt>(andv->getOperand(1));
  } else if (LoadInst::classof(andv->getOperand(1)) && ConstantInt::classof(andv->getOperand(0))) {
    bitsLoad = cast<LoadInst>(andv->getOperand(0));
    cmask = cast<ConstantInt>(andv->getOperand(1));
  } else {
    return false;
  } 
  
  if (cmask->getZExtValue() != 31) {
    return false;
  }
  
  if (!BitCastInst::classof(bitsLoad->getPointerOperand())) {
    return false;
  }
  Value *gepv = cast<BitCastInst>(bitsLoad->getPointerOperand())->getOperand(0);
  
  if (!GetElementPtrInst::classof(gepv)) {
    return false;
  }

  GetElementPtrInst *gep = cast<GetElementPtrInst>(gepv);
  if (!gep->isInBounds() || !gep->hasAllZeroIndices() || !isSEXP(gep->getPointerOperandType())) {
    return false;
  }
  
  if (!LoadInst::classof(gep->getPointerOperand())) {
    return false;
  }
  
  Value *varv = cast<LoadInst>(gep->getPointerOperand())->getPointerOperand();
  if (!AllocaInst::classof(varv)) {
    return false;
  }
  
  var = cast<AllocaInst>(varv);
  return true;
}

bool isCallThroughPointer(Value *inst) {
  if (CallInst* ci = dyn_cast<CallInst>(inst)) {
    return LoadInst::classof(ci->getCalledValue());
  } else {
    return false;
  }
}

ValuesSetTy valueOrigins(Value *inst) {

  ValuesSetTy origins;
  origins.insert(inst);
  bool insertedValue = true;
  
  while(insertedValue) {
    insertedValue = false;
    for(ValuesSetTy::iterator vi = origins.begin(), ve = origins.end(); vi != ve; ++vi) {
      Value *v = *vi;
      
      if (!Instruction::classof(v) || CallInst::classof(v) || InvokeInst::classof(v) || AllocaInst::classof(v)) {
        continue;
      }
      Instruction *inst = cast<Instruction>(v);
      for(Instruction::op_iterator oi = inst->op_begin(), oe = inst->op_end(); oi != oe; ++oi) {
        Value *op = *oi;
        auto vinsert = origins.insert(op);
        if (vinsert.second) {
          insertedValue = true;
        }
      }
    }
  }
  return origins;
}

// check if value inst origins from a load of variable var
//   it may directly be the load of var
//   but it may also be a result of a number of non-load and non-call instructions

AllocaInst* originsOnlyFromLoad(Value *inst) {

  if (LoadInst *l = dyn_cast<LoadInst>(inst)) {
    if (AllocaInst *lv = dyn_cast<AllocaInst>(l->getPointerOperand())) { // fast path
      return lv;
    }
  }

  ValuesSetTy origins = valueOrigins(inst);
  
  AllocaInst* onlyVar = NULL;
  for(ValuesSetTy::iterator vi = origins.begin(), ve = origins.end(); vi != ve; ++vi) {
    Value *v = *vi;
    if (CallInst::classof(v) || InvokeInst::classof(v)) {
      return NULL;
    }
    if (AllocaInst *curVar = dyn_cast<AllocaInst>(v)) {
      if (!onlyVar) {
        onlyVar = curVar;
      } else {
        if (onlyVar != curVar) {
          // multiple origins
          return NULL;
        }
      }
    }
    // FIXME: need to handle anything more?
  }
  return onlyVar;
}


