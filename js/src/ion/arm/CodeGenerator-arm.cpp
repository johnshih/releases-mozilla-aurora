/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-
 * vim: set ts=4 sw=4 et tw=99:
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/. */

#include "CodeGenerator-arm.h"
#include "ion/shared/CodeGenerator-shared-inl.h"
#include "ion/MIR.h"
#include "ion/MIRGraph.h"
#include "jsnum.h"
#include "jsscope.h"
#include "jsscriptinlines.h"

#include "jscntxt.h"
#include "jscompartment.h"
#include "ion/IonFrames.h"
#include "ion/MoveEmitter.h"
#include "ion/IonCompartment.h"

#include "jsscopeinlines.h"

using namespace js;
using namespace js::ion;

class DeferredJumpTable : public DeferredData
{
    LTableSwitch *lswitch;
    BufferOffset off;
    MacroAssembler *masm;
  public:
    DeferredJumpTable(LTableSwitch *lswitch, BufferOffset off_, MacroAssembler *masm_)
      : lswitch(lswitch), off(off_), masm(masm_)
    { }

    void copy(IonCode *code, uint8 *ignore__) const {
        void **jumpData = (void **)(((char*)code->raw()) + masm->actualOffset(off).getOffset());
        int numCases =  lswitch->mir()->numCases();
        // For every case write the pointer to the start in the table
        for (int j = 0; j < numCases; j++) {
            LBlock *caseblock = lswitch->mir()->getCase(numCases - 1 - j)->lir();
            Label *caseheader = caseblock->label();

            uint32 offset = caseheader->offset();
            *jumpData = (void *)(code->raw() + masm->actualOffset(offset));
            jumpData++;
        }
    }
};


// shared
CodeGeneratorARM::CodeGeneratorARM(MIRGenerator *gen, LIRGraph &graph)
  : CodeGeneratorShared(gen, graph),
    deoptLabel_(NULL)
{
}

bool
CodeGeneratorARM::generatePrologue()
{
    // Note that this automatically sets MacroAssembler::framePushed().
    masm.reserveStack(frameSize());
    masm.checkStackAlignment();
    // Allocate returnLabel_ on the heap, so we don't run its destructor and
    // assert-not-bound in debug mode on compilation failure.
    returnLabel_ = new HeapLabel();

    return true;
}

bool
CodeGeneratorARM::generateEpilogue()
{
    masm.bind(returnLabel_);

    // Pop the stack we allocated at the start of the function.
    masm.freeStack(frameSize());
    JS_ASSERT(masm.framePushed() == 0);

    masm.ma_pop(pc);
    masm.dumpPool();
    return true;
}

void
CodeGeneratorARM::emitBranch(Assembler::Condition cond, MBasicBlock *mirTrue, MBasicBlock *mirFalse)
{
    LBlock *ifTrue = mirTrue->lir();
    LBlock *ifFalse = mirFalse->lir();
    if (isNextBlock(ifFalse)) {
        masm.ma_b(ifTrue->label(), cond);
    } else {
        masm.ma_b(ifFalse->label(), Assembler::InvertCondition(cond));
        if (!isNextBlock(ifTrue)) {
            masm.ma_b(ifTrue->label());
        }
    }
}


bool
OutOfLineBailout::accept(CodeGeneratorARM *codegen)
{
    return codegen->visitOutOfLineBailout(this);
}

bool
CodeGeneratorARM::visitTestIAndBranch(LTestIAndBranch *test)
{
    const LAllocation *opd = test->getOperand(0);
    LBlock *ifTrue = test->ifTrue()->lir();
    LBlock *ifFalse = test->ifFalse()->lir();

    // Test the operand
    masm.ma_cmp(ToRegister(opd), Imm32(0));

    if (isNextBlock(ifFalse)) {
        masm.ma_b(ifTrue->label(), Assembler::NonZero);
    } else if (isNextBlock(ifTrue)) {
        masm.ma_b(ifFalse->label(), Assembler::Zero);
    } else {
        masm.ma_b(ifFalse->label(), Assembler::Zero);
        masm.ma_b(ifTrue->label());
    }
    return true;
}

void
CodeGeneratorARM::emitSet(Assembler::Condition cond, const Register &dest)
{
    masm.ma_mov(Imm32(0), dest);
    masm.ma_mov(Imm32(1), dest, NoSetCond, cond);
}

bool
CodeGeneratorARM::visitCompare(LCompare *comp)
{
    const LAllocation *left = comp->getOperand(0);
    const LAllocation *right = comp->getOperand(1);
    const LDefinition *def = comp->getDef(0);

    if (right->isConstant())
        masm.ma_cmp(ToRegister(left), Imm32(ToInt32(right)));
    else
        masm.ma_cmp(ToRegister(left), ToOperand(right));
    masm.ma_mov(Imm32(0), ToRegister(def));
    masm.ma_mov(Imm32(1), ToRegister(def), NoSetCond, JSOpToCondition(comp->jsop()));
    return true;
}

bool
CodeGeneratorARM::visitCompareAndBranch(LCompareAndBranch *comp)
{
    Assembler::Condition cond = JSOpToCondition(comp->jsop());
    if (comp->right()->isConstant())
        masm.ma_cmp(ToRegister(comp->left()), Imm32(ToInt32(comp->right())));
    else
        masm.ma_cmp(ToRegister(comp->left()), ToOperand(comp->right()));
    emitBranch(cond, comp->ifTrue(), comp->ifFalse());
    return true;

}

bool
CodeGeneratorARM::generateOutOfLineCode()
{
    if (!CodeGeneratorShared::generateOutOfLineCode())
        return false;

    if (deoptLabel_) {
        // All non-table-based bailouts will go here.
        masm.bind(deoptLabel_);

        // Push the frame size, so the handler can recover the IonScript.
        masm.ma_mov(Imm32(frameSize()), lr);

        JSContext *cx = GetIonContext()->cx;
        IonCompartment *ion = cx->compartment->ionCompartment();
        IonCode *handler = ion->getGenericBailoutHandler(cx);
        if (!handler)
            return false;
        masm.branch(handler);
    }

    return true;
}

