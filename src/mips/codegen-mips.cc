// Copyright 2010 the V8 project authors. All rights reserved.
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are
// met:
//
//     * Redistributions of source code must retain the above copyright
//       notice, this list of conditions and the following disclaimer.
//     * Redistributions in binary form must reproduce the above
//       copyright notice, this list of conditions and the following
//       disclaimer in the documentation and/or other materials provided
//       with the distribution.
//     * Neither the name of Google Inc. nor the names of its
//       contributors may be used to endorse or promote products derived
//       from this software without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.


#include "v8.h"

#if defined(V8_TARGET_ARCH_MIPS)

#include "bootstrapper.h"
#include "code-stubs.h"
#include "codegen-inl.h"
#include "compiler.h"
#include "debug.h"
#include "ic-inl.h"
#include "jsregexp.h"
#include "jump-target-light-inl.h"
#include "parser.h"
#include "regexp-macro-assembler.h"
#include "regexp-stack.h"
#include "register-allocator-inl.h"
#include "runtime.h"
#include "scopes.h"
#include "virtual-frame-inl.h"
#include "virtual-frame-mips-inl.h"

namespace v8 {
namespace internal {


#define __ ACCESS_MASM(masm_)

// -------------------------------------------------------------------------
// Platform-specific DeferredCode functions.

void DeferredCode::SaveRegisters() {
  // On MIPS you either have a completely spilled frame or you
  // handle it yourself, but at the moment there's no automation
  // of registers and deferred code.
}


void DeferredCode::RestoreRegisters() {
}


// -------------------------------------------------------------------------
// Platform-specific RuntimeCallHelper functions.

void VirtualFrameRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  frame_state_->frame()->AssertIsSpilled();
}


void VirtualFrameRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
}


void ICRuntimeCallHelper::BeforeCall(MacroAssembler* masm) const {
  masm->EnterInternalFrame();
}


void ICRuntimeCallHelper::AfterCall(MacroAssembler* masm) const {
  masm->LeaveInternalFrame();
}


// -----------------------------------------------------------------------------
// CodeGenState implementation.

CodeGenState::CodeGenState(CodeGenerator* owner)
    : owner_(owner),
      previous_(owner->state()) {
  owner->set_state(this);
}


ConditionCodeGenState::ConditionCodeGenState(CodeGenerator* owner,
                           JumpTarget* true_target,
                           JumpTarget* false_target)
    : CodeGenState(owner),
      true_target_(true_target),
      false_target_(false_target) {
  owner->set_state(this);
}


TypeInfoCodeGenState::TypeInfoCodeGenState(CodeGenerator* owner,
                                           Slot* slot,
                                           TypeInfo type_info)
    : CodeGenState(owner),
      slot_(slot) {
  owner->set_state(this);
  old_type_info_ = owner->set_type_info(slot, type_info);
}


CodeGenState::~CodeGenState() {
  ASSERT(owner_->state() == this);
  owner_->set_state(previous_);
}


TypeInfoCodeGenState::~TypeInfoCodeGenState() {
  owner()->set_type_info(slot_, old_type_info_);
}

// -----------------------------------------------------------------------------
// CodeGenerator implementation.

int CodeGenerator::inlined_write_barrier_size_ = -1;

CodeGenerator::CodeGenerator(MacroAssembler* masm)
    : deferred_(8),
      masm_(masm),
      info_(NULL),
      frame_(NULL),
      allocator_(NULL),
      cc_reg_(cc_always),
      state_(NULL),
      loop_nesting_(0),
      type_info_(NULL),
      function_return_(JumpTarget::BIDIRECTIONAL),
      function_return_is_shadowed_(false) {
}


// Calling conventions:
// fp: caller's frame pointer
// sp: stack pointer
// a1: called JS function
// cp: callee's context

void CodeGenerator::Generate(CompilationInfo* info) {
  // Record the position for debugging purposes.
  CodeForFunctionPosition(info->function());

  // Initialize state.
  info_ = info;
  int slots = scope()->num_parameters() + scope()->num_stack_slots();
  ScopedVector<TypeInfo> type_info_array(slots);
  type_info_ = &type_info_array;
  ASSERT(allocator_ == NULL);
  RegisterAllocator register_allocator(this);
  allocator_ = &register_allocator;
  ASSERT(frame_ == NULL);
  frame_ = new VirtualFrame();
  cc_reg_ = cc_always;

  // Adjust for function-level loop nesting.
  ASSERT_EQ(0, loop_nesting_);
  loop_nesting_ = info->is_in_loop() ? 1 : 0;

  {
    CodeGenState state(this);

    // Registers:
    // a1: called JS function
    // ra: return address
    // fp: caller's frame pointer
    // sp: stack pointer
    // cp: callee's context
    //
    // Stack:
    // arguments
    // receiver

#ifdef DEBUG
    if (strlen(FLAG_stop_at) > 0 &&
        info->function()->name()->IsEqualTo(CStrVector(FLAG_stop_at))) {
      frame_->SpillAll();
      __ stop("stop-at");
    }
#endif

    frame_->Enter();

    // Allocate space for locals and initialize them.
    frame_->AllocateStackSlots();

    frame_->AssertIsSpilled();
    int heap_slots = scope()->num_heap_slots() - Context::MIN_CONTEXT_SLOTS;
    if (heap_slots > 0) {
      // Allocate local context.
      // Get outer context and create a new context based on it.
      __ lw(a0, frame_->Function());
      frame_->EmitPush(a0);
      if (heap_slots <= FastNewContextStub::kMaximumSlots) {
        FastNewContextStub stub(heap_slots);
        frame_->CallStub(&stub, 1);
      } else {
        frame_->CallRuntime(Runtime::kNewContext, 1);  // v0 holds the result
      }

#ifdef DEBUG
      JumpTarget verified_true;
      verified_true.Branch(eq, v0, Operand(cp), no_hint);
      __ stop("NewContext: v0 is expected to be the same as cp");
      verified_true.Bind();
#endif
      // Update context local.
      __ sw(cp, frame_->Context());
    }

    {
      Comment cmnt2(masm_, "[ copy context parameters into .context");

      // Note that iteration order is relevant here! If we have the same
      // parameter twice (e.g., function (x, y, x)), and that parameter
      // needs to be copied into the context, it must be the last argument
      // passed to the parameter that needs to be copied. This is a rare
      // case so we don't check for it, instead we rely on the copying
      // order: such a parameter is copied repeatedly into the same
      // context location and thus the last value is what is seen inside
      // the function.
      frame_->AssertIsSpilled();
      for (int i = 0; i < scope()->num_parameters(); i++) {
        Variable* par = scope()->parameter(i);
        Slot* slot = par->AsSlot();
        if (slot != NULL && slot->type() == Slot::CONTEXT) {
          ASSERT(!scope()->is_global_scope());  // no parameters in global scope
          __ lw(a1, frame_->ParameterAt(i));
          // Loads a2 with context; used below in RecordWrite.
          __ sw(a1, SlotOperand(slot, a2));
          // Load the offset into a3.
          int slot_offset =
              FixedArray::kHeaderSize + slot->index() * kPointerSize;
          __ RecordWrite(a2, Operand(slot_offset), a3, a1);
        }
      }
    }

    // Store the arguments object.  This must happen after context
    // initialization because the arguments object may be stored in
    // the context.
    if (ArgumentsMode() != NO_ARGUMENTS_ALLOCATION) {
      StoreArgumentsObject(true);
    }

    // Initialize ThisFunction reference if present.
    if (scope()->is_function_scope() && scope()->function() != NULL) {
      frame_->EmitPushRoot(Heap::kTheHoleValueRootIndex);
      StoreToSlot(scope()->function()->AsSlot(), NOT_CONST_INIT);
    }

    // Initialize the function return target after the locals are set
    // up, because it needs the expected frame height from the frame.
    function_return_.SetExpectedHeight();
    function_return_is_shadowed_ = false;

    // Generate code to 'execute' declarations and initialize functions
    // (source elements). In case of an illegal redeclaration we need to
    // handle that instead of processing the declarations.
    if (scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ illegal redeclarations");
      scope()->VisitIllegalRedeclaration(this);
    } else {
      Comment cmnt(masm_, "[ declarations");
      ProcessDeclarations(scope()->declarations());
      // Bail out if a stack-overflow exception occurred when processing
      // declarations.
      if (HasStackOverflow()) return;
    }

    if (FLAG_trace) {
      frame_->CallRuntime(Runtime::kTraceEnter, 0);
      // Ignore the return value.
    }

    // Compile the body of the function in a vanilla state. Don't
    // bother compiling all the code if the scope has an illegal
    // redeclaration.
    if (!scope()->HasIllegalRedeclaration()) {
      Comment cmnt(masm_, "[ function body");
#ifdef DEBUG
      bool is_builtin = Bootstrapper::IsActive();
      bool should_trace =
          is_builtin ? FLAG_trace_builtin_calls : FLAG_trace_calls;
      if (should_trace) {
        frame_->CallRuntime(Runtime::kDebugTrace, 0);
        // Ignore the return value.
      }
#endif
      VisitStatements(info->function()->body());
    }
  }

  // Handle the return from the function.
  if (has_valid_frame()) {
    // If there is a valid frame, control flow can fall off the end of
    // the body.  In that case there is an implicit return statement.
    ASSERT(!function_return_is_shadowed_);
    frame_->PrepareForReturn();
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
    if (function_return_.is_bound()) {
      function_return_.Jump();
    } else {
      function_return_.Bind();
      GenerateReturnSequence();
    }
  } else if (function_return_.is_linked()) {
    // If the return target has dangling jumps to it, then we have not
    // yet generated the return sequence.  This can happen when (a)
    // control does not flow off the end of the body so we did not
    // compile an artificial return statement just above, and (b) there
    // are return statements in the body but (c) they are all shadowed.
    function_return_.Bind();
    GenerateReturnSequence();
  }

  // Adjust for function-level loop nesting.
  ASSERT(loop_nesting_ == info->is_in_loop() ? 1 : 0);
  loop_nesting_ = 0;

  // Code generation state must be reset.
  ASSERT(!has_cc());
  ASSERT(state_ == NULL);
  ASSERT(!function_return_is_shadowed_);
  function_return_.Unuse();
  DeleteFrame();

  // Process any deferred code using the register allocator.
  if (!HasStackOverflow()) {
    ProcessDeferred();
  }

  allocator_ = NULL;
  type_info_ = NULL;
}


int CodeGenerator::NumberOfSlot(Slot* slot) {
  if (slot == NULL) return kInvalidSlotNumber;
  switch (slot->type()) {
    case Slot::PARAMETER:
      return slot->index();
    case Slot::LOCAL:
      return slot->index() + scope()->num_parameters();
    default:
      break;
  }
  return kInvalidSlotNumber;
}


MemOperand CodeGenerator::SlotOperand(Slot* slot, Register tmp) {
  // Currently, this assertion will fail if we try to assign to
  // a constant variable that is constant because it is read-only
  // (such as the variable referring to a named function expression).
  // We need to implement assignments to read-only variables.
  // Ideally, we should do this during AST generation (by converting
  // such assignments into expression statements); however, in general
  // we may not be able to make the decision until past AST generation,
  // that is when the entire program is known.
  ASSERT(slot != NULL);
  int index = slot->index();
  switch (slot->type()) {
    case Slot::PARAMETER:
      return frame_->ParameterAt(index);

    case Slot::LOCAL:
      return frame_->LocalAt(index);

    case Slot::CONTEXT: {
      ASSERT(!tmp.is(cp));  // Do not overwrite context register.
      Register context = cp;
      int chain_length = scope()->ContextChainLength(slot->var()->scope());
      for (int i = 0; i < chain_length; i++) {
        // Load the closure.
        // (All contexts, even 'with' contexts, have a closure,
        // and it is the same for all contexts inside a function.
        // There is no need to go to the function context first.)
        __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
        // Load the function context (which is the incoming, outer context).
        __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
        context = tmp;
      }
      // We may have a 'with' context now. Get the function context.
      // (In fact this mov may never be the needed, since the scope analysis
      // may not permit a direct context access in this case and thus we are
      // always at a function context. However it is safe to dereference be-
      // cause the function context of a function context is itself. Before
      // deleting this mov we should try to create a counter-example first,
      // though...)
      __ lw(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
      return ContextOperand(tmp, index);
    }

    default:
      UNREACHABLE();
      return MemOperand(no_reg, 0);
  }
}


MemOperand CodeGenerator::ContextSlotOperandCheckExtensions(
    Slot* slot,
    Register tmp,
    Register tmp2,
    JumpTarget* slow) {
  ASSERT(slot->type() == Slot::CONTEXT);
  Register context = cp;

  for (Scope* s = scope(); s != slot->var()->scope(); s = s->outer_scope()) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        // Check that extension is NULL.
        __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        slow->Branch(ne, tmp2, Operand(zero_reg));
      }
      __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
  }
  // Check that last extension is NULL.
  __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
  slow->Branch(ne, tmp2, Operand(zero_reg));
  __ lw(tmp, ContextOperand(context, Context::FCONTEXT_INDEX));
  return ContextOperand(tmp, slot->index());
}



// Loads a value on TOS. If it is a boolean value, the result may have been
// (partially) translated into branches, or it may have set the condition
// code register. If force_cc is set, the value is forced to set the
// condition code register and no value is pushed. If the condition code
// register was set, has_cc() is true and cc_reg_ contains the condition to
// test for 'true'.
void CodeGenerator::LoadCondition(Expression* x,
                                  JumpTarget* true_target,
                                  JumpTarget* false_target,
                                  bool force_cc) {
  ASSERT(!has_cc());
  int original_height = frame_->height();

  { ConditionCodeGenState new_state(this, true_target, false_target);
    Visit(x);

    // If we hit a stack overflow, we may not have actually visited
    // the expression. In that case, we ensure that we have a
    // valid-looking frame state because we will continue to generate
    // code as we unwind the C++ stack.
    //
    // It's possible to have both a stack overflow and a valid frame
    // state (eg, a subexpression overflowed, visiting it returned
    // with a dummied frame state, and visiting this expression
    // returned with a normal-looking state).
    if (HasStackOverflow() &&
        has_valid_frame() &&
        !has_cc() &&
        frame_->height() == original_height) {
      true_target->Jump();
    }
  }
  if (force_cc && frame_ != NULL && !has_cc()) {
    // Convert the TOS value to a boolean in the condition code register.
    ToBoolean(true_target, false_target);
  }
  ASSERT(!force_cc || !has_valid_frame() || has_cc());
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::Load(Expression* x) {
  // We generally assume that we are not in a spilled scope for most
  // of the code generator.  A failure to ensure this caused issue 815
  // and this assert is designed to catch similar issues.
  frame_->AssertIsNotSpilled();
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  JumpTarget true_target;
  JumpTarget false_target;
  LoadCondition(x, &true_target, &false_target, false);

  if (has_cc()) {
    // Convert cc_reg_ into a boolean value.
    JumpTarget loaded;
    JumpTarget materialize_true;

    materialize_true.Branch(cc_reg_, condReg1, Operand(condReg2));
    frame_->EmitPushRoot(Heap::kFalseValueRootIndex);
    loaded.Jump();
    materialize_true.Bind();
    frame_->EmitPushRoot(Heap::kTrueValueRootIndex);
    loaded.Bind();
    cc_reg_ = cc_always;
  }

  if (true_target.is_linked() || false_target.is_linked()) {
    // We have at least one condition value that has been "translated"
    // into a branch, thus it needs to be loaded explicitly.
    JumpTarget loaded;
    if (frame_ != NULL) {
      loaded.Jump();  // Don't lose the current TOS.
    }
    bool both = true_target.is_linked() && false_target.is_linked();
    // Load "true" if necessary.
    if (true_target.is_linked()) {
      true_target.Bind();
      frame_->EmitPushRoot(Heap::kTrueValueRootIndex);
    }
    // If both "true" and "false" need to be loaded jump across the code for
    // "false".
    if (both) {
      loaded.Jump();
    }
    // Load "false" if necessary.
    if (false_target.is_linked()) {
      false_target.Bind();
      frame_->EmitPushRoot(Heap::kFalseValueRootIndex);
    }
    // A value is loaded on all paths reaching this point.
    loaded.Bind();
  }
  ASSERT(has_valid_frame());
  ASSERT(!has_cc());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::LoadGlobal() {
  Register reg = frame_->GetTOSRegister();
  __ lw(reg, GlobalObject());
  frame_->EmitPush(reg);
}


void CodeGenerator::LoadGlobalReceiver(Register scratch) {
  Register reg = frame_->GetTOSRegister();
  __ lw(reg, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ lw(reg,
        FieldMemOperand(reg, GlobalObject::kGlobalReceiverOffset));
  frame_->EmitPush(reg);
}


ArgumentsAllocationMode CodeGenerator::ArgumentsMode() {
  if (scope()->arguments() == NULL) return NO_ARGUMENTS_ALLOCATION;
  ASSERT(scope()->arguments_shadow() != NULL);
  // We don't want to do lazy arguments allocation for functions that
  // have heap-allocated contexts, because it interfers with the
  // uninitialized const tracking in the context objects.
  return (scope()->num_heap_slots() > 0)
      ? EAGER_ARGUMENTS_ALLOCATION
      : LAZY_ARGUMENTS_ALLOCATION;
}


void CodeGenerator::StoreArgumentsObject(bool initial) {
  ArgumentsAllocationMode mode = ArgumentsMode();
  ASSERT(mode != NO_ARGUMENTS_ALLOCATION);

  Comment cmnt(masm_, "[ store arguments object");
  if (mode == LAZY_ARGUMENTS_ALLOCATION && initial) {
    // When using lazy arguments allocation, we store the hole value
    // as a sentinel indicating that the arguments object hasn't been
    // allocated yet.
    frame_->EmitPushRoot(Heap::kTheHoleValueRootIndex);
  } else {
    frame_->SpillAll();
    ArgumentsAccessStub stub(ArgumentsAccessStub::NEW_OBJECT);
    __ lw(a2, frame_->Function());
    // The receiver is below the arguments, the return address, and the
    // frame pointer on the stack.
    const int kReceiverDisplacement = 2 + scope()->num_parameters();
    __ Addu(a1, fp, Operand(kReceiverDisplacement * kPointerSize));
    __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));
    frame_->Adjust(3);
    __ Push(a2, a1, a0);
    frame_->CallStub(&stub, 3);
    frame_->EmitPush(v0);
  }

  Variable* arguments = scope()->arguments();
  Variable* shadow = scope()->arguments_shadow();
  ASSERT(arguments != NULL && arguments->AsSlot() != NULL);
  ASSERT(shadow != NULL && shadow->AsSlot() != NULL);
  JumpTarget done;
  if (mode == LAZY_ARGUMENTS_ALLOCATION && !initial) {
    // We have to skip storing into the arguments slot if it has
    // already been written to. This can happen if the a function
    // has a local variable named 'arguments'.
    LoadFromSlot(scope()->arguments()->AsSlot(), NOT_INSIDE_TYPEOF);
    Register arguments = frame_->PopToRegister();
    __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
    done.Branch(ne, arguments, Operand(at));
  }
  StoreToSlot(arguments->AsSlot(), NOT_CONST_INIT);
  if (mode == LAZY_ARGUMENTS_ALLOCATION) done.Bind();
  StoreToSlot(shadow->AsSlot(), NOT_CONST_INIT);
}


void CodeGenerator::LoadTypeofExpression(Expression* x) {
  // Special handling of identifiers as subxessions of typeof.
  Variable* variable = x->AsVariableProxy()->AsVariable();
  if (variable != NULL && !variable->is_this() && variable->is_global()) {
    // For a global variable we build the property reference
    // <global>.<variable> and perform a (regular non-contextual) property
    // load to make sure we do not get reference errors.
    Slot global(variable, Slot::CONTEXT, Context::GLOBAL_INDEX);
    Literal key(variable->name());
    Property property(&global, &key, RelocInfo::kNoPosition);
    Reference ref(this, &property);
    ref.GetValue();
  } else if (variable != NULL && variable->AsSlot() != NULL) {
    // For a variable that rewrites to a slot, we signal it is the immediate
    // subxession of a typeof.
    LoadFromSlotCheckForArguments(variable->AsSlot(), INSIDE_TYPEOF);
  } else {
    // Anything else can be handled normally.
    Load(x);
  }
}


Reference::Reference(CodeGenerator* cgen,
                     Expression* expression,
                     bool persist_after_get)
    : cgen_(cgen),
      expression_(expression),
      type_(ILLEGAL),
      persist_after_get_(persist_after_get) {
  // We generally assume that we are not in a spilled scope for most
  // of the code generator.  A failure to ensure this caused issue 815
  // and this assert is designed to catch similar issues.
  cgen->frame()->AssertIsNotSpilled();
  cgen->LoadReference(this);
}


Reference::~Reference() {
  ASSERT(is_unloaded() || is_illegal());
}


void CodeGenerator::LoadReference(Reference* ref) {
  Comment cmnt(masm_, "[ LoadReference");
  Expression* e = ref->expression();
  Property* property = e->AsProperty();
  Variable* var = e->AsVariableProxy()->AsVariable();

  if (property != NULL) {
    // The expression is either a property or a variable proxy that rewrites
    // to a property.
    Load(property->obj());
    if (property->key()->IsPropertyName()) {
      ref->set_type(Reference::NAMED);
    } else {
      Load(property->key());
      ref->set_type(Reference::KEYED);
    }
  } else if (var != NULL) {
    // The expression is a variable proxy that does not rewrite to a
    // property.  Global variables are treated as named property references.
    if (var->is_global()) {
      LoadGlobal();
      ref->set_type(Reference::NAMED);
    } else {
      ASSERT(var->AsSlot() != NULL);
      ref->set_type(Reference::SLOT);
    }
  } else {
    // Anything else is a runtime error.
    Load(e);
    frame_->CallRuntime(Runtime::kThrowReferenceError, 1);
  }
}


void CodeGenerator::UnloadReference(Reference* ref) {
  int size = ref->size();
  ref->set_unloaded();
  if (size == 0) return;

  // Pop a reference from the stack while preserving TOS.
  VirtualFrame::RegisterAllocationScope scope(this);
  Comment cmnt(masm_, "[ UnloadReference");
  if (size > 0) {
    Register tos = frame_->PopToRegister();
    frame_->Drop(size);
    frame_->EmitPush(tos);
  }
}


// ECMA-262, section 9.2, page 30: ToBoolean(). Convert the given
// register to a boolean in the condition code register. The code
// may jump to 'false_target' in case the register converts to 'false'.
void CodeGenerator::ToBoolean(JumpTarget* true_target,
                              JumpTarget* false_target) {
  // Note: The generated code snippet does not change stack variables.
  //       Only the condition code should be set.
  bool known_smi = frame_->KnownSmiAt(0);
  Register tos = frame_->PopToRegister();

  // Fast case checks

  if (!known_smi) {
    // Check if the value is 'false'.
    __ LoadRoot(at, Heap::kFalseValueRootIndex);
    false_target->Branch(eq, tos, Operand(at));

    // Check if the value is 'true'.
    __ LoadRoot(at, Heap::kTrueValueRootIndex);
    true_target->Branch(eq, tos, Operand(at));

    // Check if the value is 'undefined'.
    __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
    false_target->Branch(eq, tos, Operand(at));
  }

  // Check if the value is a smi.
  __ mov(condReg1, tos);
  ASSERT(Smi::FromInt(0) == 0);
  __ mov(condReg2, zero_reg);

  if (!known_smi) {
    false_target->Branch(eq, tos, Operand(Smi::FromInt(0)));
    __ And(at, tos, Operand(kSmiTagMask));
    true_target->Branch(eq, at, Operand(zero_reg));

    if (CpuFeatures::IsSupported(FPU)) {
      CpuFeatures::Scope scope(FPU);
      // Implements the slow case by using ToBooleanStub.
      // The ToBooleanStub takes a single argument, and
      // returns a non-zero value for true, or zero for false.
      // Both the argument value and the return value use the
      // register assigned to tos_
      ToBooleanStub stub(tos);
      frame_->CallStub(&stub, 0);
      // Convert the result in "tos" to a condition code.
      __ mov(condReg1, zero_reg);
      __ mov(condReg2, tos);

    } else {
      // Slow case: call the runtime.
      frame_->EmitPush(tos);
      frame_->CallRuntime(Runtime::kToBool, 1);
      // Convert the result (v0) to a condition code.
      __ LoadRoot(condReg1, Heap::kFalseValueRootIndex);
      __ mov(condReg2, v0);
    }
  }

  cc_reg_ = ne;
}


void CodeGenerator::GenericBinaryOperation(Token::Value op,
                                           OverwriteMode overwrite_mode,
                                           GenerateInlineSmi inline_smi,
                                           int constant_rhs) {
  // sp[0] : y
  // sp[1] : x
  // result : v0

  // Stub is entered with a call: 'return address' is in lr.
  switch (op) {
    case Token::ADD:  // Fall through.
    case Token::SUB:  // Fall through.
      if (inline_smi) {
        JumpTarget done;
        JumpTarget not_smi;
        Register rhs = frame_->PopToRegister();
        Register lhs = frame_->PopToRegister(rhs);
        Register scratch = VirtualFrame::scratch0();
        __ Or(scratch, rhs, Operand(lhs));
        // Check they are both small and positive.
        __ And(scratch, scratch, Operand(kSmiTagMask | 0xc0000000));
        not_smi.Branch(ne, scratch, Operand(zero_reg));
        ASSERT(rhs.is(a0) || lhs.is(a0));  // r0 is free now.
        STATIC_ASSERT(kSmiTag == 0);
        if (op == Token::ADD) {
          __ Addu(v0, lhs, Operand(rhs));
        } else {
          __ Subu(v0, lhs, Operand(rhs));
        }
        done.Branch(eq, scratch, Operand(zero_reg));
        not_smi.Bind();
        GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
        frame_->SpillAll();
        frame_->CallStub(&stub, 0);
        done.Bind();
        frame_->EmitPush(v0);
        break;
      } else {
        // Fall through!
      }

    case Token::BIT_OR:
    case Token::BIT_AND:
    case Token::BIT_XOR:
      if (inline_smi) {
        JumpTarget not_smi;
        bool rhs_is_smi = frame_->KnownSmiAt(0);
        bool lhs_is_smi = frame_->KnownSmiAt(1);
        Register rhs = frame_->PopToRegister();
        Register lhs = frame_->PopToRegister(rhs);
        Register smi_test_reg;
        Register scratch = VirtualFrame::scratch0();
        Condition cond;
        if (!rhs_is_smi || !lhs_is_smi) {
          if (rhs_is_smi) {
            smi_test_reg = lhs;
          } else if (lhs_is_smi) {
            smi_test_reg = rhs;
          } else {
            smi_test_reg = VirtualFrame::scratch0();
            __ Or(smi_test_reg, rhs, Operand(lhs));
          }
          // Check they are both Smis.
          __ And(scratch, smi_test_reg, Operand(kSmiTagMask));
          cond = eq;
          not_smi.Branch(ne, scratch, Operand(zero_reg));
        } else {
          cond = al;
        }
        ASSERT(rhs.is(a0) || lhs.is(a0));  // r0 is free now.
        if (op == Token::BIT_OR) {
          __ or_(v0, lhs, rhs);
        } else if (op == Token::BIT_AND) {
          __ and_(v0, lhs, rhs);
        } else {
          ASSERT(op == Token::BIT_XOR);
          STATIC_ASSERT(kSmiTag == 0);
          __ xor_(v0, lhs, rhs);
        }
        not_smi.Bind();
        if (cond != al) {
          JumpTarget done;
          done.Branch(cond, scratch, Operand(zero_reg));
          GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
          frame_->SpillAll();
          frame_->CallStub(&stub, 0);
          done.Bind();
        }
        frame_->EmitPush(v0);
        break;
      } else {
        // Fall through!
      }
    case Token::MUL:
    case Token::DIV:
    case Token::MOD:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      Register rhs = frame_->PopToRegister();
      Register lhs = frame_->PopToRegister(rhs);  // Don't pop to rhs register.
      GenericBinaryOpStub stub(op, overwrite_mode, lhs, rhs, constant_rhs);
      frame_->SpillAll();
      frame_->CallStub(&stub, 0);
      frame_->EmitPush(v0);
      break;
    }

    case Token::COMMA: {
      Register scratch = frame_->PopToRegister();
      // Simply discard left value.
      frame_->Drop();
      frame_->EmitPush(scratch);
      break;
    }

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }
}


class DeferredInlineSmiOperation: public DeferredCode {
 public:
  DeferredInlineSmiOperation(Token::Value op,
                             int value,
                             bool reversed,
                             OverwriteMode overwrite_mode,
                             Register tos)
      : op_(op),
        value_(value),
        reversed_(reversed),
        overwrite_mode_(overwrite_mode),
        tos_register_(tos) {
    set_comment("[ DeferredInlinedSmiOperation");
  }

  virtual void Generate();
  // This stub makes explicit calls to SaveRegisters(), RestoreRegisters() and
  // Exit(). Currently on MIPS SaveRegisters() and RestoreRegisters() are empty
  // methods, it is the responsibility of the deferred code to save and restore
  // registers.
  virtual bool AutoSaveAndRestore() { return false; }

  void JumpToNonSmiInput(Condition cond, Register cmp1, const Operand& cmp2);
  void JumpToAnswerOutOfRange(Condition cond,
                              Register cmp1,
                              const Operand& cmp2);

 private:
  void GenerateNonSmiInput();
  void GenerateAnswerOutOfRange();
  void WriteNonSmiAnswer(Register answer,
                         Register heap_number,
                         Register scratch);

  Token::Value op_;
  int value_;
  bool reversed_;
  OverwriteMode overwrite_mode_;
  Register tos_register_;
  Label non_smi_input_;
  Label answer_out_of_range_;
};