bool
CodeGeneratorARM::bailoutIf(Assembler::Condition condition, LSnapshot *snapshot)
{
    if (!encode(snapshot))
        return false;

    // Though the assembler doesn't track all frame pushes, at least make sure
    // the known value makes sense. We can't use bailout tables if the stack
    // isn't properly aligned to the static frame size.
    JS_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                 frameClass_.frameSize() == masm.framePushed());

    if (assignBailoutId(snapshot)) {
        uint8 *code = deoptTable_->raw() + snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE;
        masm.ma_b(code, Relocation::HARDCODED, condition);
        return true;
    }

    // We could not use a jump table, either because all bailout IDs were
    // reserved, or a jump table is not optimal for this frame size or
    // platform. Whatever, we will generate a lazy bailout.
    OutOfLineBailout *ool = new OutOfLineBailout(snapshot, masm.framePushed());
    if (!addOutOfLineCode(ool))
        return false;

    masm.ma_b(ool->entry(), condition);

    return true;
}
bool
CodeGeneratorARM::bailoutFrom(Label *label, LSnapshot *snapshot)
{
    JS_ASSERT(label->used() && !label->bound());
    if (!encode(snapshot))
        return false;

    // Though the assembler doesn't track all frame pushes, at least make sure
    // the known value makes sense. We can't use bailout tables if the stack
    // isn't properly aligned to the static frame size.
    JS_ASSERT_IF(frameClass_ != FrameSizeClass::None(),
                 frameClass_.frameSize() == masm.framePushed());
    // This involves retargeting a label, which I've declared is always going
    // to be a pc-relative branch to an absolute address!
    // With no assurance that this is going to be a local branch, I am wary to
    // implement this.  Moreover, If it isn't a local branch, it will be large
    // and possibly slow.  I believe that the correct way to handle this is to
    // subclass label into a fatlabel, where we generate enough room for a load
    // before the branch
#if 0
    if (assignBailoutId(snapshot)) {
        uint8 *code = deoptTable_->raw() + snapshot->bailoutId() * BAILOUT_TABLE_ENTRY_SIZE;
        masm.retarget(label, code, Relocation::HARDCODED);
        return true;
    }
#endif
    // We could not use a jump table, either because all bailout IDs were
    // reserved, or a jump table is not optimal for this frame size or
    // platform. Whatever, we will generate a lazy bailout.
    OutOfLineBailout *ool = new OutOfLineBailout(snapshot, masm.framePushed());
    if (!addOutOfLineCode(ool)) {
        return false;
    }

    masm.retarget(label, ool->entry());

    return true;
}

bool
CodeGeneratorARM::bailout(LSnapshot *snapshot)
{
    Label label;
    masm.ma_b(&label);
    return bailoutFrom(&label, snapshot);
}

bool
CodeGeneratorARM::visitOutOfLineBailout(OutOfLineBailout *ool)
{
    if (!deoptLabel_)
        deoptLabel_ = new HeapLabel();
    masm.ma_mov(Imm32(ool->snapshot()->snapshotOffset()), ScratchRegister);
    masm.ma_push(ScratchRegister);
    masm.ma_push(ScratchRegister);
    masm.ma_b(deoptLabel_);
    return true;
}

bool
CodeGeneratorARM::visitMinMaxD(LMinMaxD *ins)
{
    FloatRegister first = ToFloatRegister(ins->first());
    FloatRegister second = ToFloatRegister(ins->second());
    FloatRegister output = ToFloatRegister(ins->output());

    JS_ASSERT(first == output);

    Assembler::Condition cond = ins->mir()->isMax()
        ? Assembler::VFP_LessThanOrEqual
        : Assembler::VFP_GreaterThanOrEqual;
    Label nan, equal, returnSecond, done;

    masm.compareDouble(first, second);
    masm.ma_b(&nan, Assembler::VFP_Unordered); // first or second is NaN, result is NaN.
    masm.ma_b(&equal, Assembler::VFP_Equal); // make sure we handle -0 and 0 right.
    masm.ma_b(&returnSecond, cond);
    masm.ma_b(&done);

    // Check for zero.
    masm.bind(&equal);
    masm.compareDouble(first, InvalidFloatReg);
    masm.ma_b(&done, Assembler::VFP_NotEqualOrUnordered); // first wasn't 0 or -0, so just return it.
    // So now both operands are either -0 or 0.
    if (ins->mir()->isMax()) {
        masm.ma_vadd(second, first, first); // -0 + -0 = -0 and -0 + 0 = 0.
    } else {
        masm.ma_vneg(first, first);
        masm.ma_vsub(first, second, first);
        masm.ma_vneg(first, first);
    }
    masm.ma_b(&done);

    masm.bind(&nan);
    masm.loadStaticDouble(&js_NaN, output);
    masm.ma_b(&done);

    masm.bind(&returnSecond);
    masm.ma_vmov(second, output);

    masm.bind(&done);
    return true;
}

bool
CodeGeneratorARM::visitAbsD(LAbsD *ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    JS_ASSERT(input == ToFloatRegister(ins->output()));
    masm.as_vabs(input, input);
    return true;
}

bool
CodeGeneratorARM::visitSqrtD(LSqrtD *ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    JS_ASSERT(input == ToFloatRegister(ins->output()));
    masm.as_vsqrt(input, input);
    return true;
}

bool
CodeGeneratorARM::visitAddI(LAddI *ins)
{
    const LAllocation *lhs = ins->getOperand(0);
    const LAllocation *rhs = ins->getOperand(1);
    const LDefinition *dest = ins->getDef(0);

    if (rhs->isConstant()) {
        masm.ma_add(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest), SetCond);
    } else {
        masm.ma_add(ToRegister(lhs), ToOperand(rhs), ToRegister(dest), SetCond);
    }

    if (ins->snapshot() && !bailoutIf(Assembler::Overflow, ins->snapshot()))
        return false;

    return true;
}

bool
CodeGeneratorARM::visitSubI(LSubI *ins)
{
    const LAllocation *lhs = ins->getOperand(0);
    const LAllocation *rhs = ins->getOperand(1);
    const LDefinition *dest = ins->getDef(0);
    if (rhs->isConstant()) {
        masm.ma_sub(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest), SetCond);
    } else {
        masm.ma_sub(ToRegister(lhs), ToOperand(rhs), ToRegister(dest), SetCond);
    }

    if (ins->snapshot() && !bailoutIf(Assembler::Overflow, ins->snapshot()))
        return false;
    return true;
}

bool
CodeGeneratorARM::visitMulI(LMulI *ins)
{
    const LAllocation *lhs = ins->getOperand(0);
    const LAllocation *rhs = ins->getOperand(1);
    const LDefinition *dest = ins->getDef(0);
    MMul *mul = ins->mir();

    if (rhs->isConstant()) {
        // Bailout when this condition is met.
        Assembler::Condition c = Assembler::Overflow;
        // Bailout on -0.0
        int32 constant = ToInt32(rhs);
        if (mul->canBeNegativeZero() && constant <= 0) {
            Assembler::Condition bailoutCond = (constant == 0) ? Assembler::LessThan : Assembler::Equal;
            masm.ma_cmp(ToRegister(lhs), Imm32(0));
            if (!bailoutIf(bailoutCond, ins->snapshot()))
                    return false;
        }
        // TODO: move these to ma_mul.
        switch (constant) {
          case -1:
            masm.ma_rsb(ToRegister(lhs), Imm32(0), ToRegister(dest), SetCond);
            break;
          case 0:
            masm.ma_mov(Imm32(0), ToRegister(dest));
            return true; // escape overflow check;
          case 1:
            // nop
            masm.ma_mov(ToRegister(lhs), ToRegister(dest));
            return true; // escape overflow check;
          case 2:
            masm.ma_add(ToRegister(lhs), ToRegister(lhs), ToRegister(dest), SetCond);
            // Overflow is handled later.
            break;
          default: {
            bool handled = false;
            if (!mul->canOverflow()) {
                // If it cannot overflow, we can do lots of optimizations
                Register src = ToRegister(lhs);
                uint32 shift;
                JS_FLOOR_LOG2(shift, constant);
                uint32 rest = constant - (1 << shift);
                // See if the constant has one bit set, meaning it can be encoded as a bitshift
                if ((1 << shift) == constant) {
                    masm.ma_lsl(Imm32(shift), src, ToRegister(dest));
                    handled = true;
                } else {
                    // If the constant cannot be encoded as (1<<C1), see if it can be encoded as
                    // (1<<C1) | (1<<C2), which can be computed using an add and a shift
                    uint32 shift_rest;
                    JS_FLOOR_LOG2(shift_rest, rest);
                    if ((1u << shift_rest) == rest) {
                        masm.as_add(ToRegister(dest), src, lsl(src, shift-shift_rest));
                        if (shift_rest != 0)
                            masm.ma_lsl(Imm32(shift_rest), ToRegister(dest), ToRegister(dest));
                        handled = true;
                    }
                }
            } else if (ToRegister(lhs) != ToRegister(dest)) {
                // To stay on the safe side, only optimize things that are a
                // power of 2.

                uint32 shift;
                JS_FLOOR_LOG2(shift, constant);
                if ((1 << shift) == constant) {
                    // dest = lhs * pow(2,shift)
                    masm.ma_lsl(Imm32(shift), ToRegister(lhs), ToRegister(dest));
                    // At runtime, check (lhs == dest >> shift), if this does not hold,
                    // some bits were lost due to overflow, and the computation should
                    // be resumed as a double.
                    masm.as_cmp(ToRegister(lhs), asr(ToRegister(dest), shift));
                    c = Assembler::NotEqual;
                    handled = true;
                }
            }

            if (!handled) {
                if (mul->canOverflow()) {
                    c = masm.ma_check_mul(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest), c);
                } else {
                    masm.ma_mul(ToRegister(lhs), Imm32(ToInt32(rhs)), ToRegister(dest));
                }
            }
        }
        }
        // Bailout on overflow
        if (mul->canOverflow() && !bailoutIf(c, ins->snapshot()))
            return false;
    } else {
        Assembler::Condition c = Assembler::Overflow;

        //masm.imull(ToOperand(rhs), ToRegister(lhs));
        if (mul->canOverflow())
            c = masm.ma_check_mul(ToRegister(lhs), ToRegister(rhs), ToRegister(dest), c);
        else
            masm.ma_mul(ToRegister(lhs), ToRegister(rhs), ToRegister(dest));

        // Bailout on overflow
        if (mul->canOverflow() && !bailoutIf(c, ins->snapshot()))
            return false;

        if (mul->canBeNegativeZero()) {
            Label done;
            masm.ma_cmp(ToRegister(dest), Imm32(0));
            masm.ma_b(&done, Assembler::NotEqual);

            // Result is -0 if lhs or rhs is negative.
            masm.ma_cmn(ToRegister(lhs), ToRegister(rhs));
            if (!bailoutIf(Assembler::Signed, ins->snapshot()))
                return false;

            masm.bind(&done);
        }
    }

    return true;
}

extern "C" {
    extern int __aeabi_idivmod(int,int);
}

bool
CodeGeneratorARM::visitDivI(LDivI *ins)
{
    // Extract the registers from this instruction
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    MDiv *mir = ins->mir();
    if (mir->canBeNegativeOverflow()) {
        // Prevent INT_MIN / -1;
        // The integer division will give INT_MIN, but we want -(double)INT_MIN.
        masm.ma_cmp(lhs, Imm32(INT_MIN)); // sets EQ if lhs == INT_MIN
        masm.ma_cmp(rhs, Imm32(-1), Assembler::Equal); // if EQ (LHS == INT_MIN), sets EQ if rhs == -1
        if (!bailoutIf(Assembler::Equal, ins->snapshot()))
            return false;
    }

    // 0/X (with X < 0) is bad because both of these values *should* be doubles, and
    // the result should be -0.0, which cannot be represented in integers.
    // X/0 is bad because it will give garbage (or abort), when it should give
    // either \infty, -\infty or NAN.

    // Prevent 0 / X (with X < 0) and X / 0
    // testing X / Y.  Compare Y with 0.
    // There are three cases: (Y < 0), (Y == 0) and (Y > 0)
    // If (Y < 0), then we compare X with 0, and bail if X == 0
    // If (Y == 0), then we simply want to bail.  Since this does not set
    // the flags necessary for LT to trigger, we don't test X, and take the
    // bailout because the EQ flag is set.
    // if (Y > 0), we don't set EQ, and we don't trigger LT, so we don't take the bailout.
    if (mir->canBeDivideByZero() || mir->canBeNegativeZero()) {
        masm.ma_cmp(rhs, Imm32(0));
        masm.ma_cmp(lhs, Imm32(0), Assembler::LessThan);
        if (!bailoutIf(Assembler::Equal, ins->snapshot()))
            return false;
    }
    masm.setupAlignedABICall(2);
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, __aeabi_idivmod));
    // idivmod returns the quotient in r0, and the remainder in r1.
    if (!mir->isTruncated()) {
        masm.ma_cmp(r1, Imm32(0));
        if (!bailoutIf(Assembler::NonZero, ins->snapshot()))
            return false;
    }
    return true;
}