// For bit operations we try harder and handle the case where the input is not
// a Smi but a 32bits integer without calling the generic stub.
void DeferredInlineSmiOperation::JumpToNonSmiInput(Condition cond,
                                                   Register cmp1,
                                                   const Operand& cmp2) {
  ASSERT(Token::IsBitOp(op_));

  __ Branch(&non_smi_input_, cond, cmp1, cmp2);
}


// For bit operations the result is always 32bits so we handle the case where
// the result does not fit in a Smi without calling the generic stub.
void DeferredInlineSmiOperation::JumpToAnswerOutOfRange(Condition cond,
                                                        Register cmp1,
                                                        const Operand& cmp2) {
  ASSERT(Token::IsBitOp(op_));

  if ((op_ == Token::SHR) && !CpuFeatures::IsSupported(FPU)) {
    // >>> requires an unsigned to double conversion and the non FPU code
    // does not support this conversion.
    __ Branch(entry_label(), cond, cmp1, cmp2);
  } else {
    __ Branch(&answer_out_of_range_, cond, cmp1, cmp2);
  }
}

// On entry the non-constant side of the binary operation is in tos_register_
// and the constant smi side is nowhere.  The tos_register_ is not used by the
// virtual frame.  On exit the answer is in the tos_register_ and the virtual
// frame is unchanged.
void DeferredInlineSmiOperation::Generate() {
  VirtualFrame copied_frame(*frame_state()->frame());
  copied_frame.SpillAll();

  // In CodeGenerator::SmiOperation we used a1 instead of a0, and we left the
  // register untouched.
  // We just need to load value_ and switch if necessary.
  Register lhs = a1;
  Register rhs = a0;

  switch (op_) {
    case Token::ADD:
    case Token::SUB:  {
      if (reversed_) {
        __ mov(a0, tos_register_);
        __ li(a1, Operand(Smi::FromInt(value_)));
      } else {
        __ mov(a1, tos_register_);
        __ li(a0, Operand(Smi::FromInt(value_)));
      }
      break;
    }
    case Token::MUL:
    case Token::MOD:
    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND:
    case Token::SHL:
    case Token::SHR:
    case Token::SAR: {
      if (tos_register_.is(a1)) {
        __ li(a0, Operand(Smi::FromInt(value_)));
      } else {
        // This used to look a little different from the arm version.
        // Now it's a copy of that.
        ASSERT(tos_register_.is(a0));
        __ li(a1, Operand(Smi::FromInt(value_)));
      }
      if (reversed_ == tos_register_.is(a1)) {
        lhs = a0;
        rhs = a1;
      }
      break;
    }

    default:
      // Other cases should have been handled before this point.
      UNREACHABLE();
      break;
  }

  GenericBinaryOpStub stub(op_, overwrite_mode_, lhs, rhs, value_);
  __ CallStub(&stub);

  // The generic stub returns its value in v0, but that's not
  // necessarily what we want.  We want whatever the inlined code
  // expected, which is that the answer is in the same register as
  // the operand was.
  __ Move(tos_register_, v0);

  // The tos register was not in use for the virtual frame that we
  // came into this function with, so we can merge back to that frame
  // without trashing it.
  copied_frame.MergeTo(frame_state()->frame());

  Exit();

  if (non_smi_input_.is_linked()) {
    GenerateNonSmiInput();
  }

  if (answer_out_of_range_.is_linked()) {
    GenerateAnswerOutOfRange();
  }
}


// Convert and write the integer answer into heap_number.
void DeferredInlineSmiOperation::WriteNonSmiAnswer(Register answer,
                                                   Register heap_number,
                                                   Register scratch) {
  if (CpuFeatures::IsSupported(FPU)) {
    CpuFeatures::Scope scope(FPU);
    __ mtc1(answer, f0);
    if (op_ == Token::SHR) {
      __ Cvt_d_uw(f2, f0);
    } else {
      __ cvt_d_w(f2, f0);
    }
    __ Subu(scratch, heap_number, kHeapObjectTag);
    __ sdc1(f2, MemOperand(scratch, HeapNumber::kValueOffset));
  } else {
    Register scratch2 = VirtualFrame::scratch2();
    ASSERT(!scratch.is(scratch2));
    ASSERT(!answer.is(scratch2));
    ASSERT(!heap_number.is(scratch2));
    WriteInt32ToHeapNumberStub stub(answer, heap_number, scratch,
        scratch2);
    __ CallStub(&stub);
  }
}


void DeferredInlineSmiOperation::GenerateNonSmiInput() {
  // We know the left hand side is not a Smi and the right hand side is an
  // immediate value (value_) which can be represented as a Smi. We only
  // handle bit operations.
  ASSERT(Token::IsBitOp(op_));

  if (FLAG_debug_code) {
    __ Abort("Should not fall through!");
  }

  __ bind(&non_smi_input_);
  if (FLAG_debug_code) {
    __ AbortIfSmi(tos_register_);
  }

  // This routine uses the registers from a2 to t2.  At the moment they are
  // not used by the register allocator, but when they are it should use
  // SpillAll and MergeTo like DeferredInlineSmiOperation::Generate() above.

  Register heap_number_map = t3;
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
  __ lw(a3, FieldMemOperand(tos_register_, HeapNumber::kMapOffset));
  // Not a number, fall back to the GenericBinaryOpStub.
  __ Branch(entry_label(), ne, a3, Operand(heap_number_map));

  Register int32 = a2;
  // Not a 32bits signed int, fall back to the GenericBinaryOpStub.
  __ ConvertToInt32(tos_register_, int32, t0, t1, entry_label());

  // tos_register_ (a0 or a1): Original heap number.
  // int32: signed 32bits int.

  Label result_not_a_smi;
  int shift_value = value_ & 0x1f;
  switch (op_) {
    case Token::BIT_OR:  __ Or(int32, int32, value_); break;
    case Token::BIT_XOR: __ Xor(int32, int32, value_); break;
    case Token::BIT_AND: __ And(int32, int32, value_); break;
    case Token::SAR:
      ASSERT(!reversed_);
      if (shift_value != 0) {
         __ sra(int32, int32, shift_value);
      }
      break;
    case Token::SHR: {
      ASSERT(!reversed_);
      if (shift_value != 0) {
        __ srl(int32, int32, shift_value);
      }
      // SHR is special because it is required to produce a positive answer.
      if (CpuFeatures::IsSupported(FPU)) {
        __ Branch(&result_not_a_smi, lt, int32, Operand(zero_reg));
      } else {
        // Non FPU code cannot convert from unsigned to double, so fall back
        // to GenericBinaryOpStub.
        __ Branch(entry_label(), lt, int32, Operand(zero_reg));
      }
      break;
    }
    case Token::SHL:
      ASSERT(!reversed_);
      if (shift_value != 0) {
        __ sll(int32, int32, shift_value);
      }
      break;
    default: UNREACHABLE();
  }

  // Check that the *signed* result fits in a smi. Not necessary for AND, SAR
  // if the shift if more than 0 or SHR if the shit is more than 1.
  if (!( (op_ == Token::AND) ||
        ((op_ == Token::SAR) && (shift_value > 0)) ||
        ((op_ == Token::SHR) && (shift_value > 1)))) {
    __ Addu(a3, int32, Operand(0x40000000));
    __ Branch(&result_not_a_smi, lt, a3, Operand(zero_reg));
  }
  __ sll(tos_register_, int32, kSmiTagSize);
  Exit();

  if (result_not_a_smi.is_linked()) {
    __ bind(&result_not_a_smi);
    if (overwrite_mode_ != OVERWRITE_LEFT) {
      ASSERT((overwrite_mode_ == NO_OVERWRITE) ||
             (overwrite_mode_ == OVERWRITE_RIGHT));
      // If the allocation fails, fall back to the GenericBinaryOpStub.
      __ AllocateHeapNumber(t0, t1, t2, heap_number_map, entry_label());
      // Nothing can go wrong now, so overwrite tos.
      __ mov(tos_register_, t0);
    }

    // int32: answer as signed 32bits integer.
    // tos_register_: Heap number to write the answer into.
    WriteNonSmiAnswer(int32, tos_register_, a3);

    Exit();
  }
}


void DeferredInlineSmiOperation::GenerateAnswerOutOfRange() {
  // The input from a bitwise operation were Smis but the result cannot fit
  // into a Smi, so we store it into a heap number. tos_register_ holds the
  // result to be converted.
  ASSERT(Token::IsBitOp(op_));
  ASSERT(!reversed_);

  if (FLAG_debug_code) {
    __ Abort("Should not fall through!");
  }

  __ bind(&answer_out_of_range_);
  if (((value_ & 0x1f) == 0) && (op_ == Token::SHR)) {
    // >>> 0 is a special case where the result is already tagged but wrong
    // because the Smi is negative. We untag it.
    __ sra(tos_register_, tos_register_, kSmiTagSize);
  }

  // This routine uses the registers from a2 to t2.  At the moment they are
  // not used by the register allocator, but when they are it should use
  // SpillAll and MergeTo like DeferredInlineSmiOperation::Generate() above.

  // Allocate the result heap number.
  Register heap_number_map = t3;
  Register heap_number = t0;
  __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
  // If the allocation fails, fall back to the GenericBinaryOpStub.
  __ AllocateHeapNumber(heap_number, t1, t2, heap_number_map, entry_label());
  WriteNonSmiAnswer(tos_register_, heap_number, a3);
  __ mov(tos_register_, heap_number);

  Exit();
}


static bool PopCountLessThanEqual2(unsigned int x) {
  x &= x - 1;
  return (x & (x - 1)) == 0;
}


// Returns the index of the lowest bit set.
static int BitPosition(unsigned x) {
  int bit_posn = 0;
  while ((x & 0xf) == 0) {
    bit_posn += 4;
    x >>= 4;
  }
  while ((x & 1) == 0) {
    bit_posn++;
    x >>= 1;
  }
  return bit_posn;
}


// Can we multiply by x with max two shifts and an add.
// This answers yes to all integers from 2 to 10.
static bool IsEasyToMultiplyBy(int x) {
  if (x < 2) return false;                          // Avoid special cases.
  if (x > (Smi::kMaxValue + 1) >> 2) return false;  // Almost always overflows.
  if (IsPowerOf2(x)) return true;                   // Simple shift.
  if (PopCountLessThanEqual2(x)) return true;       // Shift and add and shift.
  if (IsPowerOf2(x + 1)) return true;               // Patterns like 11111.
  return false;
}


// Can multiply by anything that IsEasyToMultiplyBy returns true for.
// Source and destination may be the same register.  This routine does
// not set carry and overflow the way a mul instruction would.
static void InlineMultiplyByKnownInt(MacroAssembler* masm,
                                     Register source,
                                     Register destination,
                                     int known_int) {
  if (IsPowerOf2(known_int)) {
    masm->sll(destination, source, BitPosition(known_int));
  } else if (PopCountLessThanEqual2(known_int)) {
    int first_bit = BitPosition(known_int);
    int second_bit = BitPosition(known_int ^ (1 << first_bit));
    masm->sll(t0, source, second_bit - first_bit);
    masm->Addu(destination, source, Operand(t0));
    if (first_bit != 0) {
      masm->sll(destination, destination, first_bit);
    }
  } else {
    ASSERT(IsPowerOf2(known_int + 1));  // Patterns like 1111.
    int the_bit = BitPosition(known_int + 1);
    masm->sll(t0, source, the_bit);
    masm->Subu(destination, t0, Operand(source));
  }
}