bool
CodeGeneratorARM::visitModI(LModI *ins)
{
    // Extract the registers from this instruction
    Register lhs = ToRegister(ins->lhs());
    Register rhs = ToRegister(ins->rhs());
    Register callTemp = ToRegister(ins->getTemp(2));
    // save the lhs in case we end up with a 0 that should be a -0.0 because lhs < 0.
    JS_ASSERT(callTemp.code() > r3.code() && callTemp.code() < r12.code());
    masm.ma_mov(lhs, callTemp);
    // Prevent INT_MIN % -1;
    // The integer division will give INT_MIN, but we want -(double)INT_MIN.
    masm.ma_cmp(lhs, Imm32(INT_MIN)); // sets EQ if lhs == INT_MIN
    masm.ma_cmp(rhs, Imm32(-1), Assembler::Equal); // if EQ (LHS == INT_MIN), sets EQ if rhs == -1
    if (!bailoutIf(Assembler::Equal, ins->snapshot()))
        return false;
    // 0/X (with X < 0) is bad because both of these values *should* be doubles, and
    // the result should be -0.0, which cannot be represented in integers.
    // X/0 is bad because it will give garbage (or abort), when it should give
    // either \infty, -\infty or NAN.

    // Prevent 0 / X (with X < 0) and X / 0
    // testing X / Y.  Compare Y with 0.
    // There are three cases: (Y < 0), (Y == 0) and (Y > 0)
    // If (Y < 0), then we compare X with 0, and bail if X == 0
    // If (Y == 0), then we simply want to bail.  Since this does not set
    // the flags necessary for LT to trigger, we don't test X, and take the
    // bailout because the EQ flag is set.
    // if (Y > 0), we don't set EQ, and we don't trigger LT, so we don't take the bailout.
    masm.ma_cmp(rhs, Imm32(0));
    masm.ma_cmp(lhs, Imm32(0), Assembler::LessThan);
    if (!bailoutIf(Assembler::Equal, ins->snapshot()))
        return false;
    masm.setupAlignedABICall(2);
    masm.passABIArg(lhs);
    masm.passABIArg(rhs);
    masm.callWithABI(JS_FUNC_TO_DATA_PTR(void *, __aeabi_idivmod));
    // If X%Y == 0 and X < 0, then we *actually* wanted to return -0.0
    Label join;
    // See if X < 0
    masm.ma_cmp(r1, Imm32(0));
    masm.ma_b(&join, Assembler::NotEqual);
    masm.ma_cmp(callTemp, Imm32(0));
    if (!bailoutIf(Assembler::Signed, ins->snapshot()))
        return false;
    masm.bind(&join);
    return true;
}

bool
CodeGeneratorARM::visitModPowTwoI(LModPowTwoI *ins)
{
    Register in = ToRegister(ins->getOperand(0));
    Register out = ToRegister(ins->getDef(0));
    Label fin;
    // bug 739870, jbramley has a different sequence that may help with speed here
    masm.ma_mov(in, out, SetCond);
    masm.ma_b(&fin, Assembler::Zero);
    masm.ma_rsb(Imm32(0), out, NoSetCond, Assembler::Signed);
    masm.ma_and(Imm32((1<<ins->shift())-1), out);
    masm.ma_rsb(Imm32(0), out, SetCond, Assembler::Signed);
    if (!bailoutIf(Assembler::Zero, ins->snapshot())) {
        return false;
    }
    masm.bind(&fin);
    return true;
}

bool
CodeGeneratorARM::visitModMaskI(LModMaskI *ins)
{
    Register src = ToRegister(ins->getOperand(0));
    Register dest = ToRegister(ins->getDef(0));
    Register tmp = ToRegister(ins->getTemp(0));
    masm.ma_mod_mask(src, dest, tmp, ins->shift());
    if (!bailoutIf(Assembler::Zero, ins->snapshot())) {
        return false;
    }
    return true;
}
bool
CodeGeneratorARM::visitBitNotI(LBitNotI *ins)
{
    const LAllocation *input = ins->getOperand(0);
    const LDefinition *dest = ins->getDef(0);
    // this will not actually be true on arm.
    // We can not an imm8m in order to get a wider range
    // of numbers
    JS_ASSERT(!input->isConstant());

    masm.ma_mvn(ToRegister(input), ToRegister(dest));
    return true;
}

bool
CodeGeneratorARM::visitBitOpI(LBitOpI *ins)
{
    const LAllocation *lhs = ins->getOperand(0);
    const LAllocation *rhs = ins->getOperand(1);
    const LDefinition *dest = ins->getDef(0);
    // all of these bitops should be either imm32's, or integer registers.
    switch (ins->bitop()) {
      case JSOP_BITOR:
        if (rhs->isConstant()) {
            masm.ma_orr(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest));
        } else {
            masm.ma_orr(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
        }
        break;
      case JSOP_BITXOR:
        if (rhs->isConstant()) {
            masm.ma_eor(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest));
        } else {
            masm.ma_eor(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
        }
        break;
      case JSOP_BITAND:
        if (rhs->isConstant()) {
            masm.ma_and(Imm32(ToInt32(rhs)), ToRegister(lhs), ToRegister(dest));
        } else {
            masm.ma_and(ToRegister(rhs), ToRegister(lhs), ToRegister(dest));
        }
        break;
      default:
        JS_NOT_REACHED("unexpected binary opcode");
    }

    return true;
}

bool
CodeGeneratorARM::visitShiftI(LShiftI *ins)
{
    Register lhs = ToRegister(ins->lhs());
    const LAllocation *rhs = ins->rhs();
    Register dest = ToRegister(ins->output());

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1F;
        switch (ins->bitop()) {
          case JSOP_LSH:
            if (shift)
                masm.ma_lsl(Imm32(shift), lhs, dest);
            else
                masm.ma_mov(lhs, dest);
            break;
          case JSOP_RSH:
            if (shift)
                masm.ma_asr(Imm32(shift), lhs, dest);
            else
                masm.ma_mov(lhs, dest);
            break;
          case JSOP_URSH:
            if (shift) {
                masm.ma_lsr(Imm32(shift), lhs, dest);
            } else {
                // x >>> 0 can overflow.
                masm.ma_mov(lhs, dest);
                if (ins->mir()->toUrsh()->canOverflow()) {
                    masm.ma_cmp(dest, Imm32(0));
                    if (!bailoutIf(Assembler::LessThan, ins->snapshot()))
                        return false;
                }
            }
            break;
          default:
            JS_NOT_REACHED("Unexpected shift op");
        }
    } else {
        // The shift amounts should be AND'ed into the 0-31 range since arm
        // shifts by the lower byte of the register (it will attempt to shift
        // by 250 if you ask it to).
        masm.ma_and(Imm32(0x1F), ToRegister(rhs), dest);

        switch (ins->bitop()) {
          case JSOP_LSH:
            masm.ma_lsl(dest, lhs, dest);
            break;
          case JSOP_RSH:
            masm.ma_asr(dest, lhs, dest);
            break;
          case JSOP_URSH:
            masm.ma_lsr(dest, lhs, dest);
            if (ins->mir()->toUrsh()->canOverflow()) {
                // x >>> 0 can overflow.
                masm.ma_cmp(dest, Imm32(0));
                if (!bailoutIf(Assembler::LessThan, ins->snapshot()))
                    return false;
            }
            break;
          default:
            JS_NOT_REACHED("Unexpected shift op");
        }
    }

    return true;
}

bool
CodeGeneratorARM::visitUrshD(LUrshD *ins)
{
    Register lhs = ToRegister(ins->lhs());
    Register temp = ToRegister(ins->temp());

    const LAllocation *rhs = ins->rhs();
    FloatRegister out = ToFloatRegister(ins->output());

    if (rhs->isConstant()) {
        int32_t shift = ToInt32(rhs) & 0x1F;
        if (shift)
            masm.ma_lsr(Imm32(shift), lhs, temp);
        else
            masm.ma_mov(lhs, temp);
    } else {
        masm.ma_and(Imm32(0x1F), ToRegister(rhs), temp);
        masm.ma_lsr(temp, lhs, temp);
    }

    masm.convertUInt32ToDouble(temp, out);
    return true;
}

bool
CodeGeneratorARM::visitPowHalfD(LPowHalfD *ins)
{
    FloatRegister input = ToFloatRegister(ins->input());
    FloatRegister output = ToFloatRegister(ins->output());

    Label done;

    // Masm.pow(-Infinity, 0.5) == Infinity.
    masm.ma_vimm(js_NegativeInfinity, ScratchFloatReg);
    masm.compareDouble(input, ScratchFloatReg);
    masm.as_vneg(ScratchFloatReg, output, Assembler::Equal);
    masm.ma_b(&done, Assembler::Equal);

    // Math.pow(-0, 0.5) == 0 == Math.pow(0, 0.5). Adding 0 converts any -0 to 0.
    masm.ma_vimm(0.0, ScratchFloatReg);
    masm.ma_vadd(ScratchFloatReg, input, output);
    masm.as_vsqrt(output, output);

    masm.bind(&done);
    return true;
}

typedef MoveResolver::MoveOperand MoveOperand;

MoveOperand
CodeGeneratorARM::toMoveOperand(const LAllocation *a) const
{
    if (a->isGeneralReg())
        return MoveOperand(ToRegister(a));
    if (a->isFloatReg())
        return MoveOperand(ToFloatRegister(a));
    return MoveOperand(StackPointer, ToStackOffset(a));
}

bool
CodeGeneratorARM::visitMoveGroup(LMoveGroup *group)
{
    if (!group->numMoves())
        return true;

    MoveResolver &resolver = masm.moveResolver();

    for (size_t i = 0; i < group->numMoves(); i++) {
        const LMove &move = group->getMove(i);

        const LAllocation *from = move.from();
        const LAllocation *to = move.to();

        // No bogus moves.
        JS_ASSERT(*from != *to);
        JS_ASSERT(!from->isConstant());
        JS_ASSERT(from->isDouble() == to->isDouble());

        MoveResolver::Move::Kind kind = from->isDouble()
                                        ? MoveResolver::Move::DOUBLE
                                        : MoveResolver::Move::GENERAL;

        if (!resolver.addMove(toMoveOperand(from), toMoveOperand(to), kind)) {
            return false;
        }
    }

    if (!resolver.resolve())
        return false;

    MoveEmitter emitter(masm);
    emitter.emit(resolver);
    emitter.finish();

    return true;
}

bool
CodeGeneratorARM::visitTableSwitch(LTableSwitch *ins)
{
    // the code generated by this is utter hax.
    // the end result looks something like:
    // SUBS index, input, #base
    // RSBSPL index, index, #max
    // LDRPL pc, pc, index lsl 2
    // B default

    // If the range of targets in N through M, we first subtract off the lowest
    // case (N), which both shifts the arguments into the range 0 to (M-N) with
    // and sets the MInus flag if the argument was out of range on the low end.

    // Then we a reverse subtract with the size of the jump table, which will
    // reverse the order of range (It is size through 0, rather than 0 through
    // size).  The main purpose of this is that we set the same flag as the lower
    // bound check for the upper bound check.  Lastly, we do this conditionally
    // on the previous check succeeding.

    // Then we conditionally load the pc offset by the (reversed) index (times
    // the address size) into the pc, which branches to the correct case.
    // NOTE: when we go to read the pc, the value that we get back is the pc of
    // the current instruction *PLUS 8*.  This means that ldr foo, [pc, +0]
    // reads $pc+8.  In other words, there is an empty word after the branch into
    // the switch table before the table actually starts.  Since the only other
    // unhandled case is the default case (both out of range high and out of range low)
    // I then insert a branch to default case into the extra slot, which ensures
    // we don't attempt to execute the address table.
    MTableSwitch *mir = ins->mir();
        Label *defaultcase = mir->getDefault()->lir()->label();
    const LAllocation *temp;

    if (ins->index()->isDouble()) {
        temp = ins->tempInt();

        // The input is a double, so try and convert it to an integer.
        // If it does not fit in an integer, take the default case.
        emitDoubleToInt32(ToFloatRegister(ins->index()), ToRegister(temp), defaultcase, false);
    } else {
        temp = ins->index();
    }

    int32 cases = mir->numCases();
    Register tempReg = ToRegister(temp);
    // Lower value with low value
    masm.ma_sub(tempReg, Imm32(mir->low()), tempReg, SetCond);
    masm.ma_rsb(tempReg, Imm32(cases - 1), tempReg, SetCond, Assembler::Unsigned);
    AutoForbidPools afp(&masm);
    masm.ma_ldr(DTRAddr(pc, DtrRegImmShift(tempReg, LSL, 2)), pc, Offset, Assembler::Unsigned);
    masm.ma_b(defaultcase);
    DeferredJumpTable *d = new DeferredJumpTable(ins, masm.nextOffset(), &masm);
    masm.as_jumpPool(cases);

    if (!masm.addDeferredData(d, 0))
        return false;

    return true;
}

bool
CodeGeneratorARM::visitMathD(LMathD *math)
{
    const LAllocation *src1 = math->getOperand(0);
    const LAllocation *src2 = math->getOperand(1);
    const LDefinition *output = math->getDef(0);
    
    switch (math->jsop()) {
      case JSOP_ADD:
          masm.ma_vadd(ToFloatRegister(src1), ToFloatRegister(src2), ToFloatRegister(output));
          break;
      case JSOP_SUB:
          masm.ma_vsub(ToFloatRegister(src1), ToFloatRegister(src2), ToFloatRegister(output));
          break;
      case JSOP_MUL:
          masm.ma_vmul(ToFloatRegister(src1), ToFloatRegister(src2), ToFloatRegister(output));
          break;
      case JSOP_DIV:
          masm.ma_vdiv(ToFloatRegister(src1), ToFloatRegister(src2), ToFloatRegister(output));
          break;
      default:
        JS_NOT_REACHED("unexpected opcode");
        return false;
    }
    return true;
}

bool
CodeGeneratorARM::visitFloor(LFloor *lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    Label bail;
    masm.floor(input, output, &bail);
    if (!bailoutFrom(&bail, lir->snapshot()))
        return false;
    return true;
}

bool
CodeGeneratorARM::visitRound(LRound *lir)
{
    FloatRegister input = ToFloatRegister(lir->input());
    Register output = ToRegister(lir->output());
    FloatRegister tmp = ToFloatRegister(lir->temp());
    Label bail;
    // Output is either correct, or clamped.  All -0 cases have been translated to a clamped
    // case.a
    masm.round(input, output, &bail, tmp);
    if (!bailoutFrom(&bail, lir->snapshot()))
        return false;
    return true;
}