void CodeGenerator::SmiOperation(Token::Value op,
                                 Handle<Object> value,
                                 bool reversed,
                                 OverwriteMode mode) {
  int int_value = Smi::cast(*value)->value();
  bool something_to_inline;
  bool both_sides_are_smi = frame_->KnownSmiAt(0);
  switch (op) {
    case Token::ADD:
    case Token::SUB:
    case Token::BIT_AND:
    case Token::BIT_OR:
    case Token::BIT_XOR: {
      something_to_inline = true;
      break;
    }
    case Token::SHL: {
      something_to_inline = (both_sides_are_smi || !reversed);
      break;
    }
    case Token::SHR:
    case Token::SAR: {
      if (reversed) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    case Token::MOD: {
      if (reversed || int_value < 2 || !IsPowerOf2(int_value)) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    case Token::MUL: {
      if (!IsEasyToMultiplyBy(int_value)) {
        something_to_inline = false;
      } else {
        something_to_inline = true;
      }
      break;
    }
    default: {
      something_to_inline = false;
      break;
    }
  }

  if (!something_to_inline) {
    if (!reversed) {
      // Push the rhs onto the virtual frame by putting it in a TOS register.
      Register rhs = frame_->GetTOSRegister();
      __ li(rhs, Operand(value));
      frame_->EmitPush(rhs, TypeInfo::Smi());
      GenericBinaryOperation(op, mode, GENERATE_INLINE_SMI, int_value);
    } else {
      // Pop the rhs, then push lhs and rhs in the right order.  Only performs
      // at most one pop, the rest takes place in TOS registers.
      Register lhs = frame_->GetTOSRegister();
      Register rhs = frame_->PopToRegister(lhs);
      __ li(lhs, Operand(value));
      frame_->EmitPush(lhs, TypeInfo::Smi());
      TypeInfo t = both_sides_are_smi ? TypeInfo::Smi() : TypeInfo::Unknown();
      frame_->EmitPush(rhs, t);
      GenericBinaryOperation(op, mode, GENERATE_INLINE_SMI,
                            GenericBinaryOpStub::kUnknownIntValue);
    }
    return;
  }

  // We move the top of stack to a register (normally no move is invoved).
  Register tos = frame_->PopToRegister();

  switch (op) {
    case Token::ADD: {
      Register scratch0 = VirtualFrame::scratch0();
      Register scratch1 = VirtualFrame::scratch1();
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);

      __ Addu(v0, tos, Operand(value));
      // Check for overflow.
      __ xor_(scratch0, v0, tos);
      __ Xor(scratch1, v0, Operand(value));
      __ and_(scratch0, scratch0, scratch1);
     // Overflow occurred if result is negative.
      deferred->Branch(lt, scratch0, Operand(zero_reg));
      __ And(scratch0, v0, Operand(kSmiTagMask));
      deferred->Branch(ne, scratch0, Operand(zero_reg));
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    case Token::SUB: {
      Register scratch0 = VirtualFrame::scratch0();
      Register scratch1 = VirtualFrame::scratch1();
      Register scratch2 = VirtualFrame::scratch2();
      DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);

      __ li(scratch0, Operand(value));
      if (reversed) {
        __ Subu(v0, scratch0, Operand(tos));
        __ xor_(scratch2, v0, scratch0);  // Check for overflow.
      } else {
        __ Subu(v0, tos, Operand(scratch0));
        __ xor_(scratch2, v0, tos);  // Check for overflow.
      }
      __ xor_(scratch1, scratch0, tos);
      __ and_(scratch2, scratch2, scratch1);
      // Overflow occurred if result is negative.
      deferred->Branch(lt, scratch2, Operand(zero_reg));
      if (!both_sides_are_smi) {
        __ And(scratch0, v0, Operand(kSmiTagMask));
        deferred->Branch(ne, scratch0, Operand(zero_reg));
      }
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    case Token::BIT_OR:
    case Token::BIT_XOR:
    case Token::BIT_AND: {
     if (both_sides_are_smi) {
         switch (op) {
          case Token::BIT_OR:  __ Or(tos, tos, Operand(value)); break;
          case Token::BIT_XOR: __ Xor(tos, tos, Operand(value)); break;
          case Token::BIT_AND: __ And(tos, tos, Operand(value)); break;
          default: UNREACHABLE();
        }
        frame_->EmitPush(tos, TypeInfo::Smi());
      } else {
        Register scratch = VirtualFrame::scratch0();
        DeferredInlineSmiOperation* deferred =
          new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
        __ And(scratch, tos, Operand(kSmiTagMask));
        deferred->JumpToNonSmiInput(ne, scratch, Operand(zero_reg));
        switch (op) {
          case Token::BIT_OR:  __ Or(tos, tos, Operand(value)); break;
          case Token::BIT_XOR: __ Xor(tos, tos, Operand(value)); break;
          case Token::BIT_AND: __ And(tos, tos, Operand(value)); break;
          default: UNREACHABLE();
        }
        deferred->BindExit();
        TypeInfo result_type =
            (op == Token::BIT_AND) ? TypeInfo::Smi() : TypeInfo::Integer32();
        frame_->EmitPush(tos, result_type);
      }
      break;
    }

    case Token::SHL:
      if (reversed) {
        ASSERT(both_sides_are_smi);
        int max_shift = 0;
        int max_result = int_value == 0 ? 1 : int_value;
        while (Smi::IsValid(max_result << 1)) {
          max_shift++;
          max_result <<= 1;
        }
        DeferredCode* deferred =
          new DeferredInlineSmiOperation(op, int_value, true, mode, tos);
        // Mask off the last 5 bits of the shift operand (rhs).  This is part
        // of the definition of shift in JS and we know we have a Smi so we
        // can safely do this.  The masked version gets passed to the
        // deferred code, but that makes no difference.
        __ And(tos, tos, Operand(Smi::FromInt(0x1f)));
        deferred->Branch(ge, tos, Operand(Smi::FromInt(max_shift)));
        Register scratch = VirtualFrame::scratch0();
        __ sra(scratch, tos, kSmiTagSize);             // Untag.
        __ li(tos, Operand(Smi::FromInt(int_value)));  // Load constant.
        __ sllv(tos, tos, scratch);                    // Shift constant.
        deferred->BindExit();
        TypeInfo result = TypeInfo::Integer32();
        frame_->EmitPush(tos, result);
        break;
      }
      // Fall through!
    case Token::SHR:
    case Token::SAR: {
      ASSERT(!reversed);
      int shift_value = int_value & 0x1f;  // Least significant 5 bits.
      TypeInfo result = TypeInfo::Number();

      if (op == Token::SHR) {
        if (shift_value > 1) {
          result = TypeInfo::Smi();
        } else if (shift_value > 0) {
          result = TypeInfo::Integer32();
        }
      } else if (op == Token::SAR) {
        if (shift_value > 0) {
          result = TypeInfo::Smi();
        } else {
          result = TypeInfo::Integer32();
        }
      } else {
        ASSERT(op == Token::SHL);
        result = TypeInfo::Integer32();
      }

      Register scratch = VirtualFrame::scratch0();
      DeferredInlineSmiOperation* deferred =
        new DeferredInlineSmiOperation(op, shift_value, false, mode, tos);
      if (!both_sides_are_smi) {
        __ And(v0, tos, Operand(kSmiTagMask));
        deferred->JumpToNonSmiInput(ne, v0, Operand(zero_reg));
      }

      switch (op) {
        case Token::SHL: {
          if (shift_value != 0) {
            int adjusted_shift = shift_value - kSmiTagSize;
            ASSERT(adjusted_shift >= 0);
            if (adjusted_shift != 0) {
              __ sll(tos, tos, adjusted_shift);
            }
            // Check that the *unsigned* result fits in a Smi.
            __ Addu(scratch, tos, Operand(0x40000000));
            deferred->JumpToAnswerOutOfRange(lt, scratch, Operand(zero_reg));
            __ sll(tos, tos, kSmiTagSize);
          }
          break;
        }
        case Token::SHR: {
          if (shift_value != 0) {
            __ sra(scratch, tos, kSmiTagSize);  // Remove tag.
            __ srl(tos, scratch, shift_value);
            if (shift_value == 1) {
              // Check that the *unsigned* result fits in a smi.
              // Neither of the two high-order bits can be set:
              // - 0x80000000: high bit would be lost when smi tagging
              // - 0x40000000: this number would convert to negative when Smi
              // tagging. These two cases can only happen with shifts
              // by 0 or 1 when handed a valid smi.
              Register scratch2 = VirtualFrame::scratch2();
              __ And(scratch2, tos, Operand(0xc0000000));
              if (!CpuFeatures::IsSupported(FPU)) {
                // If the unsigned result does not fit in a Smi, we require an
                // unsigned to double conversion. Without FPU V8 has to fall
                // back to the runtime. The deferred code will expect tos
                // to hold the original Smi to be shifted.
                __ sll(scratch, scratch, kSmiTagSize);
                // Only move if scratch2 != 0.
                __ movn(tos, scratch, scratch2);
              }
              deferred->JumpToAnswerOutOfRange(ne, scratch2, Operand(zero_reg));
            }
            __ sll(tos, tos, kSmiTagSize);
          } else {
            deferred->JumpToAnswerOutOfRange(lt, tos, Operand(zero_reg));
          }
          break;
        }
        case Token::SAR: {
          if (shift_value != 0) {
            // Do the shift and the tag removal in one operation.
            __ sra(tos, tos, (kSmiTagSize + shift_value));
            __ sll(tos, tos, kSmiTagSize);
           }
          break;
        }
        default: UNREACHABLE();
      }
      deferred->BindExit();
      frame_->EmitPush(tos, result);
      break;
    }

    case Token::MOD: {
      ASSERT(!reversed);
      ASSERT(int_value >= 2);
      ASSERT(IsPowerOf2(int_value));
      Register scratch = VirtualFrame::scratch0();
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
      unsigned mask = (0x80000000u | kSmiTagMask);
      __ And(scratch, tos, Operand(mask));
      // Go to deferred code on non-Smis and negative.
      deferred->Branch(ne, scratch, Operand(zero_reg));
      mask = (int_value << kSmiTagSize) - 1;
      __ And(v0, tos, Operand(mask));
      deferred->BindExit();
      __ mov(tos, v0);
      // Mod of positive power of 2 Smi gives a Smi if the lhs is an integer.
      frame_->EmitPush(
              tos, both_sides_are_smi ? TypeInfo::Smi() : TypeInfo::Number());
      break;
    }

    case Token::MUL: {
      ASSERT(IsEasyToMultiplyBy(int_value));
      Register scratch = VirtualFrame::scratch0();
      DeferredCode* deferred =
        new DeferredInlineSmiOperation(op, int_value, reversed, mode, tos);
      unsigned max_smi_that_wont_overflow = Smi::kMaxValue / int_value;
      max_smi_that_wont_overflow <<= kSmiTagSize;
      unsigned mask = 0x80000000u;
      while ((mask & max_smi_that_wont_overflow) == 0) {
        mask |= mask >> 1;
      }
      mask |= kSmiTagMask;
      // This does a single mask that checks for a too high value in a
      // conservative way and for a non-Smi.  It also filters out negative
      // numbers, unfortunately, but since this code is inline we prefer
      // brevity to comprehensiveness.
      __ And(scratch, tos, Operand(mask));
      deferred->Branch(ne, scratch, Operand(zero_reg));
      InlineMultiplyByKnownInt(masm_, tos, v0, int_value);
      deferred->BindExit();
      __ mov(tos, v0);
      frame_->EmitPush(tos);
      break;
    }

    default:
      UNREACHABLE();
      break;
  }
}


// On MIPS we load registers condReg1 and condReg2 with the values which should
// be compared. With the CodeGenerator::cc_reg_ condition, functions will be
// able to evaluate correctly the condition. (eg CodeGenerator::Branch)
void CodeGenerator::Comparison(Condition cc,
                               Expression* left,
                               Expression* right,
                               bool strict) {
  VirtualFrame::RegisterAllocationScope scope(this);

  if (left != NULL) Load(left);
  if (right != NULL) Load(right);

  // sp[0] : y  (right)
  // sp[1] : x  (left)

  // Strict only makes sense for equality comparisons.
  ASSERT(!strict || cc == eq);

  Register lhs;
  Register rhs;

  bool lhs_is_smi;
  bool rhs_is_smi;
  // We load the top two stack positions into registers chosen by the virtual
  // frame.  This should keep the register shuffling to a minimum.
  // Implement '>' and '<=' by reversal to obtain ECMA-262 conversion order.
  if (cc == gt || cc == le) {
    cc = ReverseCondition(cc);
    lhs_is_smi = frame_->KnownSmiAt(0);
    rhs_is_smi = frame_->KnownSmiAt(1);
    lhs = frame_->PopToRegister();
    rhs = frame_->PopToRegister(lhs);  // Don't pop to the same register again!
  } else {
    rhs_is_smi = frame_->KnownSmiAt(0);
    lhs_is_smi = frame_->KnownSmiAt(1);
    rhs = frame_->PopToRegister();
    lhs = frame_->PopToRegister(rhs);  // Don't pop to the same register again!
  }

  ASSERT(rhs.is(a0) || rhs.is(a1));
  ASSERT(lhs.is(a0) || lhs.is(a1));

  bool both_sides_are_smi = (lhs_is_smi && rhs_is_smi);
  JumpTarget exit;

  if (!both_sides_are_smi) {
  // Now we have the two sides in a0 and a1.  We flush any other registers
  // because the stub doesn't know about register allocation.
    frame_->SpillAll();
    Register scratch = VirtualFrame::scratch0();
    Register smi_test_reg;
    if (lhs_is_smi) {
      smi_test_reg = rhs;
    } else if (rhs_is_smi) {
      smi_test_reg = lhs;
    } else {
      __ Or(scratch, lhs, rhs);
      smi_test_reg = scratch;
    }

  __ And(scratch, smi_test_reg, kSmiTagMask);
  JumpTarget smi;
  smi.Branch(eq, scratch, Operand(zero_reg), no_hint);

  // Perform non-smi comparison by stub.
  // CompareStub takes arguments in a0 and a1, returns <0, >0 or 0 in v0.
  // We call with 0 args because there are 0 on the stack.
  CompareStub stub(cc, strict, NO_SMI_COMPARE_IN_STUB, lhs, rhs);
  frame_->CallStub(&stub, 0);
  __ mov(condReg1, v0);
  __ mov(condReg2, zero_reg);

  exit.Jump();

  // Do smi comparisons by pointer comparison.
  smi.Bind();
  }
  __ mov(condReg1, lhs);
  __ mov(condReg2, rhs);

  exit.Bind();
  cc_reg_ = cc;
}


void CodeGenerator::CallWithArguments(ZoneList<Expression*>* args,
                                      CallFunctionFlags flags,
                                      int position) {
  // Push the arguments ("left-to-right") on the stack.
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  // Record the position for debugging purposes.
  CodeForSourcePosition(position);

  // Use the shared code stub to call the function.
  InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
  CallFunctionStub call_function(arg_count, in_loop, flags);
  frame_->CallStub(&call_function, arg_count + 1);

  // Restore context and pop function from the stack.
  __ lw(cp, frame_->Context());
  frame_->Drop();  // Discard the TOS.
}


void CodeGenerator::CallApplyLazy(Expression* applicand,
                                  Expression* receiver,
                                  VariableProxy* arguments,
                                  int position) {
  // An optimized implementation of expressions of the form
  // x.apply(y, arguments).
  // If the arguments object of the scope has not been allocated,
  // and x.apply is Function.prototype.apply, this optimization
  // just copies y and the arguments of the current function on the
  // stack, as receiver and arguments, and calls x.
  // In the implementation comments, we call x the applicand
  // and y the receiver.

  ASSERT(ArgumentsMode() == LAZY_ARGUMENTS_ALLOCATION);
  ASSERT(arguments->IsArguments());

  // Load applicand.apply onto the stack. This will usually
  // give us a megamorphic load site. Not super, but it works.
  Load(applicand);
  Handle<String> name = Factory::LookupAsciiSymbol("apply");
  frame_->Dup();
  frame_->CallLoadIC(name, RelocInfo::CODE_TARGET);
  frame_->EmitPush(v0);

  // Load the receiver and the existing arguments object onto the
  // expression stack. Avoid allocating the arguments object here.
  Load(receiver);
  LoadFromSlot(scope()->arguments()->AsSlot(), NOT_INSIDE_TYPEOF);

  // At this point the top two stack elements are probably in registers
  // since they were just loaded.  Ensure they are in regs and get the
  // regs.
  Register receiver_reg = frame_->Peek2();
  Register arguments_reg = frame_->Peek();

  // From now on the frame is spilled.
  frame_->SpillAll();

  // Emit the source position information after having loaded the
  // receiver and the arguments.
  CodeForSourcePosition(position);
  // Contents of the stack at this point:
  //   sp[0]: arguments object of the current function or the hole.
  //   sp[1]: receiver
  //   sp[2]: applicand.apply
  //   sp[3]: applicand.

  // Check if the arguments object has been lazily allocated
  // already. If so, just use that instead of copying the arguments
  // from the stack. This also deals with cases where a local variable
  // named 'arguments' has been introduced.

  JumpTarget slow;
  Label done;
  __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
  slow.Branch(ne, at, Operand(arguments_reg));


  Label build_args;
  // Get rid of the arguments object probe.
  frame_->Drop();
  // Stack now has 3 elements on it.
  // Contents of stack at this point:
  //   sp[0]: receiver - in the receiver_reg register.
  //   sp[1]: applicand.apply
  //   sp[2]: applicand.

  // Check that the receiver really is a JavaScript object.
  __ BranchOnSmi(receiver_reg, &build_args);
  // We allow all JSObjects including JSFunctions.  As long as
  // JS_FUNCTION_TYPE is the last instance type and it is right
  // after LAST_JS_OBJECT_TYPE, we do not have to check the upper
  // bound.
  STATIC_ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  STATIC_ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);

  __ GetObjectType(receiver_reg, a2, a3);
  __ Branch(&build_args, lt, a3, Operand(FIRST_JS_OBJECT_TYPE));

  // Check that applicand.apply is Function.prototype.apply.
  __ lw(v0, MemOperand(sp, kPointerSize));
  __ BranchOnSmi(v0, &build_args);

  __ GetObjectType(a0, a1, a2);
  __ Branch(&build_args, ne, a2, Operand(JS_FUNCTION_TYPE));

  Handle<Code> apply_code(Builtins::builtin(Builtins::FunctionApply));
  __ lw(a1, FieldMemOperand(v0, JSFunction::kCodeEntryOffset));
  __ Subu(a1, a1, Operand(Code::kHeaderSize - kHeapObjectTag));
  __ Branch(&build_args, ne, a1, Operand(apply_code));

  // Check that applicand is a function.
  __ lw(a1, MemOperand(sp, 2 * kPointerSize));
  __ BranchOnSmi(a1, &build_args);

  __ GetObjectType(a1, a2, a3);
  __ Branch(&build_args, ne, a3, Operand(JS_FUNCTION_TYPE));

  // Copy the arguments to this function possibly from the
  // adaptor frame below it.
  Label invoke, adapted;
  __ lw(a2, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(a3, MemOperand(a2, StandardFrameConstants::kContextOffset));
  __ Branch(&adapted, eq, a3,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // No arguments adaptor frame. Copy fixed number of arguments.
  __ Or(v0, zero_reg, Operand(scope()->num_parameters()));
  for (int i = 0; i < scope()->num_parameters(); i++) {
    __ lw(a2, frame_->ParameterAt(i));
    __ push(a2);
  }
  __ jmp(&invoke);

  // Arguments adaptor frame present. Copy arguments from there, but
  // avoid copying too many arguments to avoid stack overflows.
  __ bind(&adapted);
  static const uint32_t kArgumentsLimit = 1 * KB;
  __ lw(v0, MemOperand(a2, ArgumentsAdaptorFrameConstants::kLengthOffset));
  __ srl(v0, v0, kSmiTagSize);
  __ mov(a3, v0);
  __ Branch(&build_args, gt, v0, Operand(kArgumentsLimit));

  // Loop through the arguments pushing them onto the execution
  // stack. We don't inform the virtual frame of the push, so we don't
  // have to worry about getting rid of the elements from the virtual
  // frame.
  Label loop;
  // a3 is a small non-negative integer, due to the test above.
  __ Branch(&invoke, eq, a3, Operand(zero_reg));

  // Compute the address of the first argument.
  __ sll(t0, a3, kPointerSizeLog2);
  __ Addu(a2, a2, t0);
  __ Addu(a2, a2, Operand(kPointerSize));
  __ bind(&loop);
  // Post-decrement argument address by kPointerSize on each iteration.
  __ lw(t0, MemOperand(a2));
  __ Subu(a2, a2, Operand(kPointerSize));
  __ push(t0);
  __ Subu(a3, a3, Operand(1));
  __ Branch(&loop, gt, a3, Operand(zero_reg));

  // Invoke the function.
  __ bind(&invoke);
  ParameterCount actual(a0);
  __ InvokeFunction(a1, actual, CALL_FUNCTION);
  // Drop applicand.apply and applicand from the stack, and push
  // the result of the function call, but leave the spilled frame
  // unchanged, with 3 elements, so it is correct when we compile the
  // slow-case code.
  __ Addu(sp, sp, Operand(2 * kPointerSize));
  __ push(a0);
  // Stack now has 1 element:
  //   sp[0]: result
  __ jmp(&done);

  // Slow-case: Allocate the arguments object since we know it isn't
  // there, and fall-through to the slow-case where we call
  // applicand.apply.
  __ bind(&build_args);
  // Stack now has 3 elements, because we have jumped from where:
  //   sp[0]: receiver
  //   sp[1]: applicand.apply
  //   sp[2]: applicand.
  StoreArgumentsObject(false);

  // Stack and frame now have 4 elements.
  slow.Bind();

  // Generic computation of x.apply(y, args) with no special optimization.
  // Flip applicand.apply and applicand on the stack, so
  // applicand looks like the receiver of the applicand.apply call.
  // Then process it as a normal function call.
  __ lw(v0, MemOperand(sp, 3 * kPointerSize));
  __ lw(a1, MemOperand(sp, 2 * kPointerSize));
  __ sw(v0, MemOperand(sp, 2 * kPointerSize));
  __ sw(a1, MemOperand(sp, 3 * kPointerSize));

  CallFunctionStub call_function(2, NOT_IN_LOOP, NO_CALL_FUNCTION_FLAGS);
  frame_->CallStub(&call_function, 3);
  // The function and its two arguments have been dropped.
  frame_->Drop();  // Drop the receiver as well.
  frame_->EmitPush(v0);
  frame_->SpillAll();  // A spilled frame is also jumping to label done.
  // Stack now has 1 element:
  //   sp[0]: result
  __ bind(&done);

  // Restore the context register after a call.
  __ lw(cp, frame_->Context());
}


void CodeGenerator::Branch(bool if_true, JumpTarget* target) {
  ASSERT(has_cc());
  Condition cc = if_true ? cc_reg_ : NegateCondition(cc_reg_);
  target->Branch(cc, condReg1, Operand(condReg2), no_hint);
  cc_reg_ = cc_always;
}


void CodeGenerator::CheckStack() {
  frame_->SpillAll();
  Comment cmnt(masm_, "[ check stack");

  __ LoadRoot(t0, Heap::kStackLimitRootIndex);
  StackCheckStub stub;
  // Call the stub if lower.
  __ Push(ra);
  __ Call(Operand(reinterpret_cast<intptr_t>(stub.GetCode().location()),
          RelocInfo::CODE_TARGET),
          Uless, sp, Operand(t0));
  __ Pop(ra);
}


void CodeGenerator::VisitStatements(ZoneList<Statement*>* statements) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  for (int i = 0; frame_ != NULL && i < statements->length(); i++) {
    Visit(statements->at(i));
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitBlock(Block* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Block");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();
  VisitStatements(node->statements());
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::DeclareGlobals(Handle<FixedArray> pairs) {
  frame_->EmitPush(cp);
  frame_->EmitPush(Operand(pairs));
  frame_->EmitPush(Operand(Smi::FromInt(is_eval() ? 1 : 0)));

  frame_->CallRuntime(Runtime::kDeclareGlobals, 3);
  // The result is discarded.
}


void CodeGenerator::VisitDeclaration(Declaration* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Declaration");
  Variable* var = node->proxy()->var();
  ASSERT(var != NULL);  // Must have been resolved.
  Slot* slot = var->AsSlot();

  // If it was not possible to allocate the variable at compile time,
  // we need to "declare" it at runtime to make sure it actually
  // exists in the local context.
  if (slot != NULL && slot->type() == Slot::LOOKUP) {
    // Variables with a "LOOKUP" slot were introduced as non-locals
    // during variable resolution and must have mode DYNAMIC.
    ASSERT(var->is_dynamic());
    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(var->name()));
    // Declaration nodes are always declared in only two modes.
    ASSERT(node->mode() == Variable::VAR || node->mode() == Variable::CONST);
    PropertyAttributes attr = node->mode() == Variable::VAR ? NONE : READ_ONLY;
    frame_->EmitPush(Operand(Smi::FromInt(attr)));
    // Push initial value, if any.
    // Note: For variables we must not push an initial value (such as
    // 'undefined') because we may have a (legal) redeclaration and we
    // must not destroy the current value.
    if (node->mode() == Variable::CONST) {
      frame_->EmitPushRoot(Heap::kTheHoleValueRootIndex);
    } else if (node->fun() != NULL) {
      Load(node->fun());
    } else {
      frame_->EmitPush(zero_reg);
    }

    frame_->CallRuntime(Runtime::kDeclareContextSlot, 4);
    // Ignore the return value (declarations are statements).

    ASSERT(frame_->height() == original_height);
    return;
  }

  ASSERT(!var->is_global());

  // If we have a function or a constant, we need to initialize the variable.
  Expression* val = NULL;
  if (node->mode() == Variable::CONST) {
    val = new Literal(Factory::the_hole_value());
  } else {
    val = node->fun();  // NULL if we don't have a function.
  }

  if (val != NULL) {
    WriteBarrierCharacter wb_info =
        val->type()->IsLikelySmi() ? LIKELY_SMI : UNLIKELY_SMI;
    if (val->AsLiteral() != NULL) wb_info = NEVER_NEWSPACE;
    // Set initial value.
    Reference target(this, node->proxy());
    Load(val);
    target.SetValue(NOT_CONST_INIT, wb_info);

    // Get rid of the assigned value (declarations are statements).
    frame_->Drop();
  }
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitExpressionStatement(ExpressionStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ ExpressionStatement");
  CodeForStatementPosition(node);
  Expression* expression = node->expression();
  expression->MarkAsStatement();
  Load(expression);
  frame_->Drop();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitEmptyStatement(EmptyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "// EmptyStatement");
  CodeForStatementPosition(node);
  // nothing to do
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitIfStatement(IfStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ IfStatement");
  // Generate different code depending on which parts of the if statement
  // are present or not.
  bool has_then_stm = node->HasThenStatement();
  bool has_else_stm = node->HasElseStatement();

  CodeForStatementPosition(node);

  JumpTarget exit;
  if (has_then_stm && has_else_stm) {
    Comment cmnt(masm_, "[ IfThenElse");
    JumpTarget then;
    JumpTarget else_;
    // if (cond)
    LoadCondition(node->condition(), &then, &else_, true);
    if (frame_ != NULL) {
      Branch(false, &else_);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      Visit(node->then_statement());
    }
    if (frame_ != NULL) {
      exit.Jump();
    }
    // else
    if (else_.is_linked()) {
      else_.Bind();
      Visit(node->else_statement());
    }

  } else if (has_then_stm) {
    Comment cmnt(masm_, "[ IfThen");
    ASSERT(!has_else_stm);
    JumpTarget then;
    // if (cond)
    LoadCondition(node->condition(), &then, &exit, true);
    if (frame_ != NULL) {
      Branch(false, &exit);
    }
    // then
    if (frame_ != NULL || then.is_linked()) {
      then.Bind();
      Visit(node->then_statement());
    }

  } else if (has_else_stm) {
    Comment cmnt(masm_, "[ IfElse");
    ASSERT(!has_then_stm);
    JumpTarget else_;
    // if (!cond)
    LoadCondition(node->condition(), &exit, &else_, true);
    if (frame_ != NULL) {
      Branch(true, &exit);
    }
    // else
    if (frame_ != NULL || else_.is_linked()) {
      else_.Bind();
      Visit(node->else_statement());
    }

  } else {
    Comment cmnt(masm_, "[ If");
    ASSERT(!has_then_stm && !has_else_stm);
    // if (cond)
    LoadCondition(node->condition(), &exit, &exit, false);
    if (frame_ != NULL) {
      if (has_cc()) {
        cc_reg_ = cc_always;
      } else {
        frame_->Drop();
      }
    }
  }

  // end
  if (exit.is_linked()) {
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitContinueStatement(ContinueStatement* node) {
  Comment cmnt(masm_, "[ ContinueStatement");
  CodeForStatementPosition(node);
  node->target()->continue_target()->Jump();
}


void CodeGenerator::VisitBreakStatement(BreakStatement* node) {
  Comment cmnt(masm_, "[ BreakStatement");
  CodeForStatementPosition(node);
  node->target()->break_target()->Jump();
}


void CodeGenerator::VisitReturnStatement(ReturnStatement* node) {
  Comment cmnt(masm_, "[ ReturnStatement");

  CodeForStatementPosition(node);
  Load(node->expression());
  frame_->EmitPop(v0);
  frame_->PrepareForReturn();
  if (function_return_is_shadowed_) {
    function_return_.Jump();
  } else {
    // Pop the result from the frame and prepare the frame for
    // returning thus making it easier to merge.
    if (function_return_.is_bound()) {
      // If the function return label is already bound we reuse the
      // code by jumping to the return site.
      function_return_.Jump();
    } else {
      function_return_.Bind();
      GenerateReturnSequence();
    }
  }
}


void CodeGenerator::GenerateReturnSequence() {
  if (FLAG_trace) {
    // Push the return value on the stack as the parameter.
    // Runtime::TraceExit returns the parameter as it is.
    frame_->EmitPush(v0);
    frame_->CallRuntime(Runtime::kTraceExit, 1);
  }

#ifdef DEBUG
  // Add a label for checking the size of the code used for returning.
  Label check_exit_codesize;
  masm_->bind(&check_exit_codesize);
#endif
  // Make sure that the trampoline pool is not emitted inside of the return
  // sequence.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Tear down the frame which will restore the caller's frame pointer and
    // the link register.
    frame_->Exit();

    // Here we use masm_-> instead of the __ macro to avoid the code coverage
    // tool from instrumenting as we rely on the code size here.
    int32_t sp_delta = (scope()->num_parameters() + 1) * kPointerSize;
    masm_-> Addu(sp, sp, Operand(sp_delta));
    masm_-> Ret();
    DeleteFrame();

#ifdef DEBUG
    // Check that the size of the code used for returning matches what is
    // expected by the debugger. If the sp_delta above cannot be encoded in
    // the add instruction the add will generate two instructions.
    int return_sequence_length =
        masm_->InstructionsGeneratedSince(&check_exit_codesize);
    CHECK(return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions ||
          return_sequence_length ==
          Assembler::kJSReturnSequenceInstructions + 1);
#endif
  }
}


void CodeGenerator::VisitWithEnterStatement(WithEnterStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ WithEnterStatement");
  CodeForStatementPosition(node);
  Load(node->expression());
  if (node->is_catch_block()) {
    frame_->CallRuntime(Runtime::kPushCatchContext, 1);
  } else {
    frame_->CallRuntime(Runtime::kPushContext, 1);
  }
#ifdef DEBUG
  JumpTarget verified_true;
  verified_true.Branch(eq, v0, Operand(cp), no_hint);
  __ stop("PushContext: v0 is expected to be the same as cp");
  verified_true.Bind();
#endif
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitWithExitStatement(WithExitStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ WithExitStatement");
  CodeForStatementPosition(node);
  // Pop context.
  __ lw(cp, ContextOperand(cp, Context::PREVIOUS_INDEX));
  // Update context local.
  __ sw(cp, frame_->Context());
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitSwitchStatement(SwitchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ SwitchStatement");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();

  Load(node->tag());

  JumpTarget next_test;
  JumpTarget fall_through;
  JumpTarget default_entry;
  JumpTarget default_exit(JumpTarget::BIDIRECTIONAL);
  ZoneList<CaseClause*>* cases = node->cases();
  int length = cases->length();
  CaseClause* default_clause = NULL;

  for (int i = 0; i < length; i++) {
    CaseClause* clause = cases->at(i);
    if (clause->is_default()) {
      // Remember the default clause and compile it at the end.
      default_clause = clause;
      continue;
    }

    Comment cmnt(masm_, "[ Case clause");
    // Compile the test.
    next_test.Bind();
    next_test.Unuse();
    // Duplicate TOS.
    frame_->Dup();
    Comparison(eq, NULL, clause->label(), true);
    Branch(false, &next_test);

    // Before entering the body from the test, remove the switch value from
    // the stack.
    frame_->Drop();

    // Label the body so that fall through is enabled.
    if (i > 0 && cases->at(i - 1)->is_default()) {
      default_exit.Bind();
    } else {
      fall_through.Bind();
      fall_through.Unuse();
    }
    VisitStatements(clause->statements());

    // If control flow can fall through from the body, jump to the next body
    // or the end of the statement.
    if (frame_ != NULL) {
      if (i < length - 1 && cases->at(i + 1)->is_default()) {
        default_entry.Jump();
      } else {
        fall_through.Jump();
      }
    }
  }

  // The final "test" removes the switch value.
  next_test.Bind();
  frame_->Drop();

  // If there is a default clause, compile it.
  if (default_clause != NULL) {
    Comment cmnt(masm_, "[ Default clause");
    default_entry.Bind();
    VisitStatements(default_clause->statements());
    // If control flow can fall out of the default and there is a case after
    // it, jump to that case's body.
    if (frame_ != NULL && default_exit.is_bound()) {
      default_exit.Jump();
    }
  }

  if (fall_through.is_linked()) {
    fall_through.Bind();
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  node->break_target()->Unuse();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDoWhileStatement(DoWhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ DoWhileStatement");
  CodeForStatementPosition(node);
  node->break_target()->SetExpectedHeight();
  JumpTarget body(JumpTarget::BIDIRECTIONAL);
  IncrementLoopNesting();

  // Label the top of the loop for the backward CFG edge.  If the test
  // is always true we can use the continue target, and if the test is
  // always false there is no need.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  switch (info) {
    case ALWAYS_TRUE:
      node->continue_target()->SetExpectedHeight();
      node->continue_target()->Bind();
      break;
    case ALWAYS_FALSE:
      node->continue_target()->SetExpectedHeight();
      break;
    case DONT_KNOW:
      node->continue_target()->SetExpectedHeight();
      body.Bind();
      break;
  }

  CheckStack();  // TODO(1222600): ignore if body contains calls.
  Visit(node->body());

      // Compile the test.
  switch (info) {
    case ALWAYS_TRUE:
      // If control can fall off the end of the body, jump back to the
      // top.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
      break;
    case ALWAYS_FALSE:
      // If we have a continue in the body, we only have to bind its
      // jump target.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      break;
    case DONT_KNOW:
      // We have to compile the test expression if it can be reached by
      // control flow falling out of the body or via continue.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        LoadCondition(node->cond(), &body, node->break_target(), true);
        if (has_valid_frame()) {
          // A invalid frame here indicates that control did not
          // fall out of the test expression.
          Branch(true, &body);
        }
      }
      break;
  }

  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitWhileStatement(WhileStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ WhileStatement");
  CodeForStatementPosition(node);

  // If the test is never true and has no side effects there is no need
  // to compile the test or body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->SetExpectedHeight();
  IncrementLoopNesting();

  // Label the top of the loop with the continue target for the backward
  // CFG edge.
  node->continue_target()->SetExpectedHeight();
  node->continue_target()->Bind();


  if (info == DONT_KNOW) {
    JumpTarget body(JumpTarget::BIDIRECTIONAL);
    LoadCondition(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      // A NULL frame indicates that control did not fall out of the
      // test expression.
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): Ignore if body contains calls.
    Visit(node->body());

    // If control flow can fall out of the body, jump back to the top.
    if (has_valid_frame()) {
      node->continue_target()->Jump();
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForStatement(ForStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ ForStatement");
  CodeForStatementPosition(node);
  if (node->init() != NULL) {
    Visit(node->init());
  }

  // If the test is never true there is no need to compile the test or
  // body.
  ConditionAnalysis info = AnalyzeCondition(node->cond());
  if (info == ALWAYS_FALSE) return;

  node->break_target()->SetExpectedHeight();
  IncrementLoopNesting();

  // We know that the loop index is a smi if it is not modified in the
  // loop body and it is checked against a constant limit in the loop
  // condition.  In this case, we reset the static type information of the
  // loop index to smi before compiling the body, the update expression, and
  // the bottom check of the loop condition.
  TypeInfoCodeGenState type_info_scope(this,
                                       node->is_fast_smi_loop() ?
                                       node->loop_variable()->AsSlot() :
                                       NULL,
                                       TypeInfo::Smi());
  // If there is no update statement, label the top of the loop with the
  // continue target, otherwise with the loop target.
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  if (node->next() == NULL) {
    node->continue_target()->SetExpectedHeight();
    node->continue_target()->Bind();
  } else {
    node->continue_target()->SetExpectedHeight();
    loop.Bind();
  }

  // If the test is always true, there is no need to compile it.
  if (info == DONT_KNOW) {
    JumpTarget body;
    LoadCondition(node->cond(), &body, node->break_target(), true);
    if (has_valid_frame()) {
      Branch(false, node->break_target());
    }
    if (has_valid_frame() || body.is_linked()) {
      body.Bind();
    }
  }

  if (has_valid_frame()) {
    CheckStack();  // TODO(1222600): ignore if body contains calls.
    Visit(node->body());

    if (node->next() == NULL) {
      // If there is no update statement and control flow can fall out
      // of the loop, jump directly to the continue label.
      if (has_valid_frame()) {
        node->continue_target()->Jump();
      }
    } else {
      // If there is an update statement and control flow can reach it
      // via falling out of the body of the loop or continuing, we
      // compile the update statement.
      if (node->continue_target()->is_linked()) {
        node->continue_target()->Bind();
      }
      if (has_valid_frame()) {
        // Record source position of the statement as this code which is
        // after the code for the body actually belongs to the loop
        // statement and not the body.
        CodeForStatementPosition(node);
        Visit(node->next());
        loop.Jump();
      }
    }
  }
  if (node->break_target()->is_linked()) {
    node->break_target()->Bind();
  }
  DecrementLoopNesting();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitForInStatement(ForInStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif

  Comment cmnt(masm_, "[ ForInStatement");
  CodeForStatementPosition(node);

  JumpTarget primitive;
  JumpTarget jsobject;
  JumpTarget fixed_array;
  JumpTarget entry(JumpTarget::BIDIRECTIONAL);
  JumpTarget end_del_check;
  JumpTarget exit;

  // Get the object to enumerate over (converted to JSObject).
  Load(node->enumerable());
  VirtualFrame::SpilledScope spilled_scope(frame_);
  // Both SpiderMonkey and kjs ignore null and undefined in contrast
  // to the specification.  12.6.4 mandates a call to ToObject.
  frame_->EmitPop(a0);
  __ LoadRoot(t2, Heap::kUndefinedValueRootIndex);
  exit.Branch(eq, a0, Operand(t2), no_hint);
  __ LoadRoot(t2, Heap::kNullValueRootIndex);
  exit.Branch(eq, a0, Operand(t2), no_hint);

  // Stack layout in body:
  // [iteration counter (Smi)]
  // [length of array]
  // [FixedArray]
  // [Map or 0]
  // [Object]

  // Check if enumerable is already a JSObject
  __ And(t0, a0, Operand(kSmiTagMask));
  primitive.Branch(eq, t0, Operand(zero_reg), no_hint);
  __ GetObjectType(a0, a1, a1);
  jsobject.Branch(hs, a1, Operand(FIRST_JS_OBJECT_TYPE), no_hint);

  primitive.Bind();
  frame_->EmitPush(a0);
  frame_->InvokeBuiltin(Builtins::TO_OBJECT, CALL_JS, 1);
  __ mov(a0, v0);

  jsobject.Bind();
  // Get the set of properties (as a FixedArray or Map).
  // r0: value to be iterated over
  frame_->EmitPush(a0);  // Push the object being iterated over.

  // Check cache validity in generated code. This is a fast case for
  // the JSObject::IsSimpleEnum cache validity checks. If we cannot
  // guarantee cache validity, call the runtime system to check cache
  // validity or get the property names in a fixed array.
  JumpTarget call_runtime;
  JumpTarget loop(JumpTarget::BIDIRECTIONAL);
  JumpTarget check_prototype;
  JumpTarget use_cache;
  __ mov(a1, a0);
  loop.Bind();
  // Check that there are no elements.
  __ lw(a2, FieldMemOperand(a1, JSObject::kElementsOffset));
  __ LoadRoot(t0, Heap::kEmptyFixedArrayRootIndex);
  call_runtime.Branch(ne, a2, Operand(t0), no_hint);
  // Check that instance descriptors are not empty so that we can
  // check for an enum cache.  Leave the map in a3 for the subsequent
  // prototype load.
  __ lw(a3, FieldMemOperand(a1, HeapObject::kMapOffset));
  __ lw(a2, FieldMemOperand(a3, Map::kInstanceDescriptorsOffset));
  __ LoadRoot(t2, Heap::kEmptyDescriptorArrayRootIndex);
  call_runtime.Branch(eq, a2, Operand(t2), no_hint);
  // Check that there in an enum cache in the non-empty instance
  // descriptors.  This is the case if the next enumeration index
  // field does not contain a smi.
  __ lw(a2, FieldMemOperand(a2, DescriptorArray::kEnumerationIndexOffset));
  __ And(t1, a2, Operand(kSmiTagMask));
  call_runtime.Branch(eq, t1, Operand(zero_reg), no_hint);
  // For all objects but the receiver, check that the cache is empty.
  // t0: empty fixed array root.
  check_prototype.Branch(eq, a1, Operand(a0), no_hint);
  __ lw(a2, FieldMemOperand(a2, DescriptorArray::kEnumCacheBridgeCacheOffset));
  call_runtime.Branch(ne, a2, Operand(t0), no_hint);
  check_prototype.Bind();
  // Load the prototype from the map and loop if non-null.
  __ lw(a1, FieldMemOperand(a3, Map::kPrototypeOffset));
  __ LoadRoot(t2, Heap::kNullValueRootIndex);
  loop.Branch(ne, a1, Operand(t2), no_hint);
  // The enum cache is valid.  Load the map of the object being
  // iterated over and use the cache for the iteration.
  __ lw(a0, FieldMemOperand(a0, HeapObject::kMapOffset));
  use_cache.Jump();

  call_runtime.Bind();
  // Call the runtime to get the property names for the object.
  frame_->EmitPush(a0);  // push the object (slot 4) for the runtime call
  frame_->CallRuntime(Runtime::kGetPropertyNamesFast, 1);
  __ mov(a0, v0);

  // If we got a map from the runtime call, we can do a fast
  // modification check. Otherwise, we got a fixed array, and we have
  // to do a slow check.
  // a0: map or fixed array (result from call to
  // Runtime::kGetPropertyNamesFast)
  __ mov(a2, a0);
  __ lw(a1, FieldMemOperand(a2, HeapObject::kMapOffset));
  __ LoadRoot(t2, Heap::kMetaMapRootIndex);
  fixed_array.Branch(ne, a1, Operand(t2), no_hint);

  use_cache.Bind();

  // Get enum cache
  // v0: map (either the result from a call to
  // Runtime::kGetPropertyNamesFast or has been fetched directly from
  // the object)
  __ mov(a1, a0);
  __ lw(a1, FieldMemOperand(a1, Map::kInstanceDescriptorsOffset));
  __ lw(a1, FieldMemOperand(a1, DescriptorArray::kEnumerationIndexOffset));
  __ lw(a2,
         FieldMemOperand(a1, DescriptorArray::kEnumCacheBridgeCacheOffset));

  frame_->EmitPush(a0);  // map
  frame_->EmitPush(a2);  // enum cache bridge cache
  __ lw(a0, FieldMemOperand(a2, FixedArray::kLengthOffset));
  frame_->EmitPush(a0);
  __ li(a0, Operand(Smi::FromInt(0)));
  frame_->EmitPush(a0);
  entry.Jump();

  fixed_array.Bind();
  __ li(a1, Operand(Smi::FromInt(0)));
  frame_->EmitPush(a1);  // insert 0 in place of Map
  frame_->EmitPush(a0);

  // Push the length of the array and the initial index onto the stack.
  __ lw(a0, FieldMemOperand(a0, FixedArray::kLengthOffset));
  frame_->EmitPush(a0);
  __ li(a0, Operand(Smi::FromInt(0)));  // init index
  frame_->EmitPush(a0);

  // Condition.
  entry.Bind();
  // sp[0] : index
  // sp[1] : array/enum cache length
  // sp[2] : array or enum cache
  // sp[3] : 0 or map
  // sp[4] : enumerable
  // Grab the current frame's height for the break and continue
  // targets only after all the state is pushed on the frame.
  node->break_target()->SetExpectedHeight();
  node->continue_target()->SetExpectedHeight();

  __ lw(a0, frame_->ElementAt(0));  // load the current count
  __ lw(a1, frame_->ElementAt(1));  // load the length
  node->break_target()->Branch(hs, a0, Operand(a1));

  // Get the i'th entry of the array.
  __ lw(a2, frame_->ElementAt(2));
  __ Addu(a2, a2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  __ sll(t2, a0, kPointerSizeLog2 - kSmiTagSize);  // Scale index.
  __ addu(t2, t2, a2);  // Base + index.
  __ lw(a3, MemOperand(t2));

  // Get Map or 0.
  __ lw(a2, frame_->ElementAt(3));
  // Check if this (still) matches the map of the enumerable.
  // If not, we have to filter the key.
  __ lw(a1, frame_->ElementAt(4));
  __ lw(a1, FieldMemOperand(a1, HeapObject::kMapOffset));
  end_del_check.Branch(eq, a1, Operand(a2), no_hint);

  // Convert the entry to a string (or null if it isn't a property anymore).
  __ lw(a0, frame_->ElementAt(4));  // push enumerable
  frame_->EmitPush(a0);
  frame_->EmitPush(a3);  // push entry
  frame_->InvokeBuiltin(Builtins::FILTER_KEY, CALL_JS, 2);
  __ mov(a3, v0);
  // If the property has been removed while iterating, we just skip it.
  node->continue_target()->Branch(eq, a3, Operand(zero_reg));

  end_del_check.Bind();
  // Store the entry in the 'each' expression and take another spin in the
  // loop.  a3: i'th entry of the enum cache (or string there of)
  frame_->EmitPush(a3);  // push entry
  { VirtualFrame::RegisterAllocationScope scope(this);
    Reference each(this, node->each());
    if (!each.is_illegal()) {
      if (each.size() > 0) {
        // Loading a reference may leave the frame in an unspilled state.
        frame_->SpillAll();  // Sync stack to memory.
        // Get the value (under the reference on the stack) from memory.
        __ lw(a0, frame_->ElementAt(each.size()));
        frame_->EmitPush(a0);
        each.SetValue(NOT_CONST_INIT, UNLIKELY_SMI);
        frame_->Drop(2);
      } else {
        // If the reference was to a slot we rely on the convenient property
        // that it doesn't matter whether a value (eg, a3 pushed above) is
        // right on top of or right underneath a zero-sized reference.
        each.SetValue(NOT_CONST_INIT, UNLIKELY_SMI);
        frame_->Drop(1);  // Drop the result of the set operation.
      }
    }
  }
  // Body.
  CheckStack();  // TODO(1222600): ignore if body contains calls.
  { VirtualFrame::RegisterAllocationScope scope(this);
    Visit(node->body());
  }

  // Next.  Reestablish a spilled frame in case we are coming here via
  // a continue in the body.
  node->continue_target()->Bind();
  frame_->SpillAll();
  frame_->EmitPop(a0);
  __ Addu(a0, a0, Operand(Smi::FromInt(1)));
  frame_->EmitPush(a0);
  entry.Jump();

  // Cleanup.  No need to spill because VirtualFrame::Drop is safe for
  // any frame.
  node->break_target()->Bind();
  frame_->Drop(5);

  // Exit.
  exit.Bind();
  node->continue_target()->Unuse();
  node->break_target()->Unuse();
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::VisitTryCatchStatement(TryCatchStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ TryCatchStatement");
  CodeForStatementPosition(node);

  JumpTarget try_block;
  JumpTarget exit;

  try_block.Call();
  // --- Catch block ---
  frame_->EmitPush(v0);

  // Store the caught exception in the catch variable.
  Variable* catch_var = node->catch_var()->var();
  ASSERT(catch_var != NULL && catch_var->AsSlot() != NULL);
  StoreToSlot(catch_var->AsSlot(), NOT_CONST_INIT);

  // Remove the exception from the stack.
  frame_->Drop();

  { VirtualFrame::RegisterAllocationScope scope(this);
    VisitStatements(node->catch_block()->statements());
  }
  if (frame_ != NULL) {
    exit.Jump();
  }

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_CATCH_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. During shadowing, the original label is hidden as the
  // LabelShadow and operations on the original actually affect the
  // shadowing label.
  //
  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  { VirtualFrame::RegisterAllocationScope scope(this);
    VisitStatements(node->try_block()->statements());
  }

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  bool has_unlinks = false;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    has_unlinks = has_unlinks || shadows[i]->is_linked();
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from try chain.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame. Unlink from
    // the handler list and drop the rest of this handler from the
    // frame.
    STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);
    if (has_unlinks) {
      exit.Jump();
    }
  }

  // Generate unlink code for the (formerly) shadowing labels that have been
  // jumped to.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // Unlink from try chain;
      shadows[i]->Bind();
      // Because we can be jumping here (to spilled code) from unspilled
      // code, we need to reestablish a spilled frame at this block.
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that we
      // break from (eg, for...in) may have left stuff on the stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        frame_->PrepareForReturn();
      }
      shadows[i]->other_target()->Jump();
    }
  }

  exit.Bind();
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitTryFinallyStatement(TryFinallyStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::SpilledScope spilled_scope(frame_);
  Comment cmnt(masm_, "[ TryFinallyStatement");
  CodeForStatementPosition(node);

  // State: Used to keep track of reason for entering the finally
  // block. Should probably be extended to hold information for
  // break/continue from within the try block.
  enum { FALLING, THROWING, JUMPING };

  JumpTarget try_block;
  JumpTarget finally_block;

  try_block.Call();

  frame_->EmitPush(v0);  // Save exception object on the stack.
  // In case of thrown exceptions, this is where we continue.
  __ li(a2, Operand(Smi::FromInt(THROWING)));
  finally_block.Jump();

  // --- Try block ---
  try_block.Bind();

  frame_->PushTryHandler(TRY_FINALLY_HANDLER);
  int handler_height = frame_->height();

  // Shadow the labels for all escapes from the try block, including
  // returns. Shadowing hides the original label as the LabelShadow and
  // operations on the original actually affect the shadowing label.

  // We should probably try to unify the escaping labels and the return
  // label.
  int nof_escapes = node->escaping_targets()->length();
  List<ShadowTarget*> shadows(1 + nof_escapes);

  // Add the shadow target for the function return.
  static const int kReturnShadowIndex = 0;
  shadows.Add(new ShadowTarget(&function_return_));
  bool function_return_was_shadowed = function_return_is_shadowed_;
  function_return_is_shadowed_ = true;
  ASSERT(shadows[kReturnShadowIndex]->other_target() == &function_return_);

  // Add the remaining shadow targets.
  for (int i = 0; i < nof_escapes; i++) {
    shadows.Add(new ShadowTarget(node->escaping_targets()->at(i)));
  }

  // Generate code for the statements in the try block.
  { VirtualFrame::RegisterAllocationScope scope(this);
    VisitStatements(node->try_block()->statements());
  }

  // Stop the introduced shadowing and count the number of required unlinks.
  // After shadowing stops, the original labels are unshadowed and the
  // LabelShadows represent the formerly shadowing labels.
  int nof_unlinks = 0;
  for (int i = 0; i < shadows.length(); i++) {
    shadows[i]->StopShadowing();
    if (shadows[i]->is_linked()) nof_unlinks++;
  }
  function_return_is_shadowed_ = function_return_was_shadowed;

  // Get an external reference to the handler address.
  ExternalReference handler_address(Top::k_handler_address);

  // If we can fall off the end of the try block, unlink from the try
  // chain and set the state on the frame to FALLING.
  if (has_valid_frame()) {
    // The next handler address is on top of the frame.
    STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
    frame_->EmitPop(a1);
    __ li(a3, Operand(handler_address));
    __ sw(a1, MemOperand(a3));
    frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

    // Fake a top of stack value (unneeded when FALLING) and set the
    // state in a2, then jump around the unlink blocks if any.
    __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
    frame_->EmitPush(v0);
    __ li(a2, Operand(Smi::FromInt(FALLING)));
    if (nof_unlinks > 0) {
      finally_block.Jump();
    }
  }

  // Generate code to unlink and set the state for the (formerly)
  // shadowing targets that have been jumped to.
  for (int i = 0; i < shadows.length(); i++) {
    if (shadows[i]->is_linked()) {
      // If we have come from the shadowed return, the return value is
      // in (a non-refcounted reference to) r0.  We must preserve it
      // until it is pushed.
      //
      // Because we can be jumping here (to spilled code) from
      // unspilled code, we need to reestablish a spilled frame at
      // this block.
      shadows[i]->Bind();
      frame_->SpillAll();

      // Reload sp from the top handler, because some statements that
      // we break from (eg, for...in) may have left stuff on the
      // stack.
      __ li(a3, Operand(handler_address));
      __ lw(sp, MemOperand(a3));
      frame_->Forget(frame_->height() - handler_height);

      // Unlink this handler and drop it from the frame.  The next
      // handler address is currently on top of the frame.
      STATIC_ASSERT(StackHandlerConstants::kNextOffset == 0);
      frame_->EmitPop(a1);
      __ sw(a1, MemOperand(a3));
      frame_->Drop(StackHandlerConstants::kSize / kPointerSize - 1);

      if (i == kReturnShadowIndex) {
        // If this label shadowed the function return, materialize the
        // return value on the stack.
        frame_->EmitPush(v0);
      } else {
        // Fake TOS for targets that shadowed breaks and continues.
        __ LoadRoot(v0, Heap::kUndefinedValueRootIndex);
        frame_->EmitPush(v0);
      }
      __ li(a2, Operand(Smi::FromInt(JUMPING + i)));
      if (--nof_unlinks > 0) {
        // If this is not the last unlink block, jump around the next.
        finally_block.Jump();
      }
    }
  }

  // --- Finally block ---
  finally_block.Bind();

  // Push the state on the stack.
  frame_->EmitPush(a2);

  // We keep two elements on the stack - the (possibly faked) result
  // and the state - while evaluating the finally block.
  //
  // Generate code for the statements in the finally block.
  { VirtualFrame::RegisterAllocationScope scope(this);
    VisitStatements(node->finally_block()->statements());
  }

  if (has_valid_frame()) {
    // Restore state and return value or faked TOS.
    frame_->EmitPop(a2);
    frame_->EmitPop(v0);
  }

  // Generate code to jump to the right destination for all used
  // formerly shadowing targets.  Deallocate each shadow target.
  for (int i = 0; i < shadows.length(); i++) {
    if (has_valid_frame() && shadows[i]->is_bound()) {
      JumpTarget* original = shadows[i]->other_target();
      if (!function_return_is_shadowed_ && i == kReturnShadowIndex) {
        JumpTarget skip;
        skip.Branch(ne, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
        frame_->PrepareForReturn();
        original->Jump();
        skip.Bind();
      } else {
        original->Branch(eq, a2, Operand(Smi::FromInt(JUMPING + i)), no_hint);
      }
    }
  }

  if (has_valid_frame()) {
    // Check if we need to rethrow the exception.
    JumpTarget exit;
    exit.Branch(ne, a2, Operand(Smi::FromInt(THROWING)), no_hint);

    // Rethrow exception.
    frame_->EmitPush(v0);
    frame_->CallRuntime(Runtime::kReThrow, 1);

    // Done.
    exit.Bind();
  }
  ASSERT(!has_valid_frame() || frame_->height() == original_height);
}


void CodeGenerator::VisitDebuggerStatement(DebuggerStatement* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ DebuggerStatament");
  CodeForStatementPosition(node);
#ifdef ENABLE_DEBUGGER_SUPPORT
  frame_->DebugBreak();
#endif
  // Ignore the return value.
  ASSERT(frame_->height() == original_height);
}


void CodeGenerator::InstantiateFunction(
    Handle<SharedFunctionInfo> function_info) {
  // Use the fast case closure allocation code that allocates in new
  // space for nested functions that don't need literals cloning.
  if (scope()->is_function_scope() && function_info->num_literals() == 0) {
    FastNewClosureStub stub;
    frame_->EmitPush(Operand(function_info));
    frame_->SpillAll();
    frame_->CallStub(&stub, 1);
    frame_->EmitPush(v0);
  } else {
    // Create a new closure.
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(function_info));
    frame_->CallRuntime(Runtime::kNewClosure, 2);
    frame_->EmitPush(v0);
  }
}


void CodeGenerator::VisitFunctionLiteral(FunctionLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ FunctionLiteral");

  // Build the function info and instantiate it.
  Handle<SharedFunctionInfo> function_info =
      Compiler::BuildFunctionInfo(node, script());
  if (function_info.is_null()) {
    SetStackOverflow();
    ASSERT(frame_->height() == original_height);
    return;
  }
  InstantiateFunction(function_info);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitSharedFunctionInfoLiteral(
    SharedFunctionInfoLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ SharedFunctionInfoLiteral");
  InstantiateFunction(node->shared_function_info());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitConditional(Conditional* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Conditional");
  JumpTarget then;
  JumpTarget else_;
  LoadCondition(node->condition(), &then, &else_, true);
  if (has_valid_frame()) {
    Branch(false, &else_);
  }
  if (has_valid_frame() || then.is_linked()) {
    then.Bind();
    Load(node->then_expression());
  }
  if (else_.is_linked()) {
    JumpTarget exit;
    if (has_valid_frame()) exit.Jump();
    else_.Bind();
    Load(node->else_expression());
    if (exit.is_linked()) exit.Bind();
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::LoadFromSlot(Slot* slot, TypeofState typeof_state) {
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    // JumpTargets do not yet support merging frames so the frame must be
    // spilled when jumping to these targets.
    JumpTarget slow;
    JumpTarget done;

    // Generate fast case for loading from slots that correspond to
    // local/global variables or arguments unless they are shadowed by
    // eval-introduced bindings.
    EmitDynamicLoadFromSlotFastCase(slot,
                                    typeof_state,
                                    &slow,
                                    &done);

    slow.Bind();
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(slot->var()->name()));

    if (typeof_state == INSIDE_TYPEOF) {
      frame_->CallRuntime(Runtime::kLoadContextSlotNoReferenceError, 2);
    } else {
      frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    }
    done.Bind();
    frame_->EmitPush(v0);

  } else {
    Register scratch0 = VirtualFrame::scratch0();
    Register scratch1 = VirtualFrame::scratch1();
    Register scratch2 = VirtualFrame::scratch2();
    TypeInfo info = type_info(slot);
    __ lw(v0, SlotOperand(slot, scratch2));
    frame_->EmitPush(v0, info);
    if (slot->var()->mode() == Variable::CONST) {
      // Const slots may contain 'the hole' value (the constant hasn't been
      // initialized yet) which needs to be converted into the 'undefined'
      // value.
      Comment cmnt(masm_, "[ Unhole const");
      Register tos = frame_->PopToRegister();
      __ LoadRoot(scratch0, Heap::kTheHoleValueRootIndex);
      __ subu(scratch1, tos, scratch0);
      __ LoadRoot(scratch2, Heap::kUndefinedValueRootIndex);
      // Conditional move if tos was the hole.
      __ movz(tos, scratch2, scratch1);
      frame_->EmitPush(tos);
    }
  }
}


void CodeGenerator::LoadFromSlotCheckForArguments(Slot* slot,
                                                  TypeofState state) {
  VirtualFrame::RegisterAllocationScope scope(this);
  LoadFromSlot(slot, state);

  // Bail out quickly if we're not using lazy arguments allocation.
  if (ArgumentsMode() != LAZY_ARGUMENTS_ALLOCATION) return;

  // ... or if the slot isn't a non-parameter arguments slot.
  if (slot->type() == Slot::PARAMETER || !slot->is_arguments()) return;

  // Load the loaded value from the stack into a register but leave it on the
  // stack.
  Register tos = frame_->Peek();

  // If the loaded value is the sentinel that indicates that we
  // haven't loaded the arguments object yet, we need to do it now.
  JumpTarget exit;
  __ LoadRoot(at, Heap::kTheHoleValueRootIndex);
  exit.Branch(ne, tos, Operand(at));
  frame_->Drop();
  StoreArgumentsObject(false);
  exit.Bind();
}


void CodeGenerator::StoreToSlot(Slot* slot, InitState init_state) {
  ASSERT(slot != NULL);
  VirtualFrame::RegisterAllocationScope scope(this);
  if (slot->type() == Slot::LOOKUP) {
    ASSERT(slot->var()->is_dynamic());

    // For now, just do a runtime call.
    frame_->EmitPush(cp);
    frame_->EmitPush(Operand(slot->var()->name()));

    if (init_state == CONST_INIT) {
      // Same as the case for a normal store, but ignores attribute
      // (e.g. READ_ONLY) of context slot so that we can initialize
      // const properties (introduced via eval("const foo = (some
      // expr);")). Also, uses the current function context instead of
      // the top context.
      //
      // Note that we must declare the foo upon entry of eval(), via a
      // context slot declaration, but we cannot initialize it at the
      // same time, because the const declaration may be at the end of
      // the eval code (sigh...) and the const variable may have been
      // used before (where its value is 'undefined'). Thus, we can only
      // do the initialization when we actually encounter the expression
      // and when the expression operands are defined and valid, and
      // thus we need the split into 2 operations: declaration of the
      // context slot followed by initialization.
      frame_->CallRuntime(Runtime::kInitializeConstContextSlot, 3);
    } else {
      frame_->CallRuntime(Runtime::kStoreContextSlot, 3);
    }
    // Storing a variable must keep the (new) value on the expression
    // stack. This is necessary for compiling assignment expressions.
    frame_->EmitPush(v0);

  } else {
    ASSERT(!slot->var()->is_dynamic());
    Register scratch = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();
    Register scratch3 = VirtualFrame::scratch2();

    // The frame must be spilled when branching to this target.
    JumpTarget exit;
    if (init_state == CONST_INIT) {
      ASSERT(slot->var()->mode() == Variable::CONST);
      // Only the first const initialization must be executed (the slot
      // still contains 'the hole' value). When the assignment is
      // executed, the code is identical to a normal store (see below).
      Comment cmnt(masm_, "[ Init const");
      __ lw(scratch, SlotOperand(slot, scratch));
      __ LoadRoot(scratch2, Heap::kTheHoleValueRootIndex);
      exit.Branch(ne, scratch, Operand(scratch2));
    }

    // We must execute the store. Storing a variable must keep the
    // (new) value on the stack. This is necessary for compiling
    // assignment expressions.
    //
    // Note: We will reach here even with slot->var()->mode() ==
    // Variable::CONST because of const declarations which will
    // initialize consts to 'the hole' value and by doing so, end up
    // calling this code. a2 may be loaded with context; used below in
    // RecordWrite.
    Register tos = frame_->Peek();
    __ sw(tos, SlotOperand(slot, scratch));
    if (slot->type() == Slot::CONTEXT) {
      // Skip write barrier if the written value is a smi.
      __ And(scratch2, tos, Operand(kSmiTagMask));
      // We don't use tos any more after here.
      exit.Branch(eq, scratch2, Operand(zero_reg));
      // scratch is loaded with context when calling SlotOperand above.
      int offset = FixedArray::kHeaderSize + slot->index() * kPointerSize;
      // Not sure if this is needed, but it makes sure that the TOS state
      // matches the ARM version.
      scratch3 = frame_->GetTOSRegister();
      __ RecordWrite(scratch, Operand(offset), scratch2, scratch3);
    }
    // If we definitely did not jump over the assignment, we do not need
    // to bind the exit label. Doing so can defeat peephole
    // optimization.
    if (init_state == CONST_INIT || slot->type() == Slot::CONTEXT) {
      exit.Bind();
    }
  }
}


void CodeGenerator::LoadFromGlobalSlotCheckExtensions(Slot* slot,
                                                      TypeofState typeof_state,
                                                      JumpTarget* slow) {
  // Check that no extension objects have been created by calls to
  // eval from the current scope to the global scope.
  Register tmp = frame_->scratch0();
  Register tmp2 = frame_->scratch1();
  Register context = cp;
  Scope* s = scope();
  while (s != NULL) {
    if (s->num_heap_slots() > 0) {
      if (s->calls_eval()) {
        frame_->SpillAll();
        // Check that extension is NULL.
        __ lw(tmp2, ContextOperand(context, Context::EXTENSION_INDEX));
        slow->Branch(ne, tmp2, Operand(zero_reg));
      }
      // Load next context in chain.
      __ lw(tmp, ContextOperand(context, Context::CLOSURE_INDEX));
      __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
      context = tmp;
    }
    // If no outer scope calls eval, we do not need to check more
    // context extensions.
    if (!s->outer_scope_calls_eval() || s->is_eval_scope()) break;
    s = s->outer_scope();
  }

  if (s->is_eval_scope()) {
    frame_->SpillAll();
    Label next, fast;
    __ mov(tmp, context);
    __ bind(&next);
    // Terminate at global context.
    __ lw(tmp2, FieldMemOperand(tmp, HeapObject::kMapOffset));
    __ LoadRoot(t8, Heap::kGlobalContextMapRootIndex);
    __ Branch(&fast, eq, tmp2, Operand(t8));
    // Check that extension is NULL.
    __ lw(tmp2, ContextOperand(tmp, Context::EXTENSION_INDEX));
    slow->Branch(ne, tmp2, Operand(zero_reg));
    // Load next context in chain.
    __ lw(tmp, ContextOperand(tmp, Context::CLOSURE_INDEX));
    __ lw(tmp, FieldMemOperand(tmp, JSFunction::kContextOffset));
    __ jmp(&next);
    __ bind(&fast);
  }

  // Load the global object.
  LoadGlobal();
  // Setup the name register and call load IC.
  frame_->CallLoadIC(slot->var()->name(),
                     typeof_state == INSIDE_TYPEOF
                         ? RelocInfo::CODE_TARGET
                         : RelocInfo::CODE_TARGET_CONTEXT);
}


void CodeGenerator::EmitDynamicLoadFromSlotFastCase(Slot* slot,
                                                    TypeofState typeof_state,
                                                    JumpTarget* slow,
                                                    JumpTarget* done) {
  // Generate fast-case code for variables that might be shadowed by
  // eval-introduced variables.  Eval is used a lot without
  // introducing variables.  In those cases, we do not want to
  // perform a runtime call for all variables in the scope
  // containing the eval.
  if (slot->var()->mode() == Variable::DYNAMIC_GLOBAL) {
    LoadFromGlobalSlotCheckExtensions(slot, typeof_state, slow);
    frame_->SpillAll();
    done->Jump();

  } else if (slot->var()->mode() == Variable::DYNAMIC_LOCAL) {
    frame_->SpillAll();
    Slot* potential_slot = slot->var()->local_if_not_shadowed()->AsSlot();
    Expression* rewrite = slot->var()->local_if_not_shadowed()->rewrite();
    if (potential_slot != NULL) {
      // Generate fast case for locals that rewrite to slots.
      __ lw(v0,
             ContextSlotOperandCheckExtensions(potential_slot,
                                               a1,
                                               a2,
                                               slow));
      if (potential_slot->var()->mode() == Variable::CONST) {
        __ LoadRoot(a1, Heap::kTheHoleValueRootIndex);
        __ subu(a1, v0, a1);  // Leave 0 in a1 on equal.
        __ LoadRoot(a0, Heap::kUndefinedValueRootIndex);
        __ movz(v0, a0, a1);  // Cond move Undef if v0 was 'the hole'.
      }
      done->Jump();
    } else if (rewrite != NULL) {
      // Generate fast case for argument loads.
      Property* property = rewrite->AsProperty();
      if (property != NULL) {
        VariableProxy* obj_proxy = property->obj()->AsVariableProxy();
        Literal* key_literal = property->key()->AsLiteral();
        if (obj_proxy != NULL &&
            key_literal != NULL &&
            obj_proxy->IsArguments() &&
            key_literal->handle()->IsSmi()) {
          // Load arguments object if there are no eval-introduced
          // variables. Then load the argument from the arguments
          // object using keyed load.
          __ lw(a0,
                 ContextSlotOperandCheckExtensions(obj_proxy->var()->AsSlot(),
                                                   a1,
                                                   a2,
                                                   slow));
          frame_->EmitPush(a0);
          __ li(a1, Operand(key_literal->handle()));
          frame_->EmitPush(a1);
          EmitKeyedLoad();
          done->Jump();
        }
      }
    }
  }
}


void CodeGenerator::VisitSlot(Slot* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Slot");
  LoadFromSlotCheckForArguments(node, NOT_INSIDE_TYPEOF);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitVariableProxy(VariableProxy* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ VariableProxy");

  Variable* var = node->var();
  Expression* expr = var->rewrite();
  if (expr != NULL) {
    Visit(expr);
  } else {
    ASSERT(var->is_global());
    Reference ref(this, node);
    ref.GetValue();
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitLiteral(Literal* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Literal");
  Register reg = frame_->GetTOSRegister();
  bool is_smi = node->handle()->IsSmi();
  __ li(reg, Operand(node->handle()));
  frame_->EmitPush(reg, is_smi ? TypeInfo::Smi() : TypeInfo::Unknown());
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitRegExpLiteral(RegExpLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ RexExp Literal");

  Register tmp = VirtualFrame::scratch0();
  // Free up a TOS register that can be used to push the literal.
  Register literal = frame_->GetTOSRegister();

  // Retrieve the literal array and check the allocated entry.

  // Load the function of this activation.
  __ lw(tmp, frame_->Function());

  // Load the literals array of the function.
  __ lw(tmp, FieldMemOperand(tmp, JSFunction::kLiteralsOffset));

  // Load the literal at the ast saved index.
  int literal_offset =
      FixedArray::kHeaderSize + node->literal_index() * kPointerSize;
  __ lw(literal, FieldMemOperand(tmp, literal_offset));

  JumpTarget materialized;
  __ LoadRoot(at, Heap::kUndefinedValueRootIndex);
  // This branch locks the virtual frame at the done label to match the
  // one we have here, where the literal register is not on the stack and
  // nothing is spilled.
  materialized.Branch(ne, literal, Operand(at));

  // If the entry is undefined we call the runtime system to compute
  // the literal.

  // literal array  (0)
  frame_->EmitPush(tmp);
  // literal index  (1)
  frame_->EmitPush(Operand(Smi::FromInt(node->literal_index())));
  // RegExp pattern (2)
  frame_->EmitPush(Operand(node->pattern()));
  // RegExp flags   (3)
  frame_->EmitPush(Operand(node->flags()));
  frame_->CallRuntime(Runtime::kMaterializeRegExpLiteral, 4);
  __ Move(literal, v0);

  materialized.Bind();

  frame_->EmitPush(literal);

  int size = JSRegExp::kSize + JSRegExp::kInObjectFieldCount * kPointerSize;
  frame_->EmitPush(Operand(Smi::FromInt(size)));
  frame_->CallRuntime(Runtime::kAllocateInNewSpace, 1);
  // From ARM:TODO(lrn): Use AllocateInNewSpace macro with fallback to runtime.
  // v0 is newly allocated space.

  // Reuse literal variable with (possibly) a new register, still holding
  // the materialized boilerplate.
  literal = frame_->PopToRegister();

  __ CopyFields(v0, literal, tmp.bit(), size / kPointerSize);

  // Push the clone.
  frame_->EmitPush(v0);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitObjectLiteral(ObjectLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ ObjectLiteral");

  Register literal = frame_->GetTOSRegister();
  // Load the function of this activation.
  __ lw(literal, frame_->Function());
  // Literal array.
  __ lw(literal, FieldMemOperand(literal, JSFunction::kLiteralsOffset));
  frame_->EmitPush(literal);
  // Literal index.
  frame_->EmitPush(Operand(Smi::FromInt(node->literal_index())));
  // Constant properties.
  frame_->EmitPush(Operand(node->constant_properties()));
  // Should the object literal have fast elements?
  frame_->EmitPush(Operand(Smi::FromInt(node->fast_elements() ? 1 : 0)));

  if (node->depth() > 1) {
    frame_->CallRuntime(Runtime::kCreateObjectLiteral, 4);
  } else {
    frame_->CallRuntime(Runtime::kCreateObjectLiteralShallow, 4);
  }
  frame_->EmitPush(v0);  // Save the result.

  for (int i = 0; i < node->properties()->length(); i++) {
    // At the start of each iteration, the top of stack contains
    // the newly created object literal.
    ObjectLiteral::Property* property = node->properties()->at(i);
    Literal* key = property->key();
    Expression* value = property->value();
    switch (property->kind()) {
      case ObjectLiteral::Property::CONSTANT:
        break;
      case ObjectLiteral::Property::MATERIALIZED_LITERAL:
        if (CompileTimeValue::IsCompileTimeValue(property->value())) break;
        // Else fall through
      case ObjectLiteral::Property::COMPUTED:
        if (key->handle()->IsSymbol()) {
          Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
          Load(value);
          frame_->PopToA0();
          // Fetch the object literal.
          frame_->SpillAllButCopyTOSToA1();
          __ li(a2, Operand(key->handle()));
          frame_->CallCodeObject(ic, RelocInfo::CODE_TARGET, 0);
          break;
        }
        // Else fall through.
      case ObjectLiteral::Property::PROTOTYPE: {
        frame_->Dup();
        Load(key);
        Load(value);
        frame_->CallRuntime(Runtime::kSetProperty, 3);
        break;
      }
      case ObjectLiteral::Property::SETTER: {
        frame_->Dup();
        Load(key);
        frame_->EmitPush(Operand(Smi::FromInt(1)));
        Load(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
      case ObjectLiteral::Property::GETTER: {
        frame_->Dup();
        Load(key);
        frame_->EmitPush(Operand(Smi::FromInt(0)));
        Load(value);
        frame_->CallRuntime(Runtime::kDefineAccessor, 4);
        break;
      }
    }
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitArrayLiteral(ArrayLiteral* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ ArrayLiteral");

  Register tos = frame_->GetTOSRegister();
  // Load the function of this activation.
  __ lw(tos, frame_->Function());
  // Load the literals array of the function.
  __ lw(tos, FieldMemOperand(tos, JSFunction::kLiteralsOffset));
  frame_->EmitPush(tos);
  frame_->EmitPush(Operand(Smi::FromInt(node->literal_index())));
  frame_->EmitPush(Operand(node->constant_elements()));
  int length = node->values()->length();
  if (node->constant_elements()->map() == Heap::fixed_cow_array_map()) {
    FastCloneShallowArrayStub stub(
        FastCloneShallowArrayStub::COPY_ON_WRITE_ELEMENTS, length);
    frame_->CallStub(&stub, 3);
    __ IncrementCounter(&Counters::cow_arrays_created_stub, 1, a1, a2);
  } else if (node->depth() > 1) {
    frame_->CallRuntime(Runtime::kCreateArrayLiteral, 3);
  } else if (length > FastCloneShallowArrayStub::kMaximumClonedLength) {
    frame_->CallRuntime(Runtime::kCreateArrayLiteralShallow, 3);
  } else {
    FastCloneShallowArrayStub stub(
        FastCloneShallowArrayStub::CLONE_ELEMENTS, length);
    frame_->CallStub(&stub, 3);
  }
  frame_->EmitPush(v0);  // Save the result.
  // v0: created object literal

  // Generate code to set the elements in the array that are not
  // literals.
  for (int i = 0; i < node->values()->length(); i++) {
    Expression* value = node->values()->at(i);

    // If value is a literal the property value is already set in the
    // boilerplate object.
    if (value->AsLiteral() != NULL) continue;
    // If value is a materialized literal the property value is already set
    // in the boilerplate object if it is simple.
    if (CompileTimeValue::IsCompileTimeValue(value)) continue;

    // The property must be set by generated code.
    Load(value);
    frame_->PopToA0();

    // Fetch the object literal.
    frame_->SpillAllButCopyTOSToA1();
    // Get the elements array.
    __ lw(a1, FieldMemOperand(a1, JSObject::kElementsOffset));

    // Write to the indexed properties array.
    int offset = i * kPointerSize + FixedArray::kHeaderSize;
    __ sw(a0, FieldMemOperand(a1, offset));

    // Update the write barrier for the array address.
    __ RecordWrite(a1, Operand(offset), a3, a2);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCatchExtensionObject(CatchExtensionObject* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  // Call runtime routine to allocate the catch extension object and
  // assign the exception value to the catch variable.
  Comment cmnt(masm_, "[ CatchExtensionObject");
  Load(node->key());
  Load(node->value());
  frame_->CallRuntime(Runtime::kCreateCatchExtensionObject, 2);
  frame_->EmitPush(v0);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitSlotAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm(), "[ Variable Assignment");
  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  ASSERT(var != NULL);
  Slot* slot = var->AsSlot();
  ASSERT(slot != NULL);

  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    LoadFromSlotCheckForArguments(slot, NOT_INSIDE_TYPEOF);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
  } else {
    Load(node->value());
  }

  // Perform the assignment.
  if (var->mode() != Variable::CONST || node->op() == Token::INIT_CONST) {
    CodeForSourcePosition(node->position());
    StoreToSlot(slot,
                node->op() == Token::INIT_CONST ? CONST_INIT : NOT_CONST_INIT);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitNamedPropertyAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm(), "[ Named Property Assignment");
  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  Property* prop = node->target()->AsProperty();
  ASSERT(var == NULL || (prop == NULL && var->is_global()));

  // Initialize name and evaluate the receiver sub-expression if necessary. If
  // the receiver is trivial it is not placed on the stack at this point, but
  // loaded whenever actually needed.
  Handle<String> name;
  bool is_trivial_receiver = false;
  if (var != NULL) {
    name = var->name();
  } else {
    Literal* lit = prop->key()->AsLiteral();
    ASSERT_NOT_NULL(lit);
    name = Handle<String>::cast(lit->handle());
    // Do not materialize the receiver on the frame if it is trivial.
    is_trivial_receiver = prop->obj()->IsTrivial();
    if (!is_trivial_receiver) Load(prop->obj());
  }

  // Change to slow case in the beginning of an initialization block to
  // avoid the quadratic behavior of repeatedly adding fast properties.
  if (node->starts_initialization_block()) {
    // Initialization block consists of assignments of the form expr.x = ..., so
    // this will never be an assignment to a variable, so there must be a
    // receiver object.
    ASSERT_EQ(NULL, var);
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else {
      frame_->Dup();
    }
    frame_->CallRuntime(Runtime::kToSlowProperties, 1);
  }

  // Change to fast case at the end of an initialization block. To prepare for
  // that add an extra copy of the receiver to the frame, so that it can be
  // converted back to fast case after the assignment.
  if (node->ends_initialization_block() && !is_trivial_receiver) {
    frame_->Dup();
  }

  // Stack layout:
  // [tos]   : receiver (only materialized if non-trivial)
  // [tos+1] : receiver if at the end of an initialization block

  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else if (var != NULL) {
      LoadGlobal();
    } else {
      frame_->Dup();
    }
    EmitNamedLoad(name, var != NULL);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
  } else {
    // For non-compound assignment just load the right-hand side.
    Load(node->value());
  }

  // Stack layout:
  // [tos]   : value
  // [tos+1] : receiver (only materialized if non-trivial)
  // [tos+2] : receiver if at the end of an initialization block

  // Perform the assignment.  It is safe to ignore constants here.
  ASSERT(var == NULL || var->mode() != Variable::CONST);
  ASSERT_NE(Token::INIT_CONST, node->op());
  if (is_trivial_receiver) {
    // Load the receiver and swap with the value.
    Load(prop->obj());
    Register reg0 = frame_->PopToRegister();
    Register reg1 = frame_->PopToRegister(reg0);
    frame_->EmitPush(reg0);
    frame_->EmitPush(reg1);
  }
  CodeForSourcePosition(node->position());
  bool is_contextual = (var != NULL);
  EmitNamedStore(name, is_contextual);
  frame_->EmitPush(v0);

  // Change to fast case at the end of an initialization block.
  if (node->ends_initialization_block()) {
    ASSERT_EQ(NULL, var);
    // The argument to the runtime call is the receiver.
    if (is_trivial_receiver) {
      Load(prop->obj());
    } else {
      // A copy of the receiver is below the value of the assignment. Swap
      // the receiver and the value of the assignment expression.
      Register reg0 = frame_->PopToRegister();
      Register reg1 = frame_->PopToRegister(reg0);
      frame_->EmitPush(reg0);
      frame_->EmitPush(reg1);
    }
    frame_->CallRuntime(Runtime::kToFastProperties, 1);
  }

  // Stack layout:
  // [tos]   : result

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::EmitKeyedPropertyAssignment(Assignment* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Keyed Property Assignment");
  Property* prop = node->target()->AsProperty();
  ASSERT_NOT_NULL(prop);

  // Evaluate the receiver subexpression.
  Load(prop->obj());

  WriteBarrierCharacter wb_info;

  // Change to slow case in the beginning of an initialization block to
  // avoid the quadratic behavior of repeatedly adding fast properties.
  if (node->starts_initialization_block()) {
    frame_->Dup();
    frame_->CallRuntime(Runtime::kToSlowProperties, 1);
  }

  // Change to fast case at the end of an initialization block. To prepare for
  // that add an extra copy of the receiver to the frame, so that it can be
  // converted back to fast case after the assignment.
  if (node->ends_initialization_block()) {
    frame_->Dup();
  }

  // Evaluate the key subexpression.
  Load(prop->key());

  // Stack layout:
  // [tos]   : key
  // [tos+1] : receiver
  // [tos+2] : receiver if at the end of an initialization block
  //
  // Evaluate the right-hand side.
  if (node->is_compound()) {
    // For a compound assignment the right-hand side is a binary operation
    // between the current property value and the actual right-hand side.
    // Duplicate receiver and key for loading the current property value.
    frame_->Dup2();
    EmitKeyedLoad();
    frame_->EmitPush(v0);

    // Perform the binary operation.
    Literal* literal = node->value()->AsLiteral();
    bool overwrite_value =
        (node->value()->AsBinaryOperation() != NULL &&
         node->value()->AsBinaryOperation()->ResultOverwriteAllowed());
    if (literal != NULL && literal->handle()->IsSmi()) {
      SmiOperation(node->binary_op(),
                   literal->handle(),
                   false,
                   overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (literal != NULL) {
        ASSERT(!literal->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      Load(node->value());
      GenericBinaryOperation(node->binary_op(),
                             overwrite_value ? OVERWRITE_RIGHT : NO_OVERWRITE,
                             inline_smi);
    }
    wb_info = node->type()->IsLikelySmi() ? LIKELY_SMI : UNLIKELY_SMI;
  } else {
    // For non-compound assignment just load the right-hand side.
    Load(node->value());
    wb_info = node->value()->AsLiteral() != NULL ?
        NEVER_NEWSPACE :
        (node->value()->type()->IsLikelySmi() ? LIKELY_SMI : UNLIKELY_SMI);
  }

  // Stack layout:
  // [tos]   : value
  // [tos+1] : key
  // [tos+2] : receiver
  // [tos+3] : receiver if at the end of an initialization block

  // Perform the assignment.  It is safe to ignore constants here.
  ASSERT(node->op() != Token::INIT_CONST);
  CodeForSourcePosition(node->position());
  EmitKeyedStore(prop->key()->type(), wb_info);
  frame_->EmitPush(v0);


  // Stack layout:
  // [tos]   : result
  // [tos+1] : receiver if at the end of an initialization block

  // Change to fast case at the end of an initialization block.
  if (node->ends_initialization_block()) {
    // The argument to the runtime call is the extra copy of the receiver,
    // which is below the value of the assignment. Swap the receiver and
    // the value of the assignment expression.
    Register reg0 = frame_->PopToRegister();
    Register reg1 = frame_->PopToRegister(reg0);
    frame_->EmitPush(reg1);
    frame_->EmitPush(reg0);
    frame_->CallRuntime(Runtime::kToFastProperties, 1);
  }

  // Stack layout:
  // [tos]   : result

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitAssignment(Assignment* node) {
  VirtualFrame::RegisterAllocationScope scope(this);
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Assignment");

  Variable* var = node->target()->AsVariableProxy()->AsVariable();
  Property* prop = node->target()->AsProperty();

  if (var != NULL && !var->is_global()) {
    EmitSlotAssignment(node);

  } else if ((prop != NULL && prop->key()->IsPropertyName()) ||
             (var != NULL && var->is_global())) {
    // Properties whose keys are property names and global variables are
    // treated as named property references.  We do not need to consider
    // global 'this' because it is not a valid left-hand side.
    EmitNamedPropertyAssignment(node);

  } else if (prop != NULL) {
    // Other properties (including rewritten parameters for a function that
    // uses arguments) are keyed property assignments.
    EmitKeyedPropertyAssignment(node);

  } else {
    // Invalid left-hand side.
    Load(node->target());
    frame_->CallRuntime(Runtime::kThrowReferenceError, 1);
    // The runtime call doesn't actually return but the code generator will
    // still generate code and expects a certain frame height.
    frame_->EmitPush(v0);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitThrow(Throw* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Throw");

  Load(node->exception());
  CodeForSourcePosition(node->position());
  frame_->CallRuntime(Runtime::kThrow, 1);
  frame_->EmitPush(v0);
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitProperty(Property* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Property");

  { Reference property(this, node);
    property.GetValue();
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCall(Call* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ Call");

  Expression* function = node->expression();
  ZoneList<Expression*>* args = node->arguments();

  // Standard function call.
  // Check if the function is a variable or a property.
  Variable* var = function->AsVariableProxy()->AsVariable();
  Property* property = function->AsProperty();

  // ------------------------------------------------------------------------
  // Fast-case: Use inline caching.
  // ---
  // According to ECMA-262, section 11.2.3, page 44, the function to call
  // must be resolved after the arguments have been evaluated. The IC code
  // automatically handles this by loading the arguments before the function
  // is resolved in cache misses (this also holds for megamorphic calls).
  // ------------------------------------------------------------------------

  if (var != NULL && var->is_possibly_eval()) {
    // ----------------------------------
    // JavaScript example: 'eval(arg)'  // eval is not known to be shadowed.
    // ----------------------------------

    // In a call to eval, we first call %ResolvePossiblyDirectEval to
    // resolve the function we need to call and the receiver of the
    // call.  Then we call the resolved function using the given
    // arguments.

    // Prepare stack for call to resolved function.
    Load(function);

    // Allocate a frame slot for the receiver.
    frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      Load(args->at(i));
    }

    VirtualFrame::SpilledScope spilled_scope(frame_);

    // If we know that eval can only be shadowed by eval-introduced
    // variables we attempt to load the global eval function directly
    // in generated code. If we succeed, there is no need to perform a
    // context lookup in the runtime system.
    JumpTarget done;
    if (var->AsSlot() != NULL && var->mode() == Variable::DYNAMIC_GLOBAL) {
      ASSERT(var->AsSlot()->type() == Slot::LOOKUP);
      JumpTarget slow;
      // Prepare the stack for the call to
      // ResolvePossiblyDirectEvalNoLookup by pushing the loaded
      // function, the first argument to the eval call and the
      // receiver.
      LoadFromGlobalSlotCheckExtensions(var->AsSlot(),
                                        NOT_INSIDE_TYPEOF,
                                        &slow);
      frame_->EmitPush(v0);
      if (arg_count > 0) {
        __ lw(a1, MemOperand(sp, arg_count * kPointerSize));
        frame_->EmitPush(a1);
      } else {
        frame_->EmitPush(a2);
      }
      __ lw(a1, frame_->Receiver());
      frame_->EmitPush(a1);

      frame_->CallRuntime(Runtime::kResolvePossiblyDirectEvalNoLookup, 3);

      done.Jump();
      slow.Bind();
    }

    // Prepare the stack for the call to ResolvePossiblyDirectEval by
    // pushing the loaded function, the first argument to the eval
    // call and the receiver.

    __ lw(a1, MemOperand(sp, arg_count * kPointerSize + kPointerSize));
    frame_->EmitPush(a1);
    if (arg_count > 0) {
      __ lw(a1, MemOperand(sp, arg_count * kPointerSize));
      frame_->EmitPush(a1);
    } else {
      frame_->EmitPush(a2);
    }
    __ lw(a1, frame_->Receiver());
    frame_->EmitPush(a1);

    // Resolve the call.
    frame_->CallRuntime(Runtime::kResolvePossiblyDirectEval, 3);

    // If we generated fast-case code bind the jump-target where fast
    // and slow case merge.
    if (done.is_linked()) done.Bind();

    // Touch up stack with the right values for the function and the receiver.
    // Runtime::kResolvePossiblyDirectEval returns object pair in v0/v1.
    __ sw(v0, MemOperand(sp, (arg_count + 1) * kPointerSize));
    __ sw(v1, MemOperand(sp, arg_count * kPointerSize));

    // Call the function.
    CodeForSourcePosition(node->position());

    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    CallFunctionStub call_function(arg_count, in_loop, RECEIVER_MIGHT_BE_VALUE);
    frame_->CallStub(&call_function, arg_count + 1);

    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->Drop();
    frame_->EmitPush(v0);

  } else if (var != NULL && !var->is_this() && var->is_global()) {
    // -----------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is global.
    // -----------------------------------------------------
    // Pass the global object as the receiver and let the IC stub
    // patch the stack to use the global proxy as 'this' in the
    // invoked function.
    LoadGlobal();

    // Load the arguments.
    int arg_count = args->length();
    for (int i = 0; i < arg_count; i++) {
      Load(args->at(i));
    }

    VirtualFrame::SpilledScope spilled_scope(frame_);
    // Setup the receiver register and call the IC initialization code.
    __ li(a2, Operand(var->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    CodeForSourcePosition(node->position());
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET_CONTEXT,
                           arg_count + 1);
    __ lw(cp, frame_->Context());
    // Remove the function from the stack.
    frame_->EmitPush(v0);

  } else if (var != NULL && var->AsSlot() != NULL &&
             var->AsSlot()->type() == Slot::LOOKUP) {
    // ----------------------------------
    // JavaScript examples:
    //
    //  with (obj) foo(1, 2, 3)  // foo may be in obj.
    //
    //  function f() {};
    //  function g() {
    //    eval(...);
    //    f();  // f could be in extension object.
    //  }
    // ----------------------------------

    // JumpTargets do not yet support merging frames so the frame must be
    // spilled when jumping to these targets.
    JumpTarget slow, done;

    // Generate fast case for loading functions from slots that
    // correspond to local/global variables or arguments unless they
    // are shadowed by eval-introduced bindings.
    EmitDynamicLoadFromSlotFastCase(var->AsSlot(),
                                    NOT_INSIDE_TYPEOF,
                                    &slow,
                                    &done);

    slow.Bind();
    // Load the function
    frame_->EmitPush(cp);
    __ li(a0, Operand(var->name()));
    frame_->EmitPush(a0);
    frame_->CallRuntime(Runtime::kLoadContextSlot, 2);
    // v0: slot value; v1: receiver

    // Load the receiver.
    // Push the function and receiver on the stack.
    frame_->EmitMultiPushReversed(v0.bit() | v1.bit());

    // If fast case code has been generated, emit code to push the
    // function and receiver and have the slow path jump around this
    // code.
    if (done.is_linked()) {
      JumpTarget call;
      call.Jump();
      done.Bind();
      frame_->EmitPush(v0);  // function
      LoadGlobalReceiver(VirtualFrame::scratch0());  // receiver
      call.Bind();
    }

    // Call the function. At this point, everything is spilled but the
    // function and receiver are in v0 and v1.
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);

  } else if (property != NULL) {
    // Check if the key is a literal string.
    Literal* literal = property->key()->AsLiteral();

    if (literal != NULL && literal->handle()->IsSymbol()) {
      // ------------------------------------------------------------------
      // JavaScript example: 'object.foo(1, 2, 3)' or 'map["key"](1, 2, 3)'
      // ------------------------------------------------------------------

      Handle<String> name = Handle<String>::cast(literal->handle());

      if (ArgumentsMode() == LAZY_ARGUMENTS_ALLOCATION &&
          name->IsEqualTo(CStrVector("apply")) &&
          args->length() == 2 &&
          args->at(1)->AsVariableProxy() != NULL &&
          args->at(1)->AsVariableProxy()->IsArguments()) {
        // Use the optimized Function.prototype.apply that avoids
        // allocating lazily allocated arguments objects.
        CallApplyLazy(property->obj(),
                      args->at(0),
                      args->at(1)->AsVariableProxy(),
                      node->position());

      } else {
        Load(property->obj());  // Receiver.
        // Load the arguments.
        int arg_count = args->length();
        for (int i = 0; i < arg_count; i++) {
          Load(args->at(i));
        }

        VirtualFrame::SpilledScope spilled_scope(frame_);
        // Set the name register and call the IC initialization code.
        __ li(a2, Operand(name));
        InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
        Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
        CodeForSourcePosition(node->position());
        frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
        __ lw(cp, frame_->Context());
        frame_->EmitPush(v0);
      }

    } else {
      // -------------------------------------------
      // JavaScript example: 'array[index](1, 2, 3)'
      // -------------------------------------------

      Load(property->obj());
      if (property->is_synthetic()) {
        Load(property->key());
        EmitKeyedLoad();
        // Put the function below the receiver.
        // Use the global receiver.
        frame_->EmitPush(v0);  // Function.
        LoadGlobalReceiver(VirtualFrame::scratch0());
        // Call the function.
        CallWithArguments(args, RECEIVER_MIGHT_BE_VALUE, node->position());
        frame_->EmitPush(v0);
      } else {
        // Load the arguments.
        int arg_count = args->length();
        for (int i = 0; i < arg_count; i++) {
          Load(args->at(i));
        }

        // Set the name register and call the IC initialization code.
        Load(property->key());
        frame_->SpillAll();
        frame_->EmitPop(a2);  // Function name.

        InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
        Handle<Code> stub = ComputeKeyedCallInitialize(arg_count, in_loop);
        CodeForSourcePosition(node->position());
        frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
        __ lw(cp, frame_->Context());
        frame_->EmitPush(v0);
      }
    }

  } else {
    // --------------------------------------------------------
    // JavaScript example: 'foo(1, 2, 3)'  // foo is not global
    // --------------------------------------------------------

    // Load the function.
    Load(function);


    // Pass the global proxy as the receiver.
    LoadGlobalReceiver(VirtualFrame::scratch0());

    // Call the function (and allocate args slots).
    CallWithArguments(args, NO_CALL_FUNCTION_FLAGS, node->position());
    frame_->EmitPush(v0);
  }

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCallNew(CallNew* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CallNew");

  // According to ECMA-262, section 11.2.2, page 44, the function
  // expression in new calls must be evaluated before the
  // arguments. This is different from ordinary calls, where the
  // actual function to call is resolved after the arguments have been
  // evaluated.

  // Push constructor on the stack.  If it's not a function it's used as
  // receiver for CALL_NON_FUNCTION, otherwise the value on the stack is
  // ignored.
  Load(node->expression());

  ZoneList<Expression*>* args = node->arguments();
  int arg_count = args->length();
  // Push the arguments ("left-to-right") on the stack.
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  // Spill everything from here to simplify the implementation.
  VirtualFrame::SpilledScope spilled_scope(frame_);

  // Load the argument count into a0 and the function into a1 as per
  // calling convention.
  __ li(a0, Operand(arg_count));
  __ lw(a1, frame_->ElementAt(arg_count));

  // Call the construct call builtin that handles allocation and
  // constructor invocation.
  CodeForSourcePosition(node->position());
  Handle<Code> ic(Builtins::builtin(Builtins::JSConstructCall));
  frame_->CallCodeObject(ic, RelocInfo::CONSTRUCT_CALL, arg_count + 1);
  frame_->EmitPush(v0);

  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::GenerateClassOf(ZoneList<Expression*>* args) {
  JumpTarget leave, null, function, non_function_constructor;
  Register scratch = VirtualFrame::scratch0();

  // Load the object into register.
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register tos = frame_->PopToRegister();

  // If the object is a smi, we return null.
  __ And(t0, tos, Operand(kSmiTagMask));
  null.Branch(eq, t0, Operand(zero_reg), no_hint);

  // Check that the object is a JS object but take special care of JS
  // functions to make sure they have 'Function' as their class.
  __ GetObjectType(tos, tos, scratch);
  null.Branch(less, scratch, Operand(FIRST_JS_OBJECT_TYPE), no_hint);

  // As long as JS_FUNCTION_TYPE is the last instance type and it is
  // right after LAST_JS_OBJECT_TYPE, we can avoid checking for
  // LAST_JS_OBJECT_TYPE.
  STATIC_ASSERT(LAST_TYPE == JS_FUNCTION_TYPE);
  STATIC_ASSERT(JS_FUNCTION_TYPE == LAST_JS_OBJECT_TYPE + 1);
  function.Branch(eq, scratch, Operand(JS_FUNCTION_TYPE), no_hint);

  // Check if the constructor in the map is a function.
  __ lw(tos, FieldMemOperand(tos, Map::kConstructorOffset));
  __ GetObjectType(tos, scratch, scratch);
  non_function_constructor.Branch(ne, scratch, Operand(JS_FUNCTION_TYPE));

  // The tos register now contains the constructor function. Grab the
  // instance class name from there.
  __ lw(tos, FieldMemOperand(tos, JSFunction::kSharedFunctionInfoOffset));
  __ lw(tos,
        FieldMemOperand(tos, SharedFunctionInfo::kInstanceClassNameOffset));
  frame_->EmitPush(tos);
  leave.Jump();

  // Functions have class 'Function'.
  function.Bind();
  __ li(tos, Operand(Factory::function_class_symbol()));
  frame_->EmitPush(tos);
  leave.Jump();

  // Objects with a non-function constructor have class 'Object'.
  non_function_constructor.Bind();
  __ li(tos, Operand(Factory::Object_symbol()));
  frame_->EmitPush(tos);
  leave.Jump();

  // Non-JS objects have class null.
  null.Bind();
  __ LoadRoot(tos, Heap::kNullValueRootIndex);
  frame_->EmitPush(tos);

  // All done.
  leave.Bind();
}


void CodeGenerator::GenerateValueOf(ZoneList<Expression*>* args) {
  Register scratch = VirtualFrame::scratch0();
  JumpTarget leave;

  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register tos = frame_->PopToRegister();  // tos contains object.
  // if (object->IsSmi()) return the object.
  __ And(t0, tos, Operand(kSmiTagMask));
  leave.Branch(eq, t0, Operand(zero_reg));
  // It is a heap object - get map. If (!object->IsJSValue()) return the object.
  __ GetObjectType(tos, scratch, scratch);
  leave.Branch(ne, scratch, Operand(JS_VALUE_TYPE));
  // Load the value.
  __ lw(tos, FieldMemOperand(tos, JSValue::kValueOffset));
  leave.Bind();
  frame_->EmitPush(tos);
}


void CodeGenerator::GenerateSetValueOf(ZoneList<Expression*>* args) {
  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  JumpTarget leave;

  ASSERT(args->length() == 2);
  Load(args->at(0));  // Load the object.
  Load(args->at(1));  // Load the value.
  Register value = frame_->PopToRegister();
  Register object = frame_->PopToRegister(value);
  // if (object->IsSmi()) return value.
  __ And(scratch1, object, Operand(kSmiTagMask));
  leave.Branch(eq, scratch1, Operand(zero_reg), no_hint);
  // It is a heap object - get map. If (!object->IsJSValue()) return the value.
  __ GetObjectType(object, scratch1, scratch1);
  leave.Branch(ne, scratch1, Operand(JS_VALUE_TYPE), no_hint);
  // Store the value in object, and return value.
  __ sw(value, FieldMemOperand(object, JSValue::kValueOffset));
  // Update the write barrier.
  __ RecordWrite(object,
                 Operand(JSValue::kValueOffset - kHeapObjectTag),
                 scratch1,
                 scratch2);
  // Leave.
  leave.Bind();
  frame_->EmitPush(value);
}


void CodeGenerator::GenerateIsSmi(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register reg = frame_->PopToRegister();
  __ And(condReg1, reg, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateLog(ZoneList<Expression*>* args) {
  // See comment in CodeGenerator::GenerateLog in codegen-ia32.cc.
  ASSERT_EQ(args->length(), 3);
#ifdef ENABLE_LOGGING_AND_PROFILING
  if (ShouldGenerateLog(args->at(0))) {
    Load(args->at(1));
    Load(args->at(2));
    frame_->CallRuntime(Runtime::kLog, 2);
  }
#endif
  frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
}


void CodeGenerator::GenerateIsNonNegativeSmi(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register reg = frame_->PopToRegister();
  __ And(condReg1, reg, Operand(kSmiTagMask | 0x80000000u));
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateMathPow(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 2);
  Load(args->at(0));
  Load(args->at(1));


  // There is a performance bug with the new code.
  // Just force the call to runtime until this is debugged.
  // if (!CpuFeatures::IsSupported(FPU)) {
  if (1) {  // Fix this.........................................................
    frame_->CallRuntime(Runtime::kMath_pow, 2);
    frame_->EmitPush(v0);
  } else {
    CpuFeatures::Scope scope(FPU);
    JumpTarget runtime, done;
    Label exponent_nonsmi, base_nonsmi, powi, not_minus_half, allocate_return;

    Register scratch1 = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();

    // Get base and exponent to registers.
    Register exponent = frame_->PopToRegister();
    Register base = frame_->PopToRegister(exponent);
    Register heap_number_map = no_reg;

    // Set the frame for the runtime jump target. The code below jumps to the
    // jump target label so the frame needs to be established before that.
    ASSERT(runtime.entry_frame() == NULL);
    runtime.set_entry_frame(frame_);

    __ BranchOnNotSmi(exponent, &exponent_nonsmi);
    __ BranchOnNotSmi(base, &base_nonsmi);

    heap_number_map = t2;
    __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);

    // Exponent is a smi and base is a smi. Get the smi value into fpu register
    // f2.
    __ SmiToDoubleFPURegister(base, f2, scratch1);
    __ Branch(&powi);

    __ bind(&base_nonsmi);
    // Exponent is smi and base is non smi. Get the double value from the base
    // into fpu register f2.
    __ ObjectToDoubleFPURegister(base, f2,
                                 scratch1, scratch2, heap_number_map,
                                 runtime.entry_label());

    __ bind(&powi);

    // Load 1.0 into f0.
    __ li(scratch2, Operand(0x3ff00000));
    __ mtc1(scratch2, f1);
    __ mtc1(zero_reg, f0);

    // Get the absolute untagged value of the exponent and use that for the
    // calculation.
    __ sra(scratch1, exponent, kSmiTagSize);
    {
      Label exponent_not_negative;
      __ Branch(&exponent_not_negative, gt,
                scratch1, Operand(zero_reg));
      __ Subu(scratch1, zero_reg, scratch1);  // Negate if negative.
      __ mov_d(f4, f0);  // 1.0 needed in f4 later if exponent is negative.
      __ bind(&exponent_not_negative);
    }

    // Run through all the bits in the exponent. The result is calculated in f0
    // and f2 holds base^(bit^2).
    Label more_bits;
    __ bind(&more_bits);
    {
      Label will_not_carry;
      __ andi(scratch2, scratch1, 1);  // Test LSB.
      __ Branch(&will_not_carry, eq, scratch2, Operand(zero_reg));
      __ mul_d(f0, f0, f2);  // Multiply with base^(bit^2).
      __ bind(&will_not_carry);
    }
    __ srl(scratch1, scratch1, 1);
    {
      Label zero;
      __ Branch(&zero, eq, scratch1, Operand(zero_reg));
      __ mul_d(f2, f2, f2);
      __ Branch(&more_bits);
      __ bind(&zero);
    }

    // If exponent is positive we are done.
    __ Branch(&allocate_return, ge, exponent, Operand(zero_reg));

    // If exponent is negative result is 1/result (f4 already holds 1.0 in that
    // case). However if f0 has reached infinity this will not provide the
    // correct result, so call runtime if that is the case.
    {
      // Testing for Infinity. This is much simpler than comparing FPU values.
      Label no_match;
      __ mfc1(scratch2, f0);
      __ Branch(&no_match, ne, scratch2, Operand(zero_reg));
      __ mfc1(scratch2, f1);
      runtime.Branch(eq, scratch2, Operand(0x7FF00000));  // f0 == Inf.
      __ bind(&no_match);
    }
    __ div_d(f0, f4, f0);
    __ Branch(&allocate_return);

    __ bind(&exponent_nonsmi);

    // Special handling of raising to the power of -0.5 and 0.5. First check
    // that the value is a heap number and that the lower bits (which for both
    // values are zero).
    heap_number_map = t2;
    __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);
    __ lw(scratch1, FieldMemOperand(exponent, HeapObject::kMapOffset));
    __ lw(scratch2, FieldMemOperand(exponent, HeapNumber::kMantissaOffset));
    runtime.Branch(ne, scratch1, Operand(heap_number_map));
    __ And(at, scratch1, scratch2);
    runtime.Branch(ne, at, Operand(zero_reg));

    // Load the higher bits (which contains the floating point exponent).
    __ lw(scratch1, FieldMemOperand(exponent, HeapNumber::kExponentOffset));

    // Compare exponent with -0.5.
    __ Branch(&not_minus_half, ne, scratch1, Operand(0xbfe00000));

    // Get the double value from the base into fpu register f0.
    __ ObjectToDoubleFPURegister(base, f0,
                                 scratch1, scratch2, heap_number_map,
                                 runtime.entry_label(),
                                 AVOID_NANS_AND_INFINITIES);

    // Load 1.0 into f2.
    __ li(scratch2, 0x3ff00000);
    __ mtc1(scratch2, f2);
    __ mtc1(zero_reg, f3);

    // Calculate the reciprocal of the square root. 1/sqrt(x) = sqrt(1/x).
    __ div_d(f0, f2, f0);
    __ sqrt_d(f0, f0);

    __ Branch(&allocate_return);

    __ bind(&not_minus_half);
    // Compare exponent with 0.5.
    runtime.Branch(ne, scratch1, Operand(0x3fe00000));

      // Get the double value from the base into fpu register f0.
    __ ObjectToDoubleFPURegister(base, f0,
                                 scratch1, scratch2, heap_number_map,
                                 runtime.entry_label(),
                                 AVOID_NANS_AND_INFINITIES);
    __ sqrt_d(f0, f0);

    __ bind(&allocate_return);
    Register scratch3 = t3;
    __ AllocateHeapNumberWithValue(scratch3, f0, scratch1, scratch2,
                                   runtime.entry_label());
    __ Move(base, scratch3);
    done.Jump();

    runtime.Bind();

    // Push back the arguments again for the runtime call.
    frame_->EmitPush(base);
    frame_->EmitPush(exponent);
    frame_->CallRuntime(Runtime::kMath_pow, 2);
    __ Move(base, v0);

    done.Bind();
    frame_->EmitPush(base);
  }
}


// Generates the Math.sqrt method.
void CodeGenerator::GenerateMathSqrt(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));

  if (!CpuFeatures::IsSupported(FPU)) {
    frame_->CallRuntime(Runtime::kMath_sqrt, 1);
    frame_->EmitPush(v0);
  } else {
    CpuFeatures::Scope scope(FPU);
    JumpTarget runtime, done;

    Register scratch1 = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();

    // Get the value from the frame.
    Register tos = frame_->PopToRegister();

    // Set the frame for the runtime jump target. The code below jumps to the
    // jump target label so the frame needs to be established before that.
    ASSERT(runtime.entry_frame() == NULL);
    runtime.set_entry_frame(frame_);

    Register heap_number_map = t2;
    __ LoadRoot(heap_number_map, Heap::kHeapNumberMapRootIndex);

    // Get the double value from the heap number into fpu register f0.
    __ ObjectToDoubleFPURegister(tos, f0,
                                 scratch1, scratch2, heap_number_map,
                                 runtime.entry_label());

    // Calculate the square root of f0 and place result in a heap number object.
    __ sqrt_d(f0, f0);
    __ AllocateHeapNumberWithValue(
        tos, f0, scratch1, scratch2, runtime.entry_label());
    done.Jump();

    runtime.Bind();
    // Push back the argument again for the runtime call.
    frame_->EmitPush(tos);
    frame_->CallRuntime(Runtime::kMath_sqrt, 1);
    __ Move(tos, v0);

    done.Bind();
    frame_->EmitPush(tos);
  }
}


class DeferredStringCharCodeAt : public DeferredCode {
 public:
  DeferredStringCharCodeAt(Register object,
                           Register index,
                           Register scratch,
                           Register result)
      : result_(result),
        char_code_at_generator_(object,
                                index,
                                scratch,
                                result,
                                &need_conversion_,
                                &need_conversion_,
                                &index_out_of_range_,
                                STRING_INDEX_IS_NUMBER) {}

  StringCharCodeAtGenerator* fast_case_generator() {
    return &char_code_at_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_code_at_generator_.GenerateSlow(masm(), call_helper);

    __ bind(&need_conversion_);
    // Move the undefined value into the result register, which will
    // trigger conversion.
    __ LoadRoot(result_, Heap::kUndefinedValueRootIndex);
    __ Branch(exit_label());

    __ bind(&index_out_of_range_);
    // When the index is out of range, the spec requires us to return
    // NaN.
    __ LoadRoot(result_, Heap::kNanValueRootIndex);
    __ Branch(exit_label());
  }

 private:
  Register result_;

  Label need_conversion_;
  Label index_out_of_range_;

  StringCharCodeAtGenerator char_code_at_generator_;
};


// This generates code that performs a String.prototype.charCodeAt() call
// or returns a smi in order to trigger conversion.
void CodeGenerator::GenerateStringCharCodeAt(ZoneList<Expression*>* args) {
  Comment(masm_, "[ GenerateStringCharCodeAt");
  ASSERT(args->length() == 2);

  Load(args->at(0));
  Load(args->at(1));

  Register index = frame_->PopToRegister();
  Register object = frame_->PopToRegister(index);

  // We need two extra registers.
  Register scratch = VirtualFrame::scratch0();
  Register result =  VirtualFrame::scratch1();

  DeferredStringCharCodeAt* deferred =
      new DeferredStringCharCodeAt(object,
                                   index,
                                   scratch,
                                   result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}


class DeferredStringCharFromCode : public DeferredCode {
 public:
  DeferredStringCharFromCode(Register code,
                             Register result)
      : char_from_code_generator_(code, result) {}

  StringCharFromCodeGenerator* fast_case_generator() {
    return &char_from_code_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_from_code_generator_.GenerateSlow(masm(), call_helper);
  }

 private:
  StringCharFromCodeGenerator char_from_code_generator_;
};


// Generates code for creating a one-char string from a char code.
void CodeGenerator::GenerateStringCharFromCode(ZoneList<Expression*>* args) {
  Comment(masm_, "[ GenerateStringCharFromCode");
  ASSERT(args->length() == 1);

  Load(args->at(0));

  Register code = frame_->PopToRegister();
  Register result = v0;

  DeferredStringCharFromCode* deferred = new DeferredStringCharFromCode(
      code, result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}


class DeferredStringCharAt : public DeferredCode {
 public:
  DeferredStringCharAt(Register object,
                       Register index,
                       Register scratch1,
                       Register scratch2,
                       Register result)
      : result_(result),
        char_at_generator_(object,
                           index,
                           scratch1,
                           scratch2,
                           result,
                           &need_conversion_,
                           &need_conversion_,
                           &index_out_of_range_,
                           STRING_INDEX_IS_NUMBER) {}

  StringCharAtGenerator* fast_case_generator() {
    return &char_at_generator_;
  }

  virtual void Generate() {
    VirtualFrameRuntimeCallHelper call_helper(frame_state());
    char_at_generator_.GenerateSlow(masm(), call_helper);

    __ bind(&need_conversion_);
    // Move smi zero into the result register, which will trigger
    // conversion.
    __ li(result_, Operand(Smi::FromInt(0)));
    __ Branch(exit_label());

    __ bind(&index_out_of_range_);
    // When the index is out of range, the spec requires us to return
    // the empty string.
    __ LoadRoot(result_, Heap::kEmptyStringRootIndex);
    __ Branch(exit_label());
  }

 private:
  Register result_;

  Label need_conversion_;
  Label index_out_of_range_;

  StringCharAtGenerator char_at_generator_;
};


// This generates code that performs a String.prototype.charAt() call
// or returns a smi in order to trigger conversion.
void CodeGenerator::GenerateStringCharAt(ZoneList<Expression*>* args) {
  Comment(masm_, "[ GenerateStringCharAt");
  ASSERT(args->length() == 2);

  Load(args->at(0));
  Load(args->at(1));

  Register index = frame_->PopToRegister();
  Register object = frame_->PopToRegister(index);

  // We need three extra registers.
  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  Register result = v0;

  DeferredStringCharAt* deferred =
      new DeferredStringCharAt(object,
                               index,
                               scratch1,
                               scratch2,
                               result);
  deferred->fast_case_generator()->GenerateFast(masm_);
  deferred->BindExit();
  frame_->EmitPush(result);
}


void CodeGenerator::GenerateIsArray(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  JumpTarget answer;

  // We need the condition to be not_equal if the object is a smi.
  Register possible_array = frame_->PopToRegister();
  Register scratch = VirtualFrame::scratch0();
  __ And(scratch, possible_array, Operand(kSmiTagMask));
  __ Xor(condReg1, scratch, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, scratch, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a JS array.
  __ GetObjectType(possible_array, scratch, condReg1);
  __ li(condReg2, Operand(JS_ARRAY_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsRegExp(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  JumpTarget answer;
  // We need the condition to be not_equal is the object is a smi.
  Register possible_regexp = frame_->PopToRegister();
  Register scratch = VirtualFrame::scratch0();
  __ And(scratch, possible_regexp, Operand(kSmiTagMask));
  __ Xor(condReg1, scratch, Operand(kSmiTagMask));
  __ mov(condReg2, zero_reg);
  answer.Branch(eq, scratch, Operand(zero_reg));
  // It is a heap object - get the map. Check if the object is a regexp.
  __ GetObjectType(possible_regexp, scratch, condReg1);
  __ li(condReg2, Operand(JS_REGEXP_TYPE));
  answer.Bind();
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsObject(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (typeof(arg) === 'object' || %_ClassOf(arg) == 'RegExp')
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_object = frame_->PopToRegister();
  __ And(t1, possible_object, Operand(kSmiTagMask));
  false_target()->Branch(eq, t1, Operand(zero_reg));

  __ LoadRoot(t0, Heap::kNullValueRootIndex);
  true_target()->Branch(eq, possible_object, Operand(t0));

  // scratch0 == t4, so it's safe to use t1 below.
  Register map_reg = VirtualFrame::scratch0();
  __ lw(map_reg, FieldMemOperand(possible_object, HeapObject::kMapOffset));
  // Undetectable objects behave like undefined when tested with typeof.
  __ lbu(possible_object, FieldMemOperand(map_reg, Map::kBitFieldOffset));
  __ And(t1, possible_object, Operand(1 << Map::kIsUndetectable));
  false_target()->Branch(ne, t1, Operand(zero_reg));

  __ lbu(possible_object, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
  false_target()->Branch(less, possible_object, Operand(FIRST_JS_OBJECT_TYPE));
  __ mov(condReg1, possible_object);
  __ li(condReg2, Operand(LAST_JS_OBJECT_TYPE));
  cc_reg_ = less_equal;
}


void CodeGenerator::GenerateIsSpecObject(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (typeof(arg) === 'object' || %_ClassOf(arg) == 'RegExp' ||
  // typeof(arg) == function).
  // It includes undetectable objects (as opposed to IsObject).
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register value = frame_->PopToRegister();
  __ And(at, value, Operand(kSmiTagMask));
  false_target()->Branch(eq, at, Operand(zero_reg));
  // Check that this is an object.
  __ lw(value, FieldMemOperand(value, HeapObject::kMapOffset));
  __ lbu(condReg1, FieldMemOperand(value, Map::kInstanceTypeOffset));
  __ li(condReg2, Operand(FIRST_JS_OBJECT_TYPE));
  cc_reg_ = ge;
}


// Deferred code to check whether the String JavaScript object is safe for using
// default value of. This code is called after the bit caching this information
// in the map has been checked with the map for the object in the map_result_
// register. On return the register map_result_ contains 1 for true and 0 for
// false.
class DeferredIsStringWrapperSafeForDefaultValueOf : public DeferredCode {
 public:
  DeferredIsStringWrapperSafeForDefaultValueOf(Register object,
                                               Register map_result,
                                               Register scratch1,
                                               Register scratch2)
      : object_(object),
        map_result_(map_result),
        scratch1_(scratch1),
        scratch2_(scratch2) { }

  virtual void Generate() {
    Label false_result;

    // Check that map is loaded as expected.
    if (FLAG_debug_code) {
      __ lw(scratch1_, FieldMemOperand(object_, HeapObject::kMapOffset));
      __ Assert(eq, "Map not in expected register",
                map_result_, Operand(scratch1_));
    }

    // Check for fast case object. Generate false result for slow case object.
    __ lw(scratch1_, FieldMemOperand(object_, JSObject::kPropertiesOffset));
    __ lw(scratch1_, FieldMemOperand(scratch1_, HeapObject::kMapOffset));
    __ LoadRoot(scratch2_, Heap::kHashTableMapRootIndex);
    __ Branch(&false_result, eq, scratch1_, Operand(scratch2_));

    // Look for valueOf symbol in the descriptor array, and indicate false if
    // found. The type is not checked, so if it is a transition it is a false
    // negative.
    __ lw(map_result_,
           FieldMemOperand(map_result_, Map::kInstanceDescriptorsOffset));
    __ lw(scratch2_, FieldMemOperand(map_result_, FixedArray::kLengthOffset));
    // map_result_: descriptor array
    // scratch2_: length of descriptor array
    // Calculate the end of the descriptor array.
    STATIC_ASSERT(kSmiTag == 0);
    STATIC_ASSERT(kSmiTagSize == 1);
    STATIC_ASSERT(kPointerSize == 4);
    __ Addu(scratch1_,
            map_result_,
            Operand(FixedArray::kHeaderSize - kHeapObjectTag));
    __ sll(scratch2_, scratch2_, kPointerSizeLog2 - kSmiTagSize);
    __ Addu(scratch1_, scratch1_, scratch2_);

    // Calculate location of the first key name.
    __ Addu(map_result_,
            map_result_,
            Operand(FixedArray::kHeaderSize - kHeapObjectTag +
                    DescriptorArray::kFirstIndex * kPointerSize));
    // Loop through all the keys in the descriptor array. If one of these is the
    // symbol valueOf the result is false.
    Label entry, loop;
    // The use of t0 to store the valueOf symbol asumes that it is not otherwise
    // used in the loop below.
    __ li(t0, Factory::value_of_symbol());
    __ Branch(&entry);
    __ bind(&loop);
    __ lw(scratch2_, MemOperand(map_result_, 0));
    __ Branch(&false_result, eq, scratch2_, Operand(t0));
    __ Addu(map_result_, map_result_, Operand(kPointerSize));
    __ bind(&entry);
    __ Branch(&loop, ne, map_result_, Operand(scratch1_));

    // Reload map as register map_result_ was used as temporary above.
    __ lw(map_result_, FieldMemOperand(object_, HeapObject::kMapOffset));

    // If a valueOf property is not found on the object check that it's
    // prototype is the un-modified String prototype. If not result is false.
    __ lw(scratch1_, FieldMemOperand(map_result_, Map::kPrototypeOffset));
    __ BranchOnSmi(scratch1_, &false_result);
    __ lw(scratch1_, FieldMemOperand(scratch1_, HeapObject::kMapOffset));
    __ lw(scratch2_,
          CodeGenerator::ContextOperand(cp, Context::GLOBAL_INDEX));
    __ lw(scratch2_,
          FieldMemOperand(scratch2_, GlobalObject::kGlobalContextOffset));
    __ lw(scratch2_,
          CodeGenerator::ContextOperand(
              scratch2_, Context::STRING_FUNCTION_PROTOTYPE_MAP_INDEX));
    __ Branch(&false_result, ne, scratch1_, Operand(scratch2_));

    // Set the bit in the map to indicate that it has been checked safe for
    // default valueOf and set true result.
    __ lw(scratch1_, FieldMemOperand(map_result_, Map::kBitField2Offset));
    __ Or(scratch1_,
          scratch1_,
          Operand(1 << Map::kStringWrapperSafeForDefaultValueOf));
    __ sw(scratch1_, FieldMemOperand(map_result_, Map::kBitField2Offset));
    __ li(map_result_, 1);
    __ Branch(exit_label());
    __ bind(&false_result);
    // Set false result.
    __ li(map_result_, 0);
  }

 private:
  Register object_;
  Register map_result_;
  Register scratch1_;
  Register scratch2_;
};


void CodeGenerator::GenerateIsStringWrapperSafeForDefaultValueOf(
    ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register obj = frame_->PopToRegister();  // Pop the string wrapper.
  if (FLAG_debug_code) {
    __ AbortIfSmi(obj);
  }

  // Check whether this map has already been checked to be safe for default
  // valueOf.
  Register map_result = VirtualFrame::scratch0();

  // We need an additional two scratch registers for the deferred code.
  Register scratch1 = VirtualFrame::scratch1();
  Register scratch2 = VirtualFrame::scratch2();

  __ lw(map_result, FieldMemOperand(obj, HeapObject::kMapOffset));
  __ lbu(scratch2, FieldMemOperand(map_result, Map::kBitField2Offset));
  __ And(scratch2, scratch2,
         Operand(1 << Map::kStringWrapperSafeForDefaultValueOf));
  true_target()->Branch(ne, scratch2, Operand(zero_reg));

  DeferredIsStringWrapperSafeForDefaultValueOf* deferred =
      new DeferredIsStringWrapperSafeForDefaultValueOf(
          obj, map_result, scratch1, scratch2);
  deferred->Branch(eq, scratch2, Operand(zero_reg));
  deferred->BindExit();
  __ mov(condReg1, map_result);
  __ mov(condReg2, zero_reg);
  cc_reg_ = ne;
}


void CodeGenerator::GenerateIsFunction(ZoneList<Expression*>* args) {
  // This generates a fast version of:
  // (%_ClassOf(arg) === 'Function')
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_function = frame_->PopToRegister();
  __ And(t0, possible_function, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  Register map_reg = VirtualFrame::scratch0();
  __ GetObjectType(possible_function, map_reg, condReg1);
  __ li(condReg2, Operand(JS_FUNCTION_TYPE));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsUndetectableObject(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register possible_undetectable = frame_->PopToRegister();
  __ And(t0, possible_undetectable, Operand(kSmiTagMask));
  false_target()->Branch(eq, t0, Operand(zero_reg));
  Register scratch = VirtualFrame::scratch0();
  __ lw(scratch,
        FieldMemOperand(possible_undetectable, HeapObject::kMapOffset));
  __ lbu(scratch, FieldMemOperand(scratch, Map::kBitFieldOffset));
  __ And(condReg1, scratch, Operand(1 << Map::kIsUndetectable));
  __ mov(condReg2, zero_reg);
  cc_reg_ = ne;
}


void CodeGenerator::GenerateIsConstructCall(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 0);

  Register scratch0 = VirtualFrame::scratch0();
  Register scratch1 = VirtualFrame::scratch1();
  // Get the frame pointer for the calling frame.
  __ lw(scratch0, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));

  // Skip the arguments adaptor frame if it exists.
  Label check_frame_marker;
  __ lw(scratch1,
        MemOperand(scratch0, StandardFrameConstants::kContextOffset));
  __ Branch(&check_frame_marker, ne,
            scratch1, Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));
  __ lw(scratch0,
        MemOperand(scratch0, StandardFrameConstants::kCallerFPOffset));

  // Check the marker in the calling frame.
  __ bind(&check_frame_marker);
  __ lw(condReg1, MemOperand(scratch0, StandardFrameConstants::kMarkerOffset));
  __ li(condReg2, Operand(Smi::FromInt(StackFrame::CONSTRUCT)));
  cc_reg_ = eq;
}


void CodeGenerator::GenerateArgumentsLength(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 0);

  Label exit;
  Register tos = frame_->GetTOSRegister();
  Register scratch0 = VirtualFrame::scratch0();
  Register scratch1 = VirtualFrame::scratch1();

  // Get the number of formal parameters.
  __ li(tos, Operand(Smi::FromInt(scope()->num_parameters())));

  __ lw(scratch0, MemOperand(fp, StandardFrameConstants::kCallerFPOffset));
  __ lw(scratch1, MemOperand(scratch0, StandardFrameConstants::kContextOffset));
  __ Branch(&exit,
            ne,
            scratch1,
            Operand(Smi::FromInt(StackFrame::ARGUMENTS_ADAPTOR)));

  // Arguments adaptor case: Read the arguments length from the
  // adaptor frame and return it.
  __ lw(tos, MemOperand(scratch0,
                        ArgumentsAdaptorFrameConstants::kLengthOffset));

  __ bind(&exit);
  frame_->EmitPush(tos);
}


void CodeGenerator::GenerateArguments(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);

  // Satisfy contract with ArgumentsAccessStub:
  // Load the key into a1 and the formal parameters count into a0.
  Load(args->at(0));
  frame_->PopToA1();
  frame_->SpillAll();
  __ li(a0, Operand(Smi::FromInt(scope()->num_parameters())));

  // Call the shared stub to get to arguments[key].
  ArgumentsAccessStub stub(ArgumentsAccessStub::READ_ELEMENT);
  frame_->CallStub(&stub, 0);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRandomHeapNumber(
    ZoneList<Expression*>* args) {
  VirtualFrame::SpilledScope spilled_scope(frame_);
  ASSERT(args->length() == 0);

  Label slow_allocate_heapnumber;
  Label heapnumber_allocated;

  // Save the new heap number in callee-saved register s0, since
  // we call out to external C code below.
  __ LoadRoot(t6, Heap::kHeapNumberMapRootIndex);
  __ AllocateHeapNumber(s0, a1, a2, t6, &slow_allocate_heapnumber);
  __ jmp(&heapnumber_allocated);

  __ bind(&slow_allocate_heapnumber);

  // Allocate a heap number.
  __ CallRuntime(Runtime::kNumberAlloc, 0);
  __ mov(s0, v0);   // Save result in s0, so it is saved thru CFunc call.

  __ bind(&heapnumber_allocated);

  // Convert 32 random bits in r0 to 0.(32 random bits) in a double
  // by computing:
  // ( 1.(20 0s)(32 random bits) x 2^20 ) - (1.0 x 2^20)).
  if (CpuFeatures::IsSupported(FPU)) {
    __ PrepareCallCFunction(0, a1);
    __ CallCFunction(ExternalReference::random_uint32_function(), 0);

    CpuFeatures::Scope scope(FPU);
    // 0x41300000 is the top half of 1.0 x 2^20 as a double.
    __ li(a1, Operand(0x41300000));
    // Move 0x41300000xxxxxxxx (x = random bits in v0) to FPU.
    __ mtc1(a1, f13);
    __ mtc1(v0, f12);
    // Move 0x4130000000000000 to FPU.
    __ mtc1(a1, f15);
    __ mtc1(zero_reg, f14);
    // Subtract and store the result in the heap number.
    __ sub_d(f0, f12, f14);
    __ sdc1(f0, MemOperand(s0, HeapNumber::kValueOffset - kHeapObjectTag));
    frame_->EmitPush(s0);
  } else {
    __ mov(a0, s0);
    __ PrepareCallCFunction(1, a1);
    __ CallCFunction(
        ExternalReference::fill_heap_number_with_random_function(), 1);
    frame_->EmitPush(v0);
  }
}


void CodeGenerator::GenerateStringAdd(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateStringAdd");
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringAddStub stub(NO_STRING_ADD_FLAGS);
  frame_->SpillAll();
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateSubString(ZoneList<Expression*>* args) {
  ASSERT_EQ(3, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));

  SubStringStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 3);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateStringCompare(ZoneList<Expression*>* args) {
  ASSERT_EQ(2, args->length());

  Load(args->at(0));
  Load(args->at(1));

  StringCompareStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 2);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpExec(ZoneList<Expression*>* args) {
  ASSERT_EQ(4, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));
  Load(args->at(3));
  RegExpExecStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 4);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpConstructResult(ZoneList<Expression*>* args) {
  // No stub. This code only occurs a few times in regexp.js.
  const int kMaxInlineLength = 100;

  ASSERT_EQ(3, args->length());
  Load(args->at(0));  // Size of array, smi.
  Load(args->at(1));  // "index" property value.
  Load(args->at(2));  // "input" property value.
  {
    VirtualFrame::SpilledScope spilled_scope(frame_);
    Label slowcase;
    Label done;
    __ lw(a1, MemOperand(sp, kPointerSize * 2));
    STATIC_ASSERT(kSmiTag == 0);
    STATIC_ASSERT(kSmiTagSize == 1);
    __ BranchOnNotSmi(a1, &slowcase);
    __ Branch(&slowcase, hi, a1, Operand(Smi::FromInt(kMaxInlineLength)));
    // Smi-tagging is equivalent to multiplying by 2.
    // Allocate RegExpResult followed by FixedArray with size in ebx.
    // JSArray:   [Map][empty properties][Elements][Length-smi][index][input]
    // Elements:  [Map][Length][..elements..]
    // Size of JSArray with two in-object properties and the header of a
    // FixedArray.
    int objects_size =
        (JSRegExpResult::kSize + FixedArray::kHeaderSize) / kPointerSize;
    __ srl(t1, a1, kSmiTagSize + kSmiShiftSize);
    __ Addu(a2, t1, Operand(objects_size));
    __ AllocateInNewSpace(
        a2,  // In: Size, in words.
        v0,  // Out: Start of allocation (tagged).
        a3,  // Scratch register.
        t0,  // Scratch register.
        &slowcase,
        static_cast<AllocationFlags>(TAG_OBJECT | SIZE_IN_WORDS));
    // v0: Start of allocated area, object-tagged.
    // a1: Number of elements in array, as smi.
    // t1: Number of elements, untagged.

    // Set JSArray map to global.regexp_result_map().
    // Set empty properties FixedArray.
    // Set elements to point to FixedArray allocated right after the JSArray.
    // Interleave operations for better latency.
    __ lw(a2, ContextOperand(cp, Context::GLOBAL_INDEX));
    __ Addu(a3, v0, Operand(JSRegExpResult::kSize));
    __ li(t0, Operand(Factory::empty_fixed_array()));
    __ lw(a2, FieldMemOperand(a2, GlobalObject::kGlobalContextOffset));
    __ sw(a3, FieldMemOperand(v0, JSObject::kElementsOffset));
    __ lw(a2, ContextOperand(a2, Context::REGEXP_RESULT_MAP_INDEX));
    __ sw(t0, FieldMemOperand(v0, JSObject::kPropertiesOffset));
    __ sw(a2, FieldMemOperand(v0, HeapObject::kMapOffset));

    // Set input, index and length fields from arguments.
    __ MultiPop(static_cast<RegList>(a2.bit() | t0.bit()));
    __ sw(a1, FieldMemOperand(v0, JSArray::kLengthOffset));
    __ Addu(sp, sp, Operand(kPointerSize));
    __ sw(t0, FieldMemOperand(v0, JSRegExpResult::kIndexOffset));
    __ sw(a2, FieldMemOperand(v0, JSRegExpResult::kInputOffset));

    // Fill out the elements FixedArray.
    // v0: JSArray, tagged.
    // a3: FixedArray, tagged.
    // t1: Number of elements in array, untagged.

    // Set map.
    __ li(a2, Operand(Factory::fixed_array_map()));
    __ sw(a2, FieldMemOperand(a3, HeapObject::kMapOffset));
    // Set FixedArray length.
    __ sll(t2, t1, kSmiTagSize);
    __ sw(t2, FieldMemOperand(a3, FixedArray::kLengthOffset));
    // Fill contents of fixed-array with the-hole.
    __ li(a2, Operand(Factory::the_hole_value()));
    __ Addu(a3, a3, Operand(FixedArray::kHeaderSize - kHeapObjectTag));
    // Fill fixed array elements with hole.
    // v0: JSArray, tagged.
    // a2: the hole.
    // a3: Start of elements in FixedArray.
    // t1: Number of elements to fill.
    Label loop;
    __ sll(t1, t1, kPointerSizeLog2);  // Concert num elements to num bytes.
    __ addu(t1, t1, a3);  // Point past last element to store.
    __ bind(&loop);
    __ Branch(&done, ge, a3, Operand(t1));  // Break when a3 past end of elem.
    __ sw(a2, MemOperand(a3));
    __ Branch(&loop, false);         // Use branch delay slot.
    __ addiu(a3, a3, kPointerSize);  // In branch delay slot.

    __ bind(&slowcase);
    __ CallRuntime(Runtime::kRegExpConstructResult, 3);
    __ bind(&done);
  }
  frame_->Forget(3);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateRegExpCloneResult(ZoneList<Expression*>* args) {
  ASSERT_EQ(1, args->length());

  Load(args->at(0));
  frame_->PopToA0();
  {
    VirtualFrame::SpilledScope spilled_scope(frame_);

    Label done;
    Label call_runtime;
    __ BranchOnSmi(a0, &done);

    // Load JSRegExp map into a1. Check that argument object has this map.
    // Arguments to this function should be results of calling RegExp exec,
    // which is either an unmodified JSRegExpResult or null. Anything not having
    // the unmodified JSRegExpResult map is returned unmodified.
    // This also ensures that elements are fast.

    __ lw(a1, ContextOperand(cp, Context::GLOBAL_INDEX));
    __ lw(a1, FieldMemOperand(a1, GlobalObject::kGlobalContextOffset));
    __ lw(a1, ContextOperand(a1, Context::REGEXP_RESULT_MAP_INDEX));
    __ lw(t1, FieldMemOperand(a0, HeapObject::kMapOffset));
    __ Branch(&done, ne, a1, Operand(t1));

    if (FLAG_debug_code) {
      __ LoadRoot(a2, Heap::kEmptyFixedArrayRootIndex);
      __ lw(at, FieldMemOperand(a0, JSObject::kPropertiesOffset));
      __ Check(eq,
               "JSRegExpResult: default map but non-empty properties.",
               at,
               Operand(a2));
    }

    // All set, copy the contents to a new object.
    __ AllocateInNewSpace(JSRegExpResult::kSize,
                          a2,
                          a3,
                          t0,
                          &call_runtime,
                          NO_ALLOCATION_FLAGS);
    // Store RegExpResult map as map of allocated object.
    ASSERT(JSRegExpResult::kSize == 6 * kPointerSize);
    // Copy all fields (map is already in a1) from (untagged) a0 to a2.
    // Change map of elements array (ends up in a3) to be a FixedCOWArray.
    __ And(a0, a0, ~kHeapObjectTagMask);

    __ sw(a1, MemOperand(a2, 0 * kPointerSize));

    __ lw(t0, MemOperand(a0, 1 * kPointerSize));
    __ sw(t0, MemOperand(a2, 1 * kPointerSize));

    __ lw(a3, MemOperand(a0, 2 * kPointerSize));
    __ sw(a3, MemOperand(a2, 2 * kPointerSize));

    __ lw(t0, MemOperand(a0, 3 * kPointerSize));
    __ sw(t0, MemOperand(a2, 3 * kPointerSize));
    __ lw(t0, MemOperand(a0, 4 * kPointerSize));
    __ sw(t0, MemOperand(a2, 4 * kPointerSize));
    __ lw(t0, MemOperand(a0, 5 * kPointerSize));
    __ sw(t0, MemOperand(a2, 5 * kPointerSize));

    ASSERT(JSRegExp::kElementsOffset == 2 * kPointerSize);
    // Check whether elements array is empty fixed array, and otherwise make
    // it copy-on-write (it never should be empty unless someone is messing
    // with the arguments to the runtime function).
    __ LoadRoot(t1, Heap::kEmptyFixedArrayRootIndex);
    __ Addu(a0, a2, kHeapObjectTag);  // Tag result and move it to a0.
    __ Branch(&done, eq, a3, Operand(t1));
    __ LoadRoot(t1, Heap::kFixedCOWArrayMapRootIndex);
    __ sw(t1, FieldMemOperand(a3, HeapObject::kMapOffset));
    __ Branch(&done);
    __ bind(&call_runtime);
    __ push(a0);
    __ CallRuntime(Runtime::kRegExpCloneResult, 1);
    __ bind(&done);
  }
  frame_->EmitPush(a0);
}


class DeferredSearchCache: public DeferredCode {
 public:
  DeferredSearchCache(Register dst, Register cache, Register key)
      : dst_(dst), cache_(cache), key_(key) {
    set_comment("[ DeferredSearchCache");
  }

  virtual void Generate();

 private:
  Register dst_, cache_, key_;
};


void DeferredSearchCache::Generate() {
  __ Push(cache_, key_);
  __ CallRuntime(Runtime::kGetFromCache, 2);
  if (!dst_.is(v0)) {
    __ mov(dst_, v0);
  }
}


void CodeGenerator::GenerateGetFromCache(ZoneList<Expression*>* args) {
  ASSERT_EQ(2, args->length());

  ASSERT_NE(NULL, args->at(0)->AsLiteral());
  int cache_id = Smi::cast(*(args->at(0)->AsLiteral()->handle()))->value();

  Handle<FixedArray> jsfunction_result_caches(
      Top::global_context()->jsfunction_result_caches());
  if (jsfunction_result_caches->length() <= cache_id) {
    __ Abort("Attempt to use undefined cache.");
    frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
    return;
  }

  Load(args->at(1));

  frame_->PopToA1();
  frame_->SpillAll();
  Register key = a1;  // Just poped to r1
  Register result = v0;  // Free, as frame has just been spilled.
  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();

  __ lw(scratch1, ContextOperand(cp, Context::GLOBAL_INDEX));
  __ lw(scratch1,
        FieldMemOperand(scratch1, GlobalObject::kGlobalContextOffset));
  __ lw(scratch1,
        ContextOperand(scratch1, Context::JSFUNCTION_RESULT_CACHES_INDEX));
  __ lw(scratch1,
        FieldMemOperand(scratch1, FixedArray::OffsetOfElementAt(cache_id)));

  DeferredSearchCache* deferred =
      new DeferredSearchCache(result, scratch1, key);

  const int kFingerOffset =
      FixedArray::OffsetOfElementAt(JSFunctionResultCache::kFingerIndex);
  STATIC_ASSERT(kSmiTag == 0 && kSmiTagSize == 1);
  __ lw(result, FieldMemOperand(scratch1, kFingerOffset));
  // result now holds finger offset as a smi.
  __ Addu(scratch2, scratch1,
          Operand(FixedArray::kHeaderSize - kHeapObjectTag));
  // scratch2 now points to the start of fixed array elements.
  __ sll(at, result, kPointerSizeLog2 - kSmiTagSize);  // Smi to byte index.
  __ addu(scratch2, scratch2, at);  // a3 now points to the key of the pair.
  __ lw(result, MemOperand(scratch2, 0));
  deferred->Branch(ne, key, Operand(result));

  __ lw(result, MemOperand(scratch2, kPointerSize));

  deferred->BindExit();
  frame_->EmitPush(result);
}


void CodeGenerator::GenerateNumberToString(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);

  // Load the argument on the stack and jump to the runtime.
  Load(args->at(0));

  NumberToStringStub stub;
  frame_->SpillAll();
  frame_->CallStub(&stub, 1);
  frame_->EmitPush(v0);
}

class DeferredSwapElements: public DeferredCode {
 public:
  DeferredSwapElements(Register object, Register index1, Register index2)
      : object_(object), index1_(index1), index2_(index2) {
    set_comment("[ DeferredSwapElements");
  }

  virtual void Generate();

 private:
  Register object_, index1_, index2_;
};


void DeferredSwapElements::Generate() {
  __ Push(object_);
  __ Push(index1_);
  __ Push(index2_);
  __ CallRuntime(Runtime::kSwapElements, 3);
}


void CodeGenerator::GenerateSwapElements(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateSwapElements");

  ASSERT_EQ(3, args->length());

  Load(args->at(0));
  Load(args->at(1));
  Load(args->at(2));

  VirtualFrame::SpilledScope spilled_scope(frame_);

  Register index2 = a1;
  Register index1 = a0;
  Register object = v0;
  Register tmp1 = a2;
  Register tmp2 = a3;

  frame_->EmitPop(index2);
  frame_->EmitPop(index1);
  frame_->EmitPop(object);

  DeferredSwapElements* deferred =
      new DeferredSwapElements(object, index1, index2);

  // Fetch the map and check if array is in fast case.
  // Check that object doesn't require security checks and
  // has no indexed interceptor.
  __ GetObjectType(object, tmp1, tmp2);
  deferred->Branch(lt, tmp2, Operand(FIRST_JS_OBJECT_TYPE));

  __ lbu(tmp2, FieldMemOperand(tmp1, Map::kBitFieldOffset));
  __ And(tmp2, tmp2, Operand(KeyedLoadIC::kSlowCaseBitFieldMask));
  deferred->Branch(ne, tmp2, Operand(zero_reg));

  // Check the object's elements are in fast case and writable.
  __ lw(tmp1, FieldMemOperand(object, JSObject::kElementsOffset));
  __ lw(tmp2, FieldMemOperand(tmp1, HeapObject::kMapOffset));
  __ LoadRoot(t8, Heap::kFixedArrayMapRootIndex);
  deferred->Branch(ne, tmp2, Operand(t8));

  // Smi-tagging is equivalent to multiplying by 2.
  STATIC_ASSERT(kSmiTag == 0);
  STATIC_ASSERT(kSmiTagSize == 1);

  // Check that both indices are smis.
  __ mov(tmp2, index1);
  __ Or(tmp2, tmp2, index2);
  __ And(tmp2, tmp2, Operand(kSmiTagMask));
  deferred->Branch(ne, tmp2, Operand(zero_reg));

  // Bring the offsets into the fixed array in tmp1 into index1 and
  // index2.
  __ li(tmp2, Operand(FixedArray::kHeaderSize - kHeapObjectTag));

  __ sll(t8, index1, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(index1, tmp2, t8);

  __ sll(t8, index2, kPointerSizeLog2 - kSmiTagSize);
  __ Addu(index2, tmp2, t8);

  // Swap elements.
  Register tmp3 = object;
  object = no_reg;

  __ Addu(t8, tmp1, index1);
  __ Addu(t9, tmp1, index2);

  __ lw(tmp3, MemOperand(t8, 0));
  __ lw(tmp2, MemOperand(t9, 0));
  __ sw(tmp3, MemOperand(t9, 0));
  __ sw(tmp2, MemOperand(t8, 0));

  Label done;
  __ InNewSpace(tmp1, tmp2, eq, &done);
  // Possible optimization: do a check that both values are Smis
  // (or them and test against Smi mask.)

  __ mov(tmp2, tmp1);
  RecordWriteStub recordWrite1(tmp1, index1, tmp3);
  __ CallStub(&recordWrite1);

  RecordWriteStub recordWrite2(tmp2, index2, tmp3);
  __ CallStub(&recordWrite2);

  __ bind(&done);

  deferred->BindExit();
  __ LoadRoot(tmp1, Heap::kUndefinedValueRootIndex);
  frame_->EmitPush(tmp1);
}


void CodeGenerator::GenerateCallFunction(ZoneList<Expression*>* args) {
  Comment cmnt(masm_, "[ GenerateCallFunction");

  ASSERT(args->length() >= 2);

  int n_args = args->length() - 2;  // for receiver and function.
  Load(args->at(0));  // receiver
  for (int i = 0; i < n_args; i++) {
    Load(args->at(i + 1));
  }
  Load(args->at(n_args + 1));  // function
  frame_->CallJSFunction(n_args);
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateMathSin(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);
  Load(args->at(0));
  if (CpuFeatures::IsSupported(FPU)) {
    TranscendentalCacheStub stub(TranscendentalCache::SIN);
    frame_->SpillAllButCopyTOSToA0();
    frame_->CallStub(&stub, 1);
  } else {
    frame_->CallRuntime(Runtime::kMath_sin, 1);
  }
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateMathCos(ZoneList<Expression*>* args) {
  ASSERT_EQ(args->length(), 1);
  Load(args->at(0));
  if (CpuFeatures::IsSupported(FPU)) {
    TranscendentalCacheStub stub(TranscendentalCache::COS);
    frame_->SpillAllButCopyTOSToA0();
    frame_->CallStub(&stub, 1);
  } else {
    frame_->CallRuntime(Runtime::kMath_cos, 1);
  }
  frame_->EmitPush(v0);
}


void CodeGenerator::GenerateObjectEquals(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  Load(args->at(0));
  Load(args->at(1));
  Register lhs = frame_->PopToRegister();
  Register rhs = frame_->PopToRegister(lhs);
  __ mov(condReg1, lhs);
  __ mov(condReg2, rhs);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateIsRegExpEquivalent(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 2);

  // Load the two objects into registers and perform the comparison.
  Load(args->at(0));
  Load(args->at(1));

  // Since MIPS has no condition codes, this code is optimized by using the
  // condition registers.
  Register right = frame_->PopToRegister();
  Register left = frame_->PopToRegister(right);
  Register tmp = frame_->scratch0();
  Register tmp2 = frame_->scratch1();

  // Jumps to done must have the eq flag set if the test is successful
  // and clear if the test has failed.
  Label done;

  // Fail if either is a non-HeapObject.
  __ Move(condReg1, left);
  __ Move(condReg2, right);
  __ Branch(&done, eq);
  __ And(tmp, left, right);
  __ Xor(tmp, tmp, kSmiTagMask);
  __ And(condReg1, tmp, kSmiTagMask);
  __ Move(condReg2, zero_reg);
  __ Branch(&done, ne);
  __ lw(tmp, FieldMemOperand(left, HeapObject::kMapOffset));
  __ lbu(condReg1, FieldMemOperand(tmp, Map::kInstanceTypeOffset));
  __ li(condReg2, JS_REGEXP_TYPE);
  __ Branch(&done, ne);
  __ lw(condReg2, FieldMemOperand(right, HeapObject::kMapOffset));
  __ Move(condReg1, tmp);
  __ Branch(&done, ne);
  __ lw(condReg1, FieldMemOperand(left, JSRegExp::kDataOffset));
  __ lw(condReg2, FieldMemOperand(right, JSRegExp::kDataOffset));
  __ bind(&done);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateHasCachedArrayIndex(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register value = frame_->PopToRegister();
  Register tmp = frame_->scratch0();
  __ lw(tmp, FieldMemOperand(value, String::kHashFieldOffset));
  __ And(condReg1, tmp, String::kContainsCachedArrayIndexMask);
  __ mov(condReg2, zero_reg);
  cc_reg_ = eq;
}


void CodeGenerator::GenerateGetCachedArrayIndex(ZoneList<Expression*>* args) {
  ASSERT(args->length() == 1);
  Load(args->at(0));
  Register value = frame_->PopToRegister();

  __ lw(value, FieldMemOperand(value, String::kHashFieldOffset));
  __ IndexFromHash(value, value);
  frame_->EmitPush(value);
}


void CodeGenerator::VisitCallRuntime(CallRuntime* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif

  if (CheckForInlineRuntimeCall(node)) {
    ASSERT((has_cc() && frame_->height() == original_height) ||
           (!has_cc() && frame_->height() == original_height + 1));
    return;
  }

  ZoneList<Expression*>* args = node->arguments();
  Comment cmnt(masm_, "[ CallRuntime");
  Runtime::Function* function = node->function();

  if (function == NULL) {
    // Prepare stack for calling JS runtime function.
    // Push the builtins object found in the current global object.
    Register scratch = VirtualFrame::scratch0();
    __ lw(scratch, GlobalObject());
    Register builtins = frame_->GetTOSRegister();
    __ lw(builtins, FieldMemOperand(scratch, GlobalObject::kBuiltinsOffset));
    frame_->EmitPush(builtins);
  }

  // Push the arguments ("left-to-right").
  int arg_count = args->length();
  for (int i = 0; i < arg_count; i++) {
    Load(args->at(i));
  }

  VirtualFrame::SpilledScope spilled_scope(frame_);

  if (function == NULL) {
    // Call the JS runtime function.
    __ li(a2, Operand(node->name()));
    InLoopFlag in_loop = loop_nesting() > 0 ? IN_LOOP : NOT_IN_LOOP;
    Handle<Code> stub = ComputeCallInitialize(arg_count, in_loop);
    frame_->CallCodeObject(stub, RelocInfo::CODE_TARGET, arg_count + 1);
    __ lw(cp, frame_->Context());
    frame_->EmitPush(v0);
  } else {
    // Call the C runtime function.
    frame_->CallRuntime(function, arg_count);
    frame_->EmitPush(v0);
  }
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitUnaryOperation(UnaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ UnaryOperation");

  Token::Value op = node->op();

  if (op == Token::NOT) {
    // LoadCondition reversing the false and true targets.
    LoadCondition(node->expression(), false_target(), true_target(), true);
    // LoadCondition may (and usually does) leave a test and branch to
    // be emitted by the caller.  In that case, negate the condition.
    if (has_cc()) cc_reg_ = NegateCondition(cc_reg_);

  } else if (op == Token::DELETE) {
    Property* property = node->expression()->AsProperty();
    Variable* variable = node->expression()->AsVariableProxy()->AsVariable();
    if (property != NULL) {
      Load(property->obj());
      Load(property->key());
      frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
      frame_->EmitPush(v0);

    } else if (variable != NULL) {
      Slot* slot = variable->AsSlot();
      if (variable->is_global()) {
        LoadGlobal();
        frame_->EmitPush(Operand(variable->name()));
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
        frame_->EmitPush(v0);

      } else if (slot != NULL && slot->type() == Slot::LOOKUP) {
        // lookup the context holding the named variable
        frame_->EmitPush(cp);
        frame_->EmitPush(Operand(variable->name()));
        frame_->CallRuntime(Runtime::kLookupContext, 2);
        // v0: context
        frame_->EmitPush(v0);
        frame_->EmitPush(Operand(variable->name()));
        frame_->InvokeBuiltin(Builtins::DELETE, CALL_JS, 2);
        frame_->EmitPush(v0);

      } else {
        // Default: Result of deleting non-global, not dynamically
        // introduced variables is false.
        frame_->EmitPushRoot(Heap::kFalseValueRootIndex);
      }

    } else {
      // Default: Result of deleting expressions is true.
      Load(node->expression());  // may have side-effects
      frame_->Drop();
      frame_->EmitPushRoot(Heap::kTrueValueRootIndex);
    }

  } else if (op == Token::TYPEOF) {
    // Special case for loading the typeof expression; see comment on
    // LoadTypeofExpression().
    LoadTypeofExpression(node->expression());
    frame_->CallRuntime(Runtime::kTypeof, 1);
    frame_->EmitPush(v0);  // v0 holds the result.

  } else {
    bool can_overwrite =
        (node->expression()->AsBinaryOperation() != NULL &&
         node->expression()->AsBinaryOperation()->ResultOverwriteAllowed());
    UnaryOverwriteMode overwrite =
       can_overwrite ? UNARY_OVERWRITE : UNARY_NO_OVERWRITE;

    bool no_negative_zero = node->expression()->no_negative_zero();
    Load(node->expression());
    switch (op) {
      case Token::NOT:
      case Token::DELETE:
      case Token::TYPEOF:
        UNREACHABLE();  // Handled above.
        break;

      case Token::SUB: {
        frame_->PopToA0();
        GenericUnaryOpStub stub(
            Token::SUB,
            overwrite,
            NO_UNARY_FLAGS,
            no_negative_zero ? kIgnoreNegativeZero : kStrictNegativeZero);
        frame_->CallStub(&stub, 0);
        frame_->EmitPush(v0);  // v0 has result
        break;
      }

      case Token::BIT_NOT: {
        Register tos = frame_->PopToRegister();
        JumpTarget not_smi_label;
        JumpTarget continue_label;
        // Smi check.
        __ And(t0, tos, Operand(kSmiTagMask));
        not_smi_label.Branch(ne, t0, Operand(zero_reg));

        // We have a smi. Invert all bits except bit 0.
        __ Xor(tos, tos, 0xfffffffe);
        frame_->EmitPush(tos);
        // The fast case is the first to jump to the continue label, so it gets
        // to decide the virtual frame layout.
        continue_label.Jump();

        not_smi_label.Bind();
        frame_->SpillAll();
        __ Move(a0, tos);
        GenericUnaryOpStub stub(Token::BIT_NOT,
                                overwrite,
                                NO_UNARY_SMI_CODE_IN_STUB);
        frame_->CallStub(&stub, 0);
        frame_->EmitPush(v0);
        continue_label.Bind();
        break;
      }

      case Token::VOID:
        frame_->Drop();
        frame_->EmitPushRoot(Heap::kUndefinedValueRootIndex);
        break;

      case Token::ADD: {
        Register tos = frame_->Peek();
        // Smi check.
        JumpTarget continue_label;
        __ And(t0, tos, Operand(kSmiTagMask));
        continue_label.Branch(eq, t0, Operand(zero_reg));

        frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, 1);
        frame_->EmitPush(v0);  // v0 holds the result.
        continue_label.Bind();
        break;
      }
      default:
        UNREACHABLE();
    }
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCountOperation(CountOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  VirtualFrame::RegisterAllocationScope scope(this);
  Comment cmnt(masm_, "[ CountOperation");

  bool is_postfix = node->is_postfix();
  bool is_increment = node->op() == Token::INC;

  Variable* var = node->expression()->AsVariableProxy()->AsVariable();
  bool is_const = (var != NULL && var->mode() == Variable::CONST);
  bool is_slot = (var != NULL && var->mode() == Variable::VAR);

  if (!is_const && is_slot && type_info(var->AsSlot()).IsSmi()) {
    // The type info declares that this variable is always a Smi.  That
    // means it is a Smi both before and after the increment/decrement.
    // Lets make use of that to make a very minimal count.
    Reference target(this, node->expression(), !is_const);
    ASSERT(!target.is_illegal());
    target.GetValue();  // Pushes the value.
    Register value = frame_->PopToRegister();
    if (is_postfix) frame_->EmitPush(value);
    if (is_increment) {
      __ Addu(value, value, Operand(Smi::FromInt(1)));
    } else {
      __ Subu(value, value, Operand(Smi::FromInt(1)));
    }
    frame_->EmitPush(value);
    target.SetValue(NOT_CONST_INIT, LIKELY_SMI);
    if (is_postfix) frame_->Pop();
    ASSERT_EQ(original_height + 1, frame_->height());
    return;
  }

  // If it's a postfix expression and its result is not ignored and the
  // reference is non-trivial, then push a placeholder on the stack now
  // to hold the result of the expression.
  bool placeholder_pushed = false;
  if (!is_slot && is_postfix) {
    frame_->EmitPush(Operand(Smi::FromInt(0)));
    placeholder_pushed = true;
  }

  // A constant reference is not saved to, so a constant reference is not a
  // compound assignment reference.
  { Reference target(this, node->expression(), !is_const);
    if (target.is_illegal()) {
      // Spoof the virtual frame to have the expected height (one higher
      // than on entry).
      if (!placeholder_pushed) {
        frame_->EmitPush(Operand(Smi::FromInt(0)));
      }
      ASSERT_EQ(original_height + 1, frame_->height());
      return;
    }
    // This pushes 0, 1 or 2 words on the object to be used later when updating
    // the target.  It also pushes the current value of the target.
    target.GetValue();

    JumpTarget slow;
    JumpTarget exit;

    Register value = frame_->PopToRegister();

    // Postfix: Store the old value as the result.
    if (placeholder_pushed) {
      frame_->SetElementAt(value, target.size());
    } else if (is_postfix) {
      frame_->EmitPush(value);
      __ mov(VirtualFrame::scratch0(), value);
      value = VirtualFrame::scratch0();
    }

    // Check for smi operand.
    __ And(t0, value, Operand(kSmiTagMask));
    slow.Branch(ne, t0, Operand(zero_reg));

    // Perform optimistic increment/decrement and check for overflow.
    // If we don't overflow we are done.
    if (is_increment) {
      __ Addu(v0, value, Operand(Smi::FromInt(1)));
      // Check for overflow of value + Smi::FromInt(1).
      __ Xor(t0, v0, value);
      __ Xor(t1, v0, Operand(Smi::FromInt(1)));
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      exit.Branch(ge, t0, Operand(zero_reg));  // Exit on NO overflow (ge 0).
    } else {
      __ Addu(v0, value, Operand(Smi::FromInt(-1)));
      // Check for overflow of value + Smi::FromInt(-1).
      __ Xor(t0, v0, value);
      __ Xor(t1, v0, Operand(Smi::FromInt(-1)));
      __ and_(t0, t0, t1);    // Overflow occurred if result is negative.
      exit.Branch(ge, t0, Operand(zero_reg));  // Exit on NO overflow (ge 0).
    }
    // Slow case: Convert to number.  At this point the
    // value to be incremented is in the value register.
    slow.Bind();

    // Convert the operand to a number.
    frame_->EmitPush(value);

    {
      VirtualFrame::SpilledScope spilled(frame_);
      frame_->InvokeBuiltin(Builtins::TO_NUMBER, CALL_JS, 1);
      if (is_postfix) {
        // Postfix: store to result (on the stack).
        __ sw(v0, frame_->ElementAt(target.size()));
      }

      // Compute the new value.
      frame_->EmitPush(v0);
      frame_->EmitPush(Operand(Smi::FromInt(1)));
      if (is_increment) {
        frame_->CallRuntime(Runtime::kNumberAdd, 2);
      } else {
        frame_->CallRuntime(Runtime::kNumberSub, 2);
      }
    }

    // Store the new value in the target if not const.
    exit.Bind();
    frame_->EmitPush(v0);
    // Set the target with the result, leaving the result on
    // top of the stack.  Removes the target from the stack if
    // it has a non-zero size.
    if (!is_const) target.SetValue(NOT_CONST_INIT, LIKELY_SMI);
  }

  // Postfix: Discard the new value and use the old.
  if (is_postfix) frame_->Pop();
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::GenerateLogicalBooleanOperation(BinaryOperation* node) {
  // According to ECMA-262 section 11.11, page 58, the binary logical
  // operators must yield the result of one of the two expressions
  // before any ToBoolean() conversions. This means that the value
  // produced by a && or || operator is not necessarily a boolean.

  // NOTE: If the left hand side produces a materialized value (not in
  // the CC register), we force the right hand side to do the
  // same. This is necessary because we may have to branch to the exit
  // after evaluating the left hand side (due to the shortcut
  // semantics), but the compiler must (statically) know if the result
  // of compiling the binary operation is materialized or not.
  if (node->op() == Token::AND) {
    JumpTarget is_true;
    LoadCondition(node->left(), &is_true, false_target(), false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      frame_->Dup();
      // Avoid popping the result if it converts to 'false' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&pop_and_continue, &exit);
      Branch(false, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->Pop();

      // Evaluate right side expression.
      is_true.Bind();
      Load(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_true.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly true.
      if (has_cc()) {
        Branch(false, false_target());
      }
      is_true.Bind();
      LoadCondition(node->right(), true_target(), false_target(), false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_true.is_linked());
    }

  } else {
    ASSERT(node->op() == Token::OR);
    JumpTarget is_false;
    LoadCondition(node->left(), true_target(), &is_false, false);
    if (has_valid_frame() && !has_cc()) {
      // The left-hand side result is on top of the virtual frame.
      JumpTarget pop_and_continue;
      JumpTarget exit;

      frame_->Dup();
      // Avoid popping the result if it converts to 'true' using the
      // standard ToBoolean() conversion as described in ECMA-262,
      // section 9.2, page 30.
      ToBoolean(&exit, &pop_and_continue);
      Branch(true, &exit);

      // Pop the result of evaluating the first part.
      pop_and_continue.Bind();
      frame_->Pop();

      // Evaluate right side expression.
      is_false.Bind();
      Load(node->right());

      // Exit (always with a materialized value).
      exit.Bind();
    } else if (has_cc() || is_false.is_linked()) {
      // The left-hand side is either (a) partially compiled to
      // control flow with a final branch left to emit or (b) fully
      // compiled to control flow and possibly false.
      if (has_cc()) {
        Branch(true, true_target());
      }
      is_false.Bind();
      LoadCondition(node->right(), true_target(), false_target(), false);
    } else {
      // Nothing to do.
      ASSERT(!has_valid_frame() && !has_cc() && !is_false.is_linked());
    }
  }
}


void CodeGenerator::VisitBinaryOperation(BinaryOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ BinaryOperation");

  if (node->op() == Token::AND || node->op() == Token::OR) {
    GenerateLogicalBooleanOperation(node);
  } else {
    // Optimize for the case where (at least) one of the expressions
    // is a literal small integer.
    Literal* lliteral = node->left()->AsLiteral();
    Literal* rliteral = node->right()->AsLiteral();
    // NOTE: The code below assumes that the slow cases (calls to runtime)
    // never return a constant/immutable object.
    bool overwrite_left =
        (node->left()->AsBinaryOperation() != NULL &&
         node->left()->AsBinaryOperation()->ResultOverwriteAllowed());
    bool overwrite_right =
        (node->right()->AsBinaryOperation() != NULL &&
         node->right()->AsBinaryOperation()->ResultOverwriteAllowed());

    if (rliteral != NULL && rliteral->handle()->IsSmi()) {
      VirtualFrame::RegisterAllocationScope scope(this);
      Load(node->left());
      if (frame_->KnownSmiAt(0)) overwrite_left = false;
      SmiOperation(node->op(),
                   rliteral->handle(),
                   false,
                   overwrite_left ? OVERWRITE_LEFT : NO_OVERWRITE);

    } else if (lliteral != NULL && lliteral->handle()->IsSmi()) {
      VirtualFrame::RegisterAllocationScope scope(this);
      Load(node->right());
      if (frame_->KnownSmiAt(0)) overwrite_right = false;
      SmiOperation(node->op(),
                   lliteral->handle(),
                   true,
                   overwrite_right ? OVERWRITE_RIGHT : NO_OVERWRITE);
    } else {
      GenerateInlineSmi inline_smi =
          loop_nesting() > 0 ? GENERATE_INLINE_SMI : DONT_GENERATE_INLINE_SMI;
      if (lliteral != NULL) {
        ASSERT(!lliteral->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      if (rliteral != NULL) {
        ASSERT(!rliteral->handle()->IsSmi());
        inline_smi = DONT_GENERATE_INLINE_SMI;
      }
      VirtualFrame::RegisterAllocationScope scope(this);
      OverwriteMode overwrite_mode = NO_OVERWRITE;
      if (overwrite_left) {
        overwrite_mode = OVERWRITE_LEFT;
      } else if (overwrite_right) {
        overwrite_mode = OVERWRITE_RIGHT;
      }
      Load(node->left());
      Load(node->right());
      GenericBinaryOperation(node->op(), overwrite_mode, inline_smi);
    }
  }
  ASSERT(!has_valid_frame() ||
         (has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitThisFunction(ThisFunction* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  frame_->EmitPush(MemOperand(frame_->Function()));
  ASSERT_EQ(original_height + 1, frame_->height());
}


void CodeGenerator::VisitCompareOperation(CompareOperation* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CompareOperation");

  VirtualFrame::RegisterAllocationScope nonspilled_scope(this);

  // Get the expressions from the node.
  Expression* left = node->left();
  Expression* right = node->right();
  Token::Value op = node->op();

  // To make typeof testing for natives implemented in JavaScript really
  // efficient, we generate special code for expressions of the form:
  // 'typeof <expression> == <string>'.
  UnaryOperation* operation = left->AsUnaryOperation();
  if ((op == Token::EQ || op == Token::EQ_STRICT) &&
      (operation != NULL && operation->op() == Token::TYPEOF) &&
      (right->AsLiteral() != NULL &&
       right->AsLiteral()->handle()->IsString())) {
    Handle<String> check(String::cast(*right->AsLiteral()->handle()));

    // Load the operand, move it to register condReg1.
    LoadTypeofExpression(operation->expression());
    Register tos = frame_->PopToRegister();
    __ mov(condReg1, tos);

    Register scratch = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();

    if (check->Equals(Heap::number_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      true_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ LoadRoot(condReg2, Heap::kHeapNumberMapRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::string_symbol())) {
      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));

      // It can be an undetectable string object.
      __ lbu(condReg2, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg2, condReg2, 1 << Map::kIsUndetectable);
      false_target()->Branch(eq, condReg2, Operand(1 << Map::kIsUndetectable),
          no_hint);

      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kInstanceTypeOffset));
      __ li(condReg2, Operand(FIRST_NONSTRING_TYPE));
      cc_reg_ = less;

    } else if (check->Equals(Heap::boolean_symbol())) {
      __ LoadRoot(condReg2, Heap::kTrueValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);
      __ LoadRoot(condReg2, Heap::kFalseValueRootIndex);
      cc_reg_ = eq;

    } else if (check->Equals(Heap::undefined_symbol())) {
      __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

      __ And(condReg2, condReg1, kSmiTagMask);
      false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

      // It can be an undetectable object.
      __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
      __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
      __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
      __ li(condReg2, Operand(1 << Map::kIsUndetectable));

      cc_reg_ = eq;

    } else if (check->Equals(Heap::function_symbol())) {
      __ And(scratch, condReg1, Operand(kSmiTagMask));
      false_target()->Branch(eq, scratch, Operand(zero_reg));

      Register map_reg = scratch;
      __ GetObjectType(condReg1, map_reg, condReg1);
      true_target()->Branch(eq, condReg1, Operand(JS_FUNCTION_TYPE));
      // Regular expressions are callable so typeof == 'function'.
      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
      __ li(condReg2, Operand(JS_REGEXP_TYPE));
      cc_reg_ = eq;

    } else if (check->Equals(Heap::object_symbol())) {
      __ And(scratch, condReg1, Operand(kSmiTagMask));
      false_target()->Branch(eq, scratch, Operand(zero_reg));

      __ LoadRoot(scratch2, Heap::kNullValueRootIndex);
      true_target()->Branch(eq, condReg1, Operand(scratch2));

      Register map_reg = scratch;
      __ GetObjectType(condReg1, map_reg, condReg1);
      false_target()->Branch(eq, condReg1, Operand(JS_REGEXP_TYPE));

      // It can be an undetectable object.
      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kBitFieldOffset));
      __ And(condReg1, condReg1, Operand(1 << Map::kIsUndetectable));
      false_target()->Branch(eq, condReg1, Operand(1 << Map::kIsUndetectable));

      __ lbu(condReg1, FieldMemOperand(map_reg, Map::kInstanceTypeOffset));
      false_target()->Branch(lt, condReg1, Operand(FIRST_JS_OBJECT_TYPE));
      __ li(condReg2, Operand(LAST_JS_OBJECT_TYPE));
      cc_reg_ = le;

    } else {
      // Uncommon case: typeof testing against a string literal that is
      // never returned from the typeof operator.
      false_target()->Jump();
    }
    ASSERT(!has_valid_frame() ||
           (has_cc() && frame_->height() == original_height));
    return;
  }

  switch (op) {
    case Token::EQ:
      Comparison(eq, left, right, false);
      break;

    case Token::LT:
      Comparison(less, left, right);
      break;

    case Token::GT:
      Comparison(greater, left, right);
      break;

    case Token::LTE:
      Comparison(less_equal, left, right);
      break;

    case Token::GTE:
      Comparison(greater_equal, left, right);
      break;

    case Token::EQ_STRICT:
      Comparison(eq, left, right, true);
      break;

    case Token::IN: {
      Load(left);
      Load(right);
      frame_->InvokeBuiltin(Builtins::IN, CALL_JS, 2);
      frame_->EmitPush(v0);
      break;
    }

    case Token::INSTANCEOF: {
      Load(left);
      Load(right);
      InstanceofStub stub;
      frame_->CallStub(&stub, 2);
      // At this point if instanceof succeeded then v0 == 0.
      __ mov(condReg1, v0);
      __ mov(condReg2, zero_reg);
      cc_reg_ = eq;
      break;
    }

    default:
      UNREACHABLE();
  }
  ASSERT((has_cc() && frame_->height() == original_height) ||
         (!has_cc() && frame_->height() == original_height + 1));
}


void CodeGenerator::VisitCompareToNull(CompareToNull* node) {
#ifdef DEBUG
  int original_height = frame_->height();
#endif
  Comment cmnt(masm_, "[ CompareToNull");

  Load(node->expression());
  Register tos = frame_->PopToRegister();
  __ mov(condReg1, tos);
  __ LoadRoot(condReg2, Heap::kNullValueRootIndex);

  // The 'null' value is only equal to 'undefined' if using non-strict
  // comparisons.
  if (!node->is_strict()) {
    true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);
    __ LoadRoot(condReg2, Heap::kUndefinedValueRootIndex);
    true_target()->Branch(eq, condReg1, Operand(condReg2), no_hint);

    __ And(condReg2, condReg1, kSmiTagMask);
    false_target()->Branch(eq, condReg2, Operand(zero_reg), no_hint);

    // It can be an undetectable object.
    __ lw(condReg1, FieldMemOperand(condReg1, HeapObject::kMapOffset));
    __ lbu(condReg1, FieldMemOperand(condReg1, Map::kBitFieldOffset));
    __ And(condReg1, condReg1, 1 << Map::kIsUndetectable);
    __ li(condReg2, Operand(1 << Map::kIsUndetectable));
  }

  // We don't need to load anyting in condReg1 and condReg2 as they are
  // already correctly loaded.
  cc_reg_ = eq;
  ASSERT(has_cc() && frame_->height() == original_height);
}


class DeferredReferenceGetNamedValue: public DeferredCode {
  public:
    explicit DeferredReferenceGetNamedValue(Register receiver,
                                            Handle<String> name)
        : receiver_(receiver), name_(name) {
      set_comment("[ DeferredReferenceGetNamedValue");
    }

    virtual void Generate();

  private:
    Register receiver_;
    Handle<String> name_;
};


// Convention for this is that on entry the receiver is in a register that
// is not used by the stack.  On exit the answer is found in that same
// register and the stack has the same height.
void DeferredReferenceGetNamedValue::Generate() {
#ifdef DEBUG
  int expected_height = frame_state()->frame()->height();
#endif
  VirtualFrame copied_frame(*frame_state()->frame());
  copied_frame.SpillAll();

  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  ASSERT(!receiver_.is(scratch1) && !receiver_.is(scratch2));
  __ DecrementCounter(&Counters::named_load_inline, 1, scratch1, scratch2);
  __ IncrementCounter(&Counters::named_load_inline_miss, 1, scratch1, scratch2);

  // Ensure receiver in a0 and name in a2 to match load ic calling convention.
  __ Move(a0, receiver_);
  __ li(a2, Operand(name_));

  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    Handle<Code> ic(Builtins::builtin(Builtins::LoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);

    // The call must be followed by a nop(1) instruction to indicate that the
    // in-object has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // At this point the answer is in v0.  We move it to the expected register
    // if necessary.
    __ Move(receiver_, v0);

    // Now go back to the frame that we entered with.  This will not overwrite
    // the receiver register since that register was not in use when we came
    // in.  The instructions emitted by this merge are skipped over by the
    // inline load patching mechanism when looking for the branch instruction
    // that tells it where the code to patch is.
    copied_frame.MergeTo(frame_state()->frame());


    // Block the trampoline pool for one more instruction to
    // include the branch instruction ending the deferred code.
    __ BlockTrampolinePoolFor(1);
  }
  ASSERT_EQ(expected_height, frame_state()->frame()->height());
}


class DeferredReferenceGetKeyedValue: public DeferredCode {
 public:
  DeferredReferenceGetKeyedValue(Register key, Register receiver)
      : key_(key), receiver_(receiver) {
    set_comment("[ DeferredReferenceGetKeyedValue");
  }

  virtual void Generate();

 private:
  Register key_;
  Register receiver_;
};


// Takes key and register in a0 and a1 or vice versa.  Returns result
// in a0.
void DeferredReferenceGetKeyedValue::Generate() {
  ASSERT((key_.is(a0) && receiver_.is(a1)) ||
         (key_.is(a1) && receiver_.is(a0)));

  VirtualFrame copied_frame(*frame_state()->frame());
  copied_frame.SpillAll();

  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  __ DecrementCounter(&Counters::keyed_load_inline, 1, scratch1, scratch2);
  __ IncrementCounter(&Counters::keyed_load_inline_miss, 1, scratch1, scratch2);

  // Ensure key in a0 and receiver in a1 to match keyed load ic calling
  // convention.
  if (key_.is(a1)) {
    __ Swap(a0, a1, at);
  }

  // The rest of the instructions in the deferred code must be together.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Call keyed load IC. It has the arguments key and receiver in a0 and a1.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedLoadIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // The call must be followed by a nop instruction to indicate that the
    // keyed load has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);


    // Now go back to the frame that we entered with.  This will not overwrite
    // the receiver or key registers since they were not in use when we came
    // in.  The instructions emitted by this merge are skipped over by the
    // inline load patching mechanism when looking for the branch instruction
    // that tells it where the code to patch is.
    copied_frame.MergeTo(frame_state()->frame());

    // Block the trampoline pool for one more instruction after leaving this
    // constant pool block scope to include the branch instruction ending the
    // deferred code.
    __ BlockTrampolinePoolFor(1);
  }
}


class DeferredReferenceSetKeyedValue: public DeferredCode {
 public:
  DeferredReferenceSetKeyedValue(Register value,
                                 Register key,
                                 Register receiver)
      : value_(value), key_(key), receiver_(receiver) {
    set_comment("[ DeferredReferenceSetKeyedValue");
  }

  virtual void Generate();

 private:
  Register value_;
  Register key_;
  Register receiver_;
};


void DeferredReferenceSetKeyedValue::Generate() {
  Register scratch1 = VirtualFrame::scratch0();
  Register scratch2 = VirtualFrame::scratch1();
  __ DecrementCounter(&Counters::keyed_store_inline, 1, scratch1, scratch2);
  __ IncrementCounter(
      &Counters::keyed_store_inline_miss, 1, scratch1, scratch2);

  // Ensure value in a0, key in a1 and receiver in a2 to match keyed store ic
  // calling convention.
  if (value_.is(a1)) {
    __ Swap(a0, a1, t8);
  }
  ASSERT(receiver_.is(a2));

  // The rest of the instructions in the deferred code must be together.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Call keyed store IC. It has the arguments value, key and receiver in a0,
    // a1 and a2.
    Handle<Code> ic(Builtins::builtin(Builtins::KeyedStoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // The call must be followed by a nop instruction to indicate that the
    // keyed store has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // Block the trampoline pool for one more instruction after leaving this
    // trampoline pool block scope to include the branch instruction ending the
    // deferred code.
    __ BlockTrampolinePoolFor(1);
  }
}


class DeferredReferenceSetNamedValue: public DeferredCode {
 public:
  DeferredReferenceSetNamedValue(Register value,
                                 Register receiver,
                                 Handle<String> name)
      : value_(value), receiver_(receiver), name_(name) {
    set_comment("[ DeferredReferenceSetNamedValue");
  }

  virtual void Generate();

 private:
  Register value_;
  Register receiver_;
  Handle<String> name_;
};


// Takes value in a0 (and in v0), receiver in a1. Must return the result (the
// value) in v0 (this stub does not alter v0, which is passed in by caller.)
void DeferredReferenceSetNamedValue::Generate() {
  // Record the entry frame and spill.
  VirtualFrame copied_frame(*frame_state()->frame());
  copied_frame.SpillAll();

  // Ensure value in a0, receiver in a1 to match store ic calling
  // convention.
  ASSERT(value_.is(a0) && receiver_.is(a1));
  __ li(a2, Operand(name_));

  // The rest of the instructions in the deferred code must be together.
  { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
    // Call named store IC. It has the arguments value, receiever and name in
    // a0, a1 and a2.
    Handle<Code> ic(Builtins::builtin(Builtins::StoreIC_Initialize));
    __ Call(ic, RelocInfo::CODE_TARGET);
    // The call must be followed by a nop instruction to indicate that the
    // named store has been inlined.
    __ nop(PROPERTY_ACCESS_INLINED);

    // Go back to the frame we entered with. The instructions
    // generated by this merge are skipped over by the inline store
    // patching mechanism when looking for the branch instruction that
    // tells it where the code to patch is.
    copied_frame.MergeTo(frame_state()->frame());

    // Block the trampoline pool for one more instruction after leaving this
    // trampoline pool block scope to include the branch instruction ending the
    // deferred code.
    __ BlockTrampolinePoolFor(1);
  }
}


// Consumes the top of stack (the receiver) and pushes the result instead.
void CodeGenerator::EmitNamedLoad(Handle<String> name, bool is_contextual) {
  if (is_contextual || scope()->is_global_scope() || loop_nesting() == 0) {
    Comment cmnt(masm(), "[ Load from named Property");
    // Setup the name register and call load IC.
    frame_->CallLoadIC(name,
                       is_contextual
                           ? RelocInfo::CODE_TARGET_CONTEXT
                           : RelocInfo::CODE_TARGET);
    frame_->EmitPush(v0);  // Push answer.
  } else {
    // Inline the inobject property case.
    Comment cmnt(masm(), "[ Inlined named property load");

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::named_load_inline, 1,
                        frame_->scratch0(), frame_->scratch1());

    // The following instructions are the inlined load of an in-object property.
    // Parts of this code is patched, so the exact instructions generated needs
    // to be fixed. Therefore the instruction pool is blocked when generating
    // this code

    // Load the receiver from the stack.
    Register receiver = frame_->PopToRegister();

    DeferredReferenceGetNamedValue* deferred =
        new DeferredReferenceGetNamedValue(receiver, name);

#ifdef DEBUG
    // 9 instructions. and:1, branch:2, lw:1, li:2, Branch:2, lw:1.
    const int kInlinedNamedLoadInstructions = 9;
    Label check_inlined_codesize;
    masm_->bind(&check_inlined_codesize);
#endif

    // Generate patchable inline code. See LoadIC::PatchInlinedLoad.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      // Check that the receiver is a heap object.
      __ And(at, receiver, Operand(kSmiTagMask));
      deferred->Branch(eq, at, Operand(zero_reg));

      Register scratch = VirtualFrame::scratch0();
      Register scratch2 = VirtualFrame::scratch1();

      // Check the map. The null map used below is patched by the inline cache
      // code.  Therefore we can't use a LoadRoot call.

      __ lw(scratch, FieldMemOperand(receiver, HeapObject::kMapOffset));

      // The null map used below is patched by the inline cache code.
      __ li(scratch2, Operand(Factory::null_value()), true);
      deferred->Branch(ne, scratch, Operand(scratch2));

      // Initially use an invalid index. The index will be patched by the
      // inline cache code.
      __ lw(receiver, MemOperand(receiver, 0));

      // Make sure that the expected number of instructions are generated.
      // If this fails, LoadIC::PatchInlinedLoad() must be fixed as well.
      ASSERT_EQ(kInlinedNamedLoadInstructions,
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }
    deferred->BindExit();
    // At this point the receiver register has the result, either from the
    // deferred code or from the inlined code.
    frame_->EmitPush(receiver);
  }
}


void CodeGenerator::EmitNamedStore(Handle<String> name, bool is_contextual) {
#ifdef DEBUG
  int expected_height = frame()->height() - (is_contextual ? 1 : 2);
#endif

  Result result;
  if (is_contextual || scope()->is_global_scope() || loop_nesting() == 0) {
    frame()->CallStoreIC(name, is_contextual);
  } else {
    // Inline the in-object property case.
    JumpTarget slow, done;

    // Get the value and receiver from the stack.
    frame()->PopToA0();
    Register value = a0;
    __ mov(v0, value);  // On mips, we must also return value in v0.
    frame()->PopToA1();
    Register receiver = a1;

    DeferredReferenceSetNamedValue* deferred =
        new DeferredReferenceSetNamedValue(value, receiver, name);

    // Check that the receiver is a heap object.
    __ And(at, receiver, Operand(kSmiTagMask));
    deferred->Branch(eq, at, Operand(zero_reg));

    // The following instructions are the part of the inlined
    // in-object property store code which can be patched. Therefore
    // the exact number of instructions generated must be fixed, so
    // the trampoline pool is blocked while generating this code.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      Register scratch0 = VirtualFrame::scratch0();
      Register scratch1 = VirtualFrame::scratch1();

      // Check the map. Initially use an invalid map to force a
      // failure. The map check will be patched in the runtime system.
      __ lw(scratch1, FieldMemOperand(receiver, HeapObject::kMapOffset));

#ifdef DEBUG
      Label check_inlined_codesize;
      masm_->bind(&check_inlined_codesize);
#endif
      __ li(scratch0, Operand(Factory::null_value()), true);
      deferred->Branch(ne, scratch0, Operand(scratch1));

      int offset = 0;
      __ sw(value, MemOperand(receiver, offset));

      // Update the write barrier and record its size. We do not use
      // the RecordWrite macro here because we want the offset
      // addition instruction first to make it easy to patch.
      Label record_write_start, record_write_done;
      __ bind(&record_write_start);
      // Add offset into the object.
      __ Addu(scratch0, receiver, Operand(offset));
      // Test that the object is not in the new space.  We cannot set
      // region marks for new space pages.
      __ InNewSpace(receiver, scratch1, eq, &record_write_done);
      // Record the actual write.
      __ RecordWriteHelper(receiver, scratch0, scratch1);
      __ bind(&record_write_done);
      // Clobber all input registers when running with the debug-code flag
      // turned on to provoke errors.
      if (FLAG_debug_code) {
        __ li(receiver, Operand(BitCast<int32_t>(kZapValue)));
        __ li(scratch0, Operand(BitCast<int32_t>(kZapValue)));
        __ li(scratch1, Operand(BitCast<int32_t>(kZapValue)));
      }
      // Check that this is the first inlined write barrier or that
      // this inlined write barrier has the same size as all the other
      // inlined write barriers.
      ASSERT((inlined_write_barrier_size_ == -1) ||
             (inlined_write_barrier_size_ ==
              masm()->InstructionsGeneratedSince(&record_write_start)));
      inlined_write_barrier_size_ =
          masm()->InstructionsGeneratedSince(&record_write_start);

      // Make sure that the expected number of instructions are generated.
      ASSERT_EQ(GetInlinedNamedStoreInstructionsAfterPatch(),
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }
    deferred->BindExit();
  }
  ASSERT_EQ(expected_height, frame()->height());
}


void CodeGenerator::EmitKeyedLoad() {
  if (loop_nesting() == 0) {
    Comment cmnt(masm_, "[ Load from keyed property");
    frame_->CallKeyedLoadIC();
  } else {
    // Inline the keyed load.
    Comment cmnt(masm_, "[ Inlined load from keyed property");

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::keyed_load_inline, 1,
                        frame_->scratch0(), frame_->scratch1());

    // Load the key and receiver from the stack.
    bool key_is_known_smi = frame_->KnownSmiAt(0);
    Register key = frame_->PopToRegister();
    Register receiver = frame_->PopToRegister(key);

    // The deferred code expects key and receiver in registers.
    DeferredReferenceGetKeyedValue* deferred =
        new DeferredReferenceGetKeyedValue(key, receiver);

    // Check that the receiver is a heap object.
    __ And(at, receiver, Operand(kSmiTagMask));
    deferred->Branch(eq, at, Operand(zero_reg));

    // The following instructions are the part of the inlined load keyed
    // property code which can be patched. Therefore the exact number of
    // instructions generated need to be fixed, so the trampoline pool is
    // blocked while generating this code.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
      Register scratch1 = VirtualFrame::scratch0();
      Register scratch2 = VirtualFrame::scratch1();
      // Check the map. The null map used below is patched by the inline cache
      // code.
      __ lw(scratch1, FieldMemOperand(receiver, HeapObject::kMapOffset));
       // Check that the key is a smi.
      if (!key_is_known_smi) {
        __ And(scratch2, key, Operand(kSmiTagMask));
        deferred->Branch(ne, scratch2, Operand(zero_reg));
      }
#ifdef DEBUG
      Label check_inlined_codesize;
      masm_->bind(&check_inlined_codesize);
#endif
      __ li(scratch2, Operand(Factory::null_value()), true);
      deferred->Branch(ne, scratch1, Operand(scratch2));

      // Check that the key is a smi.
      __ And(at, key, Operand(kSmiTagMask));
      deferred->Branch(ne, at, Operand(zero_reg));

      // Get the elements array from the receiver.
      __ lw(scratch1, FieldMemOperand(receiver, JSObject::kElementsOffset));
      __ AssertFastElements(scratch1);

      // Check that key is within bounds. Use unsigned comparison to handle
      // negative keys.
      __ lw(scratch2, FieldMemOperand(scratch1, FixedArray::kLengthOffset));
      deferred->Branch(ls, scratch2, Operand(key));  // Unsigned less equal.

      // Load and check that the result is not the hole (key is a smi).
      __ LoadRoot(scratch2, Heap::kTheHoleValueRootIndex);
      __ Addu(scratch1,
              scratch1,
              Operand(FixedArray::kHeaderSize - kHeapObjectTag));
      __ sll(at, key, kPointerSizeLog2 - (kSmiTagSize + kSmiShiftSize));
      __ addu(at, at, scratch1);
      __ lw(scratch1, MemOperand(at, 0));

      deferred->Branch(eq, scratch1, Operand(scratch2));

      __ mov(v0, scratch1);
      ASSERT_EQ(GetInlinedKeyedLoadInstructionsAfterPatch(),
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }

    deferred->BindExit();
  }
}


void CodeGenerator::EmitKeyedStore(StaticType* key_type,
                                   WriteBarrierCharacter wb_info) {
  // Generate inlined version of the keyed store if the code is in a loop
  // and the key is likely to be a smi.
  if (loop_nesting() > 0 && key_type->IsLikelySmi()) {
    // Inline the keyed store.
    Comment cmnt(masm_, "[ Inlined store to keyed property");

    Register scratch1 = VirtualFrame::scratch0();
    Register scratch2 = VirtualFrame::scratch1();
    Register scratch3 = a3;

    // Counter will be decremented in the deferred code. Placed here to avoid
    // having it in the instruction stream below where patching will occur.
    __ IncrementCounter(&Counters::keyed_store_inline, 1,
                        scratch1, scratch2);

    // Load the value, key and receiver from the stack.
    bool value_is_harmless = frame_->KnownSmiAt(0);
    if (wb_info == NEVER_NEWSPACE) value_is_harmless = true;
    bool key_is_smi = frame_->KnownSmiAt(1);
    Register value = frame_->PopToRegister();
    Register key = frame_->PopToRegister(value);
    VirtualFrame::SpilledScope spilled(frame_);
    Register receiver = a2;
    frame_->EmitPop(receiver);

#ifdef DEBUG
    bool we_remembered_the_write_barrier = value_is_harmless;
#endif

    // The deferred code expects value, key and receiver in registers.
    DeferredReferenceSetKeyedValue* deferred =
        new DeferredReferenceSetKeyedValue(value, key, receiver);

    // Check that the value is a smi. As this inlined code does not set the
    // write barrier it is only possible to store smi values.
    if (!value_is_harmless) {
      // If the value is not likely to be a Smi then let's test the fixed array
      // for new space instead.  See below.
      if (wb_info == LIKELY_SMI) {
        __ And(at, value, Operand(kSmiTagMask));
        deferred->Branch(ne, at, Operand(zero_reg));
  #ifdef DEBUG
        we_remembered_the_write_barrier = true;
  #endif
      }
    }

    if (!key_is_smi) {
      // Check that the key is a smi.
      __ And(at, key, Operand(kSmiTagMask));
      deferred->Branch(ne, at, Operand(zero_reg));
    }

    // Check that the receiver is a heap object.
    __ And(at, receiver, Operand(kSmiTagMask));
    deferred->Branch(eq, at, Operand(zero_reg));

    // Check that the receiver is a JSArray.
    __ GetObjectType(receiver, scratch1, scratch1);
    deferred->Branch(ne, scratch1, Operand(JS_ARRAY_TYPE));

    // Check that the key is within bounds. Both the key and the length of
    // the JSArray are smis. Use unsigned comparison to handle negative keys.
    __ lw(scratch1, FieldMemOperand(receiver, JSArray::kLengthOffset));
    deferred->Branch(ls, scratch1, Operand(key));  // Unsigned less equal.

    // Get the elements array from the receiver.
    __ lw(scratch1, FieldMemOperand(receiver, JSObject::kElementsOffset));
    if (!value_is_harmless && wb_info != LIKELY_SMI) {
      Label ok;
      __ And(scratch2, scratch1, Operand(ExternalReference::new_space_mask()));
      __ Branch(&ok,
                eq,
                scratch2,
                Operand(ExternalReference::new_space_start()));
      __ And(at, value, Operand(kSmiTagMask));
      deferred->Branch(ne, at, Operand(zero_reg));
      __ bind(&ok);
#ifdef DEBUG
      we_remembered_the_write_barrier = true;
#endif
    }

    // Check that the elements array is not a dictionary.
    __ lw(scratch2, FieldMemOperand(scratch1, JSObject::kMapOffset));

    // The following instructions are the part of the inlined store keyed
    // property code which can be patched. Therefore the exact number of
    // instructions generated need to be fixed, so the trampoline pool is
    // blocked while generating this code.
    { Assembler::BlockTrampolinePoolScope block_trampoline_pool(masm_);
#ifdef DEBUG
    Label check_inlined_codesize;
    masm_->bind(&check_inlined_codesize);
#endif

      // Read the fixed array map from inlined code (li) (not from the root
      // array) so that the value can be patched.  When debugging, we patch this
      // comparison to always fail so that we will hit the IC call in the
      // deferred code which will allow the debugger to break for fast case
      // stores.

      __ li(scratch3, Operand(Factory::fixed_array_map()), true);
      deferred->Branch(ne, scratch2, Operand(scratch3));

      // Store the value.
      __ Addu(scratch1, scratch1,
              Operand(FixedArray::kHeaderSize - kHeapObjectTag));

      // Use (Smi) key  to index array pointed to by scratch1.
      __ sll(at, key, kPointerSizeLog2 - (kSmiTagSize + kSmiShiftSize));
      __ addu(at, scratch1, at);
      __ sw(value, MemOperand(at, 0));
      __ mov(v0, value);  // Leave stored value in v0.

      // Make sure that the expected number of instructions are generated.
      // If fail, KeyedStoreIC::PatchInlinedStore() must be fixed as well.
      ASSERT_EQ(kInlinedKeyedStoreInstructionsAfterPatch,
                masm_->InstructionsGeneratedSince(&check_inlined_codesize));
    }

    ASSERT(we_remembered_the_write_barrier);

    deferred->BindExit();
  } else {
    frame()->CallKeyedStoreIC();
  }
}


#ifdef DEBUG
bool CodeGenerator::HasValidEntryRegisters() { return true; }
#endif


#undef __
#define __ ACCESS_MASM(masm)

// -----------------------------------------------------------------------------
// Reference support.


Handle<String> Reference::GetName() {
  ASSERT(type_ == NAMED);
  Property* property = expression_->AsProperty();
  if (property == NULL) {
    // Global variable reference treated as a named property reference.
    VariableProxy* proxy = expression_->AsVariableProxy();
    ASSERT(proxy->AsVariable() != NULL);
    ASSERT(proxy->AsVariable()->is_global());
    return proxy->name();
  } else {
    Literal* raw_name = property->key()->AsLiteral();
    ASSERT(raw_name != NULL);
    return Handle<String>(String::cast(*raw_name->handle()));
  }
}


void Reference::DupIfPersist() {
  if (persist_after_get_) {
    switch (type_) {
      case KEYED:
        cgen_->frame()->Dup2();
        break;
      case NAMED:
        cgen_->frame()->Dup();
        // Fall through.
      case UNLOADED:
      case ILLEGAL:
      case SLOT:
        // Do nothing.
        ;
    }
  } else {
    set_unloaded();
  }
}


void Reference::GetValue() {
  ASSERT(cgen_->HasValidEntryRegisters());
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Load from Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->AsSlot();
      ASSERT(slot != NULL);
      DupIfPersist();
      cgen_->LoadFromSlotCheckForArguments(slot, NOT_INSIDE_TYPEOF);
      break;
    }

    case NAMED: {
      Variable* var = expression_->AsVariableProxy()->AsVariable();
      bool is_global = var != NULL;
      ASSERT(!is_global || var->is_global());
      Handle<String> name = GetName();
      DupIfPersist();
      cgen_->EmitNamedLoad(name, is_global);
      break;
    }

    case KEYED: {
      ASSERT(property != NULL);
      DupIfPersist();
      cgen_->EmitKeyedLoad();
      cgen_->frame()->EmitPush(v0);
      break;
    }

    default:
      UNREACHABLE();
  }
}


void Reference::SetValue(InitState init_state, WriteBarrierCharacter wb_info) {
  ASSERT(!is_illegal());
  ASSERT(!cgen_->has_cc());
  MacroAssembler* masm = cgen_->masm();
  VirtualFrame* frame = cgen_->frame();
  Property* property = expression_->AsProperty();
  if (property != NULL) {
    cgen_->CodeForSourcePosition(property->position());
  }

  switch (type_) {
    case SLOT: {
      Comment cmnt(masm, "[ Store to Slot");
      Slot* slot = expression_->AsVariableProxy()->AsVariable()->AsSlot();
      cgen_->StoreToSlot(slot, init_state);
      set_unloaded();
      break;
    }

    case NAMED: {
      Comment cmnt(masm, "[ Store to named Property");
      cgen_->EmitNamedStore(GetName(), false);
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    case KEYED: {
      Comment cmnt(masm, "[ Store to keyed Property");
      Property* property = expression_->AsProperty();
      ASSERT(property != NULL);
      cgen_->CodeForSourcePosition(property->position());

      cgen_->EmitKeyedStore(property->key()->type(), wb_info);
      frame->EmitPush(v0);
      set_unloaded();
      break;
    }

    default:
      UNREACHABLE();
  }
}


const char* GenericBinaryOpStub::GetName() {
  if (name_ != NULL) return name_;
  const int len = 100;
  name_ = Bootstrapper::AllocateAutoDeletedArray(len);
  if (name_ == NULL) return "OOM";
  const char* op_name = Token::Name(op_);
  const char* overwrite_name;
  switch (mode_) {
    case NO_OVERWRITE: overwrite_name = "Alloc"; break;
    case OVERWRITE_RIGHT: overwrite_name = "OverwriteRight"; break;
    case OVERWRITE_LEFT: overwrite_name = "OverwriteLeft"; break;
    default: overwrite_name = "UnknownOverwrite"; break;
  }

  OS::SNPrintF(Vector<char>(name_, len),
               "GenericBinaryOpStub_%s_%s%s_%s",
               op_name,
               overwrite_name,
               specialized_on_rhs_ ? "_ConstantRhs" : "",
               BinaryOpIC::GetName(runtime_operands_type_));
  return name_;
}


#undef __

} }  // namespace v8::internal

#endif  // V8_TARGET_ARCH_MIPS