// Checks whether a double is representable as a 32-bit integer. If so, the
// integer is written to the output register. Otherwise, a bailout is taken to
// the given snapshot. This function overwrites the scratch float register.
void
CodeGeneratorARM::emitDoubleToInt32(const FloatRegister &src, const Register &dest, Label *fail, bool negativeZeroCheck)
{
    // convert the floating point value to an integer, if it did not fit,
    //     then when we convert it *back* to  a float, it will have a
    //     different value, which we can test.
    masm.ma_vcvt_F64_I32(src, ScratchFloatReg);
    // move the value into the dest register.
    masm.ma_vxfer(ScratchFloatReg, dest);
    masm.ma_vcvt_I32_F64(ScratchFloatReg, ScratchFloatReg);
    masm.ma_vcmp(src, ScratchFloatReg);
    masm.as_vmrs(pc);
    masm.ma_b(fail, Assembler::VFP_NotEqualOrUnordered);
    // If they're equal, test for 0.  It would be nicer to test for -0.0 explicitly, but that seems hard.
    if (negativeZeroCheck) {
        masm.ma_cmp(dest, Imm32(0));
        masm.ma_b(fail, Assembler::Equal);
        // guard for != 0.
    }
}

void
CodeGeneratorARM::emitRoundDouble(const FloatRegister &src, const Register &dest, Label *fail)
{
    masm.ma_vcvt_F64_I32(src, ScratchFloatReg);
    masm.ma_vxfer(ScratchFloatReg, dest);
    masm.ma_cmp(dest, Imm32(0x7fffffff));
    masm.ma_cmp(dest, Imm32(0x80000000), Assembler::NotEqual);
    masm.ma_b(fail, Assembler::Equal);
}

bool
CodeGeneratorARM::visitTruncateDToInt32(LTruncateDToInt32 *ins)
{
    return emitTruncateDouble(ToFloatRegister(ins->input()), ToRegister(ins->output()));
}

// The first two size classes are 128 and 256 bytes respectively. After that we
// increment by 512.
static const uint32 LAST_FRAME_SIZE = 512;
static const uint32 LAST_FRAME_INCREMENT = 512;
static const uint32 FrameSizes[] = { 128, 256, LAST_FRAME_SIZE };

FrameSizeClass
FrameSizeClass::FromDepth(uint32 frameDepth)
{
    for (uint32 i = 0; i < JS_ARRAY_LENGTH(FrameSizes); i++) {
        if (frameDepth < FrameSizes[i])
            return FrameSizeClass(i);
    }

    uint32 newFrameSize = frameDepth - LAST_FRAME_SIZE;
    uint32 sizeClass = (newFrameSize / LAST_FRAME_INCREMENT) + 1;

    return FrameSizeClass(JS_ARRAY_LENGTH(FrameSizes) + sizeClass);
}
uint32
FrameSizeClass::frameSize() const
{
    JS_ASSERT(class_ != NO_FRAME_SIZE_CLASS_ID);

    if (class_ < JS_ARRAY_LENGTH(FrameSizes))
        return FrameSizes[class_];

    uint32 step = class_ - JS_ARRAY_LENGTH(FrameSizes);
    return LAST_FRAME_SIZE + step * LAST_FRAME_INCREMENT;
}

ValueOperand
CodeGeneratorARM::ToValue(LInstruction *ins, size_t pos)
{
    Register typeReg = ToRegister(ins->getOperand(pos + TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getOperand(pos + PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

ValueOperand
CodeGeneratorARM::ToOutValue(LInstruction *ins)
{
    Register typeReg = ToRegister(ins->getDef(TYPE_INDEX));
    Register payloadReg = ToRegister(ins->getDef(PAYLOAD_INDEX));
    return ValueOperand(typeReg, payloadReg);
}

bool
CodeGeneratorARM::visitValue(LValue *value)
{
    const ValueOperand out = ToOutValue(value);

    masm.moveValue(value->value(), out);
    return true;
}

bool
CodeGeneratorARM::visitOsrValue(LOsrValue *value)
{
    const LAllocation *frame   = value->getOperand(0);
    const ValueOperand out     = ToOutValue(value);

    const ptrdiff_t frameOffset = value->mir()->frameOffset();

    masm.loadValue(Address(ToRegister(frame), frameOffset), out);
    return true;
}

bool
CodeGeneratorARM::visitBox(LBox *box)
{
    const LDefinition *type = box->getDef(TYPE_INDEX);

    JS_ASSERT(!box->getOperand(0)->isConstant());

    // On x86, the input operand and the output payload have the same
    // virtual register. All that needs to be written is the type tag for
    // the type definition.
    masm.ma_mov(Imm32(MIRTypeToTag(box->type())), ToRegister(type));
    return true;
}

bool
CodeGeneratorARM::visitBoxDouble(LBoxDouble *box)
{
    const LDefinition *payload = box->getDef(PAYLOAD_INDEX);
    const LDefinition *type = box->getDef(TYPE_INDEX);
    const LAllocation *in = box->getOperand(0);

    masm.as_vxfer(ToRegister(payload), ToRegister(type),
                  VFPRegister(ToFloatRegister(in)), Assembler::FloatToCore);
    return true;
}

bool
CodeGeneratorARM::visitUnbox(LUnbox *unbox)
{
    // Note that for unbox, the type and payload indexes are switched on the
    // inputs.
    MUnbox *mir = unbox->mir();
    Register type = ToRegister(unbox->type());

    if (mir->fallible()) {
        masm.ma_cmp(type, Imm32(MIRTypeToTag(mir->type())));
        if (!bailoutIf(Assembler::NotEqual, unbox->snapshot()))
            return false;
    }
    return true;
}

void
CodeGeneratorARM::linkAbsoluteLabels()
{
    // arm doesn't have deferred doubles, so this whole thing should be a NOP (right?)
    // deferred doubles are an x86 mechanism for loading doubles into registers by storing
    // them after the function body, then referring to them by their absolute address.
    // On arm, everything should just go in a pool.
# if 0
    JS_NOT_REACHED("Absolute Labels NYI");
    JSScript *script = gen->info().script();
    IonCode *method = script->ion->method();

    for (size_t i = 0; i < deferredDoubles_.length(); i++) {
        DeferredDouble *d = deferredDoubles_[i];
        const Value &v = script->ion->getConstant(d->index());
        MacroAssembler::Bind(method, d->label(), &v);
    }
#endif
}

bool
CodeGeneratorARM::visitDouble(LDouble *ins)
{

    const LDefinition *out = ins->getDef(0);
    const LConstantIndex *cindex = ins->getOperand(0)->toConstantIndex();
    const Value &v = graph.getConstant(cindex->index());

    masm.ma_vimm(v.toDouble(), ToFloatRegister(out));
    return true;
#if 0
    DeferredDouble *d = new DeferredDouble(cindex->index());
    if (!deferredDoubles_.append(d))
        return false;

    masm.movsd(d->label(), ToFloatRegister(out));
    return true;
#endif
}

Register
CodeGeneratorARM::splitTagForTest(const ValueOperand &value)
{
    return value.typeReg();
}

bool
CodeGeneratorARM::visitTestDAndBranch(LTestDAndBranch *test)
{
    const LAllocation *opd = test->input();
    masm.as_vcmpz(VFPRegister(ToFloatRegister(opd)));
    masm.as_vmrs(pc);

    LBlock *ifTrue = test->ifTrue()->lir();
    LBlock *ifFalse = test->ifFalse()->lir();
    // If the compare set the  0 bit, then the result
    // is definately false.
    masm.ma_b(ifFalse->label(), Assembler::Zero);
    // it is also false if one of the operands is NAN, which is
    // shown as Overflow.
    masm.ma_b(ifFalse->label(), Assembler::Overflow);
    if (!isNextBlock(ifTrue))
        masm.ma_b(ifTrue->label());
    return true;
}

bool
CodeGeneratorARM::visitCompareD(LCompareD *comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
    masm.compareDouble(lhs, rhs);
    emitSet(Assembler::ConditionFromDoubleCondition(cond), ToRegister(comp->output()));
    return false;
}

bool
CodeGeneratorARM::visitCompareDAndBranch(LCompareDAndBranch *comp)
{
    FloatRegister lhs = ToFloatRegister(comp->left());
    FloatRegister rhs = ToFloatRegister(comp->right());

    Assembler::DoubleCondition cond = JSOpToDoubleCondition(comp->mir()->jsop());
    masm.compareDouble(lhs, rhs);
    emitBranch(Assembler::ConditionFromDoubleCondition(cond), comp->ifTrue(), comp->ifFalse());
    return true;
}

bool
CodeGeneratorARM::visitCompareB(LCompareB *lir)
{
    MCompare *mir = lir->mir();

    const ValueOperand lhs = ToValue(lir, LCompareB::Lhs);
    const LAllocation *rhs = lir->rhs();
    const Register output = ToRegister(lir->output());

    JS_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    Label notBoolean, done;
    masm.branchTestBoolean(Assembler::NotEqual, lhs, &notBoolean);
    {
        if (rhs->isConstant())
            masm.cmp32(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()));
        else
            masm.cmp32(lhs.payloadReg(), ToRegister(rhs));
        emitSet(JSOpToCondition(mir->jsop()), output);
        masm.jump(&done);
    }
    masm.bind(&notBoolean);
    {
        masm.move32(Imm32(mir->jsop() == JSOP_STRICTNE), output);
    }

    masm.bind(&done);
    return true;
}

bool
CodeGeneratorARM::visitCompareBAndBranch(LCompareBAndBranch *lir)
{
    MCompare *mir = lir->mir();
    const ValueOperand lhs = ToValue(lir, LCompareBAndBranch::Lhs);
    const LAllocation *rhs = lir->rhs();

    JS_ASSERT(mir->jsop() == JSOP_STRICTEQ || mir->jsop() == JSOP_STRICTNE);

    if (mir->jsop() == JSOP_STRICTEQ)
        masm.branchTestBoolean(Assembler::NotEqual, lhs, lir->ifFalse()->lir()->label());
    else
        masm.branchTestBoolean(Assembler::NotEqual, lhs, lir->ifTrue()->lir()->label());

    if (rhs->isConstant())
        masm.cmp32(lhs.payloadReg(), Imm32(rhs->toConstant()->toBoolean()));
    else
        masm.cmp32(lhs.payloadReg(), ToRegister(rhs));
    emitBranch(JSOpToCondition(mir->jsop()), lir->ifTrue(), lir->ifFalse());
    return true;
}

bool
CodeGeneratorARM::visitNotI(LNotI *ins)
{
    // It is hard to optimize !x, so just do it the basic way for now.
    masm.ma_cmp(ToRegister(ins->input()), Imm32(0));
    emitSet(Assembler::Equal, ToRegister(ins->output()));
    return true;
}

bool
CodeGeneratorARM::visitNotD(LNotD *ins)
{
    // Since this operation is not, we want to set a bit if
    // the double is falsey, which means 0.0, -0.0 or NaN.
    // when comparing with 0, an input of 0 will set the Z bit (30)
    // and NaN will set the V bit (28) of the APSR.
    FloatRegister opd = ToFloatRegister(ins->input());
    Register dest = ToRegister(ins->output());

    // Do the compare
    masm.ma_vcmpz(opd);
    bool nocond = true;
    if (nocond) {
        // Load the value into the dest register
        masm.as_vmrs(dest);
        masm.ma_lsr(Imm32(28), dest, dest);
        masm.ma_alu(dest, lsr(dest, 2), dest, op_orr); // 28 + 2 = 30
        masm.ma_and(Imm32(1), dest);
    } else {
        masm.as_vmrs(pc);
        masm.ma_mov(Imm32(0), dest);
        masm.ma_mov(Imm32(1), dest, NoSetCond, Assembler::Equal);
        masm.ma_mov(Imm32(1), dest, NoSetCond, Assembler::Overflow);
#if 0
        masm.as_vmrs(ToRegister(dest));
        // Mask out just the two bits we care about.  If neither bit is set,
        // the dest is already zero
        masm.ma_and(Imm32(0x50000000), dest, dest, Assembler::SetCond);
        // If it is non-zero, then force it to be 1.
        masm.ma_mov(Imm32(1), dest, NoSetCond, Assembler::NotEqual);
#endif
    }
    return true;
}

bool
CodeGeneratorARM::visitLoadSlotV(LLoadSlotV *load)
{
    const ValueOperand out = ToOutValue(load);
    Register base = ToRegister(load->input());
    int32 offset = load->mir()->slot() * sizeof(js::Value);

    masm.loadValue(Address(base, offset), out);
    return true;
}

bool
CodeGeneratorARM::visitLoadSlotT(LLoadSlotT *load)
{
    Register base = ToRegister(load->input());
    int32 offset = load->mir()->slot() * sizeof(js::Value);

    if (load->mir()->type() == MIRType_Double)
        masm.loadInt32OrDouble(Operand(base, offset), ToFloatRegister(load->output()));
    else
        masm.ma_ldr(Operand(base, offset + NUNBOX32_PAYLOAD_OFFSET), ToRegister(load->output()));
    return true;
}

bool
CodeGeneratorARM::visitStoreSlotT(LStoreSlotT *store)
{

    Register base = ToRegister(store->slots());
    int32 offset = store->mir()->slot() * sizeof(js::Value);

    const LAllocation *value = store->value();
    MIRType valueType = store->mir()->value()->type();

    if (store->mir()->needsBarrier())
        emitPreBarrier(Address(base, offset), store->mir()->slotType());

    if (valueType == MIRType_Double) {
        masm.ma_vstr(ToFloatRegister(value), Operand(base, offset));
        return true;
    }

    // Store the type tag if needed.
    if (valueType != store->mir()->slotType())
        masm.storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), Operand(base, offset));

    // Store the payload.
    if (value->isConstant())
        masm.storePayload(*value->toConstant(), Operand(base, offset));
    else
        masm.storePayload(ToRegister(value), Operand(base, offset));

    return true;
}

bool
CodeGeneratorARM::visitLoadElementT(LLoadElementT *load)
{
    Register base = ToRegister(load->elements());
    if (load->mir()->type() == MIRType_Double) {
        if (load->index()->isConstant()) {
            masm.loadInt32OrDouble(Address(base,ToInt32(load->index()) * sizeof(Value)), ToFloatRegister(load->output()));
        } else {
            masm.loadInt32OrDouble(base, ToRegister(load->index()), ToFloatRegister(load->output()));
        }
    } else {
        if (load->index()->isConstant()) {
            masm.load32(Address(base, ToInt32(load->index()) * sizeof(Value)), ToRegister(load->output()));
        } else {
            masm.ma_ldr(DTRAddr(base, DtrRegImmShift(ToRegister(load->index()), LSL, 3)),
                        ToRegister(load->output()));
        }
    }
    JS_ASSERT(!load->mir()->needsHoleCheck());
    return true;
}

void
CodeGeneratorARM::storeElementTyped(const LAllocation *value, MIRType valueType, MIRType elementType,
                                    const Register &elements, const LAllocation *index)
{
    if (index->isConstant()) {
        Address dest = Address(elements, ToInt32(index) * sizeof(Value));
        if (valueType == MIRType_Double) {
            masm.ma_vstr(ToFloatRegister(value), Operand(dest));
            return;
        }

        // Store the type tag if needed.
        if (valueType != elementType)
            masm.storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), dest);

        // Store the payload.
        if (value->isConstant())
            masm.storePayload(*value->toConstant(), dest);
        else
            masm.storePayload(ToRegister(value), dest);
    } else {
        Register indexReg = ToRegister(index);
        if (valueType == MIRType_Double) {
            masm.ma_vstr(ToFloatRegister(value), elements, indexReg);
            return;
        }

        // Store the type tag if needed.
        if (valueType != elementType)
            masm.storeTypeTag(ImmType(ValueTypeFromMIRType(valueType)), elements, indexReg);

        // Store the payload.
        if (value->isConstant())
            masm.storePayload(*value->toConstant(), elements, indexReg);
        else
            masm.storePayload(ToRegister(value), elements, indexReg);
    }
}

bool
CodeGeneratorARM::visitGuardShape(LGuardShape *guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());
    masm.ma_ldr(DTRAddr(obj, DtrOffImm(JSObject::offsetOfShape())), tmp);
    masm.ma_cmp(tmp, ImmGCPtr(guard->mir()->shape()));

    if (!bailoutIf(Assembler::NotEqual, guard->snapshot()))
        return false;
    return true;
}

bool
CodeGeneratorARM::visitGuardClass(LGuardClass *guard)
{
    Register obj = ToRegister(guard->input());
    Register tmp = ToRegister(guard->tempInt());

    masm.loadObjClass(obj, tmp);
    masm.ma_cmp(tmp, Imm32((uint32)guard->mir()->getClass()));
    if (!bailoutIf(Assembler::NotEqual, guard->snapshot()))
        return false;
    return true;
}

bool
CodeGeneratorARM::visitImplicitThis(LImplicitThis *lir)
{
    Register callee = ToRegister(lir->callee());
    const ValueOperand out = ToOutValue(lir);

    // The implicit |this| is always |undefined| if the function's environment
    // is the current global.
    masm.ma_ldr(DTRAddr(callee, DtrOffImm(JSFunction::offsetOfEnvironment())), out.typeReg());
    masm.ma_cmp(out.typeReg(), ImmGCPtr(&gen->info().script()->global()));

    // TODO: OOL stub path.
    if (!bailoutIf(Assembler::NotEqual, lir->snapshot()))
        return false;

    masm.moveValue(UndefinedValue(), out);
    return true;
}

bool
CodeGeneratorARM::visitRecompileCheck(LRecompileCheck *lir)
{
    Register tmp = ToRegister(lir->tempInt());
    size_t *addr = gen->info().script()->addressOfUseCount();

    // Bump the script's use count. Note that it's safe to bake in this pointer
    // since scripts are never nursery allocated and jitcode will be purged before
    // doing a compacting GC.
    masm.load32(AbsoluteAddress(addr), tmp);
    masm.ma_add(Imm32(1), tmp);
    masm.store32(tmp, AbsoluteAddress(addr));

    // Bailout if the script is hot.
    masm.ma_cmp(tmp, Imm32(lir->mir()->minUses()));
    if (!bailoutIf(Assembler::AboveOrEqual, lir->snapshot()))
        return false;
    return true;
}

bool
CodeGeneratorARM::visitInterruptCheck(LInterruptCheck *lir)
{
    typedef bool (*pf)(JSContext *);
    static const VMFunction interruptCheckInfo = FunctionInfo<pf>(InterruptCheck);

    OutOfLineCode *ool = oolCallVM(interruptCheckInfo, lir, (ArgList()), StoreNothing());
    if (!ool)
        return false;

    void *interrupt = (void*)&gen->compartment->rt->interrupt;
    masm.load32(AbsoluteAddress(interrupt), lr);
    masm.ma_cmp(lr, Imm32(0));
    masm.ma_b(ool->entry(), Assembler::NonZero);
    masm.bind(ool->rejoin());
    return true;
}

bool
CodeGeneratorARM::generateInvalidateEpilogue()
{
    // Ensure that there is enough space in the buffer for the OsiPoint
    // patching to occur. Otherwise, we could overwrite the invalidation
    // epilogue.
    for (size_t i = 0; i < sizeof(void *); i+= Assembler::nopSize())
        masm.nop();

    masm.bind(&invalidate_);

    // Push the return address of the point that we bailed out at onto the stack
    masm.Push(lr);

    JSContext *cx = GetIonContext()->cx;

    // Push the Ion script onto the stack (when we determine what that pointer is).
    invalidateEpilogueData_ = masm.pushWithPatch(ImmWord(uintptr_t(-1)));
    IonCode *thunk = cx->compartment->ionCompartment()->getOrCreateInvalidationThunk(cx);
    if (!thunk)
        return false;

    masm.branch(thunk);

    // We should never reach this point in JIT code -- the invalidation thunk should
    // pop the invalidated JS frame and return directly to its caller.
    masm.breakpoint();
    return true;
}
