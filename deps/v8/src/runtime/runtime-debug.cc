// Copyright 2014 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <vector>

#include "src/codegen/compiler.h"
#include "src/common/globals.h"
#include "src/debug/debug-coverage.h"
#include "src/debug/debug-evaluate.h"
#include "src/debug/debug-frames.h"
#include "src/debug/debug-scopes.h"
#include "src/debug/debug.h"
#include "src/debug/liveedit.h"
#include "src/execution/arguments-inl.h"
#include "src/execution/frames-inl.h"
#include "src/execution/isolate-inl.h"
#include "src/heap/heap-inl.h"  // For ToBoolean. TODO(jkummerow): Drop.
#include "src/interpreter/bytecode-array-accessor.h"
#include "src/interpreter/bytecodes.h"
#include "src/interpreter/interpreter.h"
#include "src/logging/counters.h"
#include "src/objects/debug-objects-inl.h"
#include "src/objects/heap-object-inl.h"
#include "src/objects/js-collection-inl.h"
#include "src/objects/js-generator-inl.h"
#include "src/objects/js-promise-inl.h"
#include "src/runtime/runtime-utils.h"
#include "src/runtime/runtime.h"
#include "src/snapshot/embedded/embedded-data.h"
#include "src/snapshot/snapshot.h"
#include "src/wasm/wasm-objects-inl.h"

#include <dlfcn.h>
#include <sys/time.h>
#include <unistd.h>

namespace v8 {
namespace internal {

RUNTIME_FUNCTION_RETURN_PAIR(Runtime_DebugBreakOnBytecode) {
  using interpreter::Bytecode;
  using interpreter::Bytecodes;
  using interpreter::OperandScale;

  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(Object, value, 0);
  HandleScope scope(isolate);

  // Return value can be changed by debugger. Last set value will be used as
  // return value.
  ReturnValueScope result_scope(isolate->debug());
  isolate->debug()->set_return_value(*value);

  // Get the top-most JavaScript frame.
  JavaScriptFrameIterator it(isolate);
  if (isolate->debug_execution_mode() == DebugInfo::kBreakpoints) {
    isolate->debug()->Break(it.frame(),
                            handle(it.frame()->function(), isolate));
  }

  // If we are dropping frames, there is no need to get a return value or
  // bytecode, since we will be restarting execution at a different frame.
  if (isolate->debug()->will_restart()) {
    return MakePair(ReadOnlyRoots(isolate).undefined_value(),
                    Smi::FromInt(static_cast<uint8_t>(Bytecode::kIllegal)));
  }

  // Return the handler from the original bytecode array.
  DCHECK(it.frame()->is_interpreted());
  InterpretedFrame* interpreted_frame =
      reinterpret_cast<InterpretedFrame*>(it.frame());

  bool side_effect_check_failed = false;
  if (isolate->debug_execution_mode() == DebugInfo::kSideEffects) {
    side_effect_check_failed =
        !isolate->debug()->PerformSideEffectCheckAtBytecode(interpreted_frame);
  }

  // Make sure to only access these objects after the side effect check, as the
  // check can allocate on failure.
  SharedFunctionInfo shared = interpreted_frame->function().shared();
  BytecodeArray bytecode_array = shared.GetBytecodeArray();
  int bytecode_offset = interpreted_frame->GetBytecodeOffset();
  Bytecode bytecode = Bytecodes::FromByte(bytecode_array.get(bytecode_offset));

  if (Bytecodes::Returns(bytecode)) {
    // If we are returning (or suspending), reset the bytecode array on the
    // interpreted stack frame to the non-debug variant so that the interpreter
    // entry trampoline sees the return/suspend bytecode rather than the
    // DebugBreak.
    interpreted_frame->PatchBytecodeArray(bytecode_array);
  }

  // We do not have to deal with operand scale here. If the bytecode at the
  // break is prefixed by operand scaling, we would have patched over the
  // scaling prefix. We now simply dispatch to the handler for the prefix.
  // We need to deserialize now to ensure we don't hit the debug break again
  // after deserializing.
  OperandScale operand_scale = OperandScale::kSingle;
  isolate->interpreter()->GetBytecodeHandler(bytecode, operand_scale);

  if (side_effect_check_failed) {
    return MakePair(ReadOnlyRoots(isolate).exception(),
                    Smi::FromInt(static_cast<uint8_t>(bytecode)));
  }
  Object interrupt_object = isolate->stack_guard()->HandleInterrupts();
  if (interrupt_object.IsException(isolate)) {
    return MakePair(interrupt_object,
                    Smi::FromInt(static_cast<uint8_t>(bytecode)));
  }
  return MakePair(isolate->debug()->return_value(),
                  Smi::FromInt(static_cast<uint8_t>(bytecode)));
}

RUNTIME_FUNCTION(Runtime_DebugBreakAtEntry) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  USE(function);

  DCHECK(function->shared().HasDebugInfo());
  DCHECK(function->shared().GetDebugInfo().BreakAtEntry());

  // Get the top-most JavaScript frame. This is the debug target function.
  JavaScriptFrameIterator it(isolate);
  DCHECK_EQ(*function, it.frame()->function());
  // Check whether the next JS frame is closer than the last API entry.
  // if yes, then the call to the debug target came from JavaScript. Otherwise,
  // the call to the debug target came from API. Do not break for the latter.
  it.Advance();
  if (!it.done() &&
      it.frame()->fp() < isolate->thread_local_top()->last_api_entry_) {
    isolate->debug()->Break(it.frame(), function);
  }

  return ReadOnlyRoots(isolate).undefined_value();
}

extern "C" void V8RecordReplayOnDebuggerStatement();

RUNTIME_FUNCTION(Runtime_HandleDebuggerStatement) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  if (recordreplay::IsRecordingOrReplaying()) {
    V8RecordReplayOnDebuggerStatement();
  }
  if (isolate->debug()->break_points_active()) {
    isolate->debug()->HandleDebugBreak(kIgnoreIfTopFrameBlackboxed);
  }
  return isolate->stack_guard()->HandleInterrupts();
}

RUNTIME_FUNCTION(Runtime_ScheduleBreak) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  isolate->RequestInterrupt(
      [](v8::Isolate* isolate, void*) { v8::debug::BreakRightNow(isolate); },
      nullptr);
  return ReadOnlyRoots(isolate).undefined_value();
}

template <class IteratorType>
static MaybeHandle<JSArray> GetIteratorInternalProperties(
    Isolate* isolate, Handle<IteratorType> object) {
  Factory* factory = isolate->factory();
  Handle<IteratorType> iterator = Handle<IteratorType>::cast(object);
  const char* kind = nullptr;
  switch (iterator->map().instance_type()) {
    case JS_MAP_KEY_ITERATOR_TYPE:
      kind = "keys";
      break;
    case JS_MAP_KEY_VALUE_ITERATOR_TYPE:
    case JS_SET_KEY_VALUE_ITERATOR_TYPE:
      kind = "entries";
      break;
    case JS_MAP_VALUE_ITERATOR_TYPE:
    case JS_SET_VALUE_ITERATOR_TYPE:
      kind = "values";
      break;
    default:
      UNREACHABLE();
  }

  Handle<FixedArray> result = factory->NewFixedArray(2 * 3);
  Handle<String> has_more =
      factory->NewStringFromAsciiChecked("[[IteratorHasMore]]");
  result->set(0, *has_more);
  result->set(1, isolate->heap()->ToBoolean(iterator->HasMore()));

  Handle<String> index =
      factory->NewStringFromAsciiChecked("[[IteratorIndex]]");
  result->set(2, *index);
  result->set(3, iterator->index());

  Handle<String> iterator_kind =
      factory->NewStringFromAsciiChecked("[[IteratorKind]]");
  result->set(4, *iterator_kind);
  Handle<String> kind_str = factory->NewStringFromAsciiChecked(kind);
  result->set(5, *kind_str);
  return factory->NewJSArrayWithElements(result);
}


MaybeHandle<JSArray> Runtime::GetInternalProperties(Isolate* isolate,
                                                    Handle<Object> object) {
  Factory* factory = isolate->factory();
  if (object->IsJSBoundFunction()) {
    Handle<JSBoundFunction> function = Handle<JSBoundFunction>::cast(object);

    Handle<FixedArray> result = factory->NewFixedArray(2 * 3);
    Handle<String> target =
        factory->NewStringFromAsciiChecked("[[TargetFunction]]");
    result->set(0, *target);
    result->set(1, function->bound_target_function());

    Handle<String> bound_this =
        factory->NewStringFromAsciiChecked("[[BoundThis]]");
    result->set(2, *bound_this);
    result->set(3, function->bound_this());

    Handle<String> bound_args =
        factory->NewStringFromAsciiChecked("[[BoundArgs]]");
    result->set(4, *bound_args);
    Handle<FixedArray> bound_arguments =
        factory->CopyFixedArray(handle(function->bound_arguments(), isolate));
    Handle<JSArray> arguments_array =
        factory->NewJSArrayWithElements(bound_arguments);
    result->set(5, *arguments_array);
    return factory->NewJSArrayWithElements(result);
  } else if (object->IsJSMapIterator()) {
    Handle<JSMapIterator> iterator = Handle<JSMapIterator>::cast(object);
    return GetIteratorInternalProperties(isolate, iterator);
  } else if (object->IsJSSetIterator()) {
    Handle<JSSetIterator> iterator = Handle<JSSetIterator>::cast(object);
    return GetIteratorInternalProperties(isolate, iterator);
  } else if (object->IsJSGeneratorObject()) {
    Handle<JSGeneratorObject> generator =
        Handle<JSGeneratorObject>::cast(object);

    const char* status = "suspended";
    if (generator->is_closed()) {
      status = "closed";
    } else if (generator->is_executing()) {
      status = "running";
    } else {
      DCHECK(generator->is_suspended());
    }

    Handle<FixedArray> result = factory->NewFixedArray(2 * 3);
    Handle<String> generator_status =
        factory->NewStringFromAsciiChecked("[[GeneratorState]]");
    result->set(0, *generator_status);
    Handle<String> status_str = factory->NewStringFromAsciiChecked(status);
    result->set(1, *status_str);

    Handle<String> function =
        factory->NewStringFromAsciiChecked("[[GeneratorFunction]]");
    result->set(2, *function);
    result->set(3, generator->function());

    Handle<String> receiver =
        factory->NewStringFromAsciiChecked("[[GeneratorReceiver]]");
    result->set(4, *receiver);
    result->set(5, generator->receiver());
    return factory->NewJSArrayWithElements(result);
  } else if (object->IsJSPromise()) {
    Handle<JSPromise> promise = Handle<JSPromise>::cast(object);
    const char* status = JSPromise::Status(promise->status());
    Handle<FixedArray> result = factory->NewFixedArray(2 * 2);
    Handle<String> promise_status =
        factory->NewStringFromAsciiChecked("[[PromiseState]]");
    result->set(0, *promise_status);
    Handle<String> status_str = factory->NewStringFromAsciiChecked(status);
    result->set(1, *status_str);

    Handle<Object> value_obj(promise->status() == Promise::kPending
                                 ? ReadOnlyRoots(isolate).undefined_value()
                                 : promise->result(),
                             isolate);
    Handle<String> promise_value =
        factory->NewStringFromAsciiChecked("[[PromiseResult]]");
    result->set(2, *promise_value);
    result->set(3, *value_obj);
    return factory->NewJSArrayWithElements(result);
  } else if (object->IsJSProxy()) {
    Handle<JSProxy> js_proxy = Handle<JSProxy>::cast(object);
    Handle<FixedArray> result = factory->NewFixedArray(3 * 2);

    Handle<String> handler_str =
        factory->NewStringFromAsciiChecked("[[Handler]]");
    result->set(0, *handler_str);
    result->set(1, js_proxy->handler());

    Handle<String> target_str =
        factory->NewStringFromAsciiChecked("[[Target]]");
    result->set(2, *target_str);
    result->set(3, js_proxy->target());

    Handle<String> is_revoked_str =
        factory->NewStringFromAsciiChecked("[[IsRevoked]]");
    result->set(4, *is_revoked_str);
    result->set(5, isolate->heap()->ToBoolean(js_proxy->IsRevoked()));
    return factory->NewJSArrayWithElements(result);
  } else if (object->IsJSPrimitiveWrapper()) {
    Handle<JSPrimitiveWrapper> js_value =
        Handle<JSPrimitiveWrapper>::cast(object);

    Handle<FixedArray> result = factory->NewFixedArray(2);
    Handle<String> primitive_value =
        factory->NewStringFromAsciiChecked("[[PrimitiveValue]]");
    result->set(0, *primitive_value);
    result->set(1, js_value->value());
    return factory->NewJSArrayWithElements(result);
  } else if (object->IsJSArrayBuffer()) {
    Handle<JSArrayBuffer> js_array_buffer = Handle<JSArrayBuffer>::cast(object);
    Handle<FixedArray> result = factory->NewFixedArray(1 * 2);

    Handle<String> is_detached_str =
        factory->NewStringFromAsciiChecked("[[IsDetached]]");
    result->set(0, *is_detached_str);
    result->set(1, isolate->heap()->ToBoolean(js_array_buffer->was_detached()));
    return factory->NewJSArrayWithElements(result);
  }
  return factory->NewJSArray(0);
}

RUNTIME_FUNCTION(Runtime_GetGeneratorScopeCount) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());

  if (!args[0].IsJSGeneratorObject()) return Smi::zero();

  // Check arguments.
  CONVERT_ARG_HANDLE_CHECKED(JSGeneratorObject, gen, 0);

  // Only inspect suspended generator scopes.
  if (!gen->is_suspended()) {
    return Smi::zero();
  }

  // Count the visible scopes.
  int n = 0;
  for (ScopeIterator it(isolate, gen); !it.Done(); it.Next()) {
    n++;
  }

  return Smi::FromInt(n);
}

RUNTIME_FUNCTION(Runtime_GetGeneratorScopeDetails) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());

  if (!args[0].IsJSGeneratorObject()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  // Check arguments.
  CONVERT_ARG_HANDLE_CHECKED(JSGeneratorObject, gen, 0);
  CONVERT_NUMBER_CHECKED(int, index, Int32, args[1]);

  // Only inspect suspended generator scopes.
  if (!gen->is_suspended()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  // Find the requested scope.
  int n = 0;
  ScopeIterator it(isolate, gen);
  for (; !it.Done() && n < index; it.Next()) {
    n++;
  }
  if (it.Done()) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  return *it.MaterializeScopeDetails();
}

static bool SetScopeVariableValue(ScopeIterator* it, int index,
                                  Handle<String> variable_name,
                                  Handle<Object> new_value) {
  for (int n = 0; !it->Done() && n < index; it->Next()) {
    n++;
  }
  if (it->Done()) {
    return false;
  }
  return it->SetVariableValue(variable_name, new_value);
}

// Change variable value in closure or local scope
// args[0]: number or JsFunction: break id or function
// args[1]: number: scope index
// args[2]: string: variable name
// args[3]: object: new value
//
// Return true if success and false otherwise
RUNTIME_FUNCTION(Runtime_SetGeneratorScopeVariableValue) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSGeneratorObject, gen, 0);
  CONVERT_NUMBER_CHECKED(int, index, Int32, args[1]);
  CONVERT_ARG_HANDLE_CHECKED(String, variable_name, 2);
  CONVERT_ARG_HANDLE_CHECKED(Object, new_value, 3);
  ScopeIterator it(isolate, gen);
  bool res = SetScopeVariableValue(&it, index, variable_name, new_value);
  return isolate->heap()->ToBoolean(res);
}


RUNTIME_FUNCTION(Runtime_GetBreakLocations) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CHECK(isolate->debug()->is_active());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, fun, 0);

  Handle<SharedFunctionInfo> shared(fun->shared(), isolate);
  // Find the number of break points
  Handle<Object> break_locations =
      Debug::GetSourceBreakLocations(isolate, shared);
  if (break_locations->IsUndefined(isolate)) {
    return ReadOnlyRoots(isolate).undefined_value();
  }
  // Return array as JS array
  return *isolate->factory()->NewJSArrayWithElements(
      Handle<FixedArray>::cast(break_locations));
}


// Returns the state of break on exceptions
// args[0]: boolean indicating uncaught exceptions
RUNTIME_FUNCTION(Runtime_IsBreakOnException) {
  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_NUMBER_CHECKED(uint32_t, type_arg, Uint32, args[0]);

  ExceptionBreakType type = static_cast<ExceptionBreakType>(type_arg);
  bool result = isolate->debug()->IsBreakOnException(type);
  return Smi::FromInt(result);
}

// Clear all stepping set by PrepareStep.
RUNTIME_FUNCTION(Runtime_ClearStepping) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  CHECK(isolate->debug()->is_active());
  isolate->debug()->ClearStepping();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugGetLoadedScriptIds) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  Handle<FixedArray> instances;
  {
    DebugScope debug_scope(isolate->debug());
    // Fill the script objects.
    instances = isolate->debug()->GetLoadedScripts();
  }

  // Convert the script objects to proper JS objects.
  for (int i = 0; i < instances->length(); i++) {
    Handle<Script> script(Script::cast(instances->get(i)), isolate);
    instances->set(i, Smi::FromInt(script->id()));
  }

  // Return result as a JS array.
  return *isolate->factory()->NewJSArrayWithElements(instances);
}


RUNTIME_FUNCTION(Runtime_FunctionGetInferredName) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());

  CONVERT_ARG_CHECKED(Object, f, 0);
  if (f.IsJSFunction()) {
    return JSFunction::cast(f).shared().inferred_name();
  }
  return ReadOnlyRoots(isolate).empty_string();
}


// Performs a GC.
// Presently, it only does a full GC.
RUNTIME_FUNCTION(Runtime_CollectGarbage) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(1, args.length());
  isolate->heap()->PreciseCollectAllGarbage(Heap::kNoGCFlags,
                                            GarbageCollectionReason::kRuntime);
  return ReadOnlyRoots(isolate).undefined_value();
}


// Gets the current heap usage.
RUNTIME_FUNCTION(Runtime_GetHeapUsage) {
  SealHandleScope shs(isolate);
  DCHECK_EQ(0, args.length());
  int usage = static_cast<int>(isolate->heap()->SizeOfObjects());
  if (!Smi::IsValid(usage)) {
    return *isolate->factory()->NewNumberFromInt(usage);
  }
  return Smi::FromInt(usage);
}

namespace {

int ScriptLinePosition(Handle<Script> script, int line) {
  if (line < 0) return -1;

  if (script->type() == Script::TYPE_WASM) {
    // Wasm positions are relative to the start of the module.
    return 0;
  }

  Script::InitLineEnds(script->GetIsolate(), script);

  FixedArray line_ends_array = FixedArray::cast(script->line_ends());
  const int line_count = line_ends_array.length();
  DCHECK_LT(0, line_count);

  if (line == 0) return 0;
  // If line == line_count, we return the first position beyond the last line.
  if (line > line_count) return -1;
  return Smi::ToInt(line_ends_array.get(line - 1)) + 1;
}

int ScriptLinePositionWithOffset(Handle<Script> script, int line, int offset) {
  if (line < 0 || offset < 0) return -1;

  if (line == 0 || offset == 0)
    return ScriptLinePosition(script, line) + offset;

  Script::PositionInfo info;
  if (!Script::GetPositionInfo(script, offset, &info, Script::NO_OFFSET)) {
    return -1;
  }

  const int total_line = info.line + line;
  return ScriptLinePosition(script, total_line);
}

Handle<Object> GetJSPositionInfo(Handle<Script> script, int position,
                                 Script::OffsetFlag offset_flag,
                                 Isolate* isolate) {
  Script::PositionInfo info;
  if (!Script::GetPositionInfo(script, position, &info, offset_flag)) {
    return isolate->factory()->null_value();
  }

  Handle<String> source = handle(String::cast(script->source()), isolate);
  Handle<String> sourceText = script->type() == Script::TYPE_WASM
                                  ? isolate->factory()->empty_string()
                                  : isolate->factory()->NewSubString(
                                        source, info.line_start, info.line_end);

  Handle<JSObject> jsinfo =
      isolate->factory()->NewJSObject(isolate->object_function());

  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->script_string(),
                        script, NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->position_string(),
                        handle(Smi::FromInt(position), isolate), NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->line_string(),
                        handle(Smi::FromInt(info.line), isolate), NONE);
  JSObject::AddProperty(isolate, jsinfo, isolate->factory()->column_string(),
                        handle(Smi::FromInt(info.column), isolate), NONE);
  JSObject::AddProperty(isolate, jsinfo,
                        isolate->factory()->sourceText_string(), sourceText,
                        NONE);

  return jsinfo;
}

Handle<Object> ScriptLocationFromLine(Isolate* isolate, Handle<Script> script,
                                      Handle<Object> opt_line,
                                      Handle<Object> opt_column,
                                      int32_t offset) {
  // Line and column are possibly undefined and we need to handle these cases,
  // additionally subtracting corresponding offsets.

  int32_t line = 0;
  if (!opt_line->IsNullOrUndefined(isolate)) {
    CHECK(opt_line->IsNumber());
    line = NumberToInt32(*opt_line) - script->line_offset();
  }

  int32_t column = 0;
  if (!opt_column->IsNullOrUndefined(isolate)) {
    CHECK(opt_column->IsNumber());
    column = NumberToInt32(*opt_column);
    if (line == 0) column -= script->column_offset();
  }

  int line_position = ScriptLinePositionWithOffset(script, line, offset);
  if (line_position < 0 || column < 0) return isolate->factory()->null_value();

  return GetJSPositionInfo(script, line_position + column, Script::NO_OFFSET,
                           isolate);
}

// Slow traversal over all scripts on the heap.
bool GetScriptById(Isolate* isolate, int needle, Handle<Script>* result) {
  Script::Iterator iterator(isolate);
  for (Script script = iterator.Next(); !script.is_null();
       script = iterator.Next()) {
    if (script.id() == needle) {
      *result = handle(script, isolate);
      return true;
    }
  }

  return false;
}

}  // namespace

// TODO(5530): Rename once conflicting function has been deleted.
RUNTIME_FUNCTION(Runtime_ScriptLocationFromLine2) {
  HandleScope scope(isolate);
  DCHECK_EQ(4, args.length());
  CONVERT_NUMBER_CHECKED(int32_t, scriptid, Int32, args[0]);
  CONVERT_ARG_HANDLE_CHECKED(Object, opt_line, 1);
  CONVERT_ARG_HANDLE_CHECKED(Object, opt_column, 2);
  CONVERT_NUMBER_CHECKED(int32_t, offset, Int32, args[3]);

  Handle<Script> script;
  CHECK(GetScriptById(isolate, scriptid, &script));

  return *ScriptLocationFromLine(isolate, script, opt_line, opt_column, offset);
}

// On function call, depending on circumstances, prepare for stepping in,
// or perform a side effect check.
RUNTIME_FUNCTION(Runtime_DebugOnFunctionCall) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, fun, 0);
  CONVERT_ARG_HANDLE_CHECKED(Object, receiver, 1);
  if (isolate->debug()->needs_check_on_function_call()) {
    // Ensure that the callee will perform debug check on function call too.
    Deoptimizer::DeoptimizeFunction(*fun);
    if (isolate->debug()->last_step_action() >= StepIn ||
        isolate->debug()->break_on_next_function_call()) {
      DCHECK_EQ(isolate->debug_execution_mode(), DebugInfo::kBreakpoints);
      isolate->debug()->PrepareStepIn(fun);
    }
    if (isolate->debug_execution_mode() == DebugInfo::kSideEffects &&
        !isolate->debug()->PerformSideEffectCheck(fun, receiver)) {
      return ReadOnlyRoots(isolate).exception();
    }
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

// Set one shot breakpoints for the suspended generator object.
RUNTIME_FUNCTION(Runtime_DebugPrepareStepInSuspendedGenerator) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  isolate->debug()->PrepareStepInSuspendedGenerator();
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugPushPromise) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  CONVERT_ARG_HANDLE_CHECKED(JSObject, promise, 0);
  isolate->PushPromise(promise);
  return ReadOnlyRoots(isolate).undefined_value();
}


RUNTIME_FUNCTION(Runtime_DebugPopPromise) {
  DCHECK_EQ(0, args.length());
  SealHandleScope shs(isolate);
  isolate->PopPromise();
  return ReadOnlyRoots(isolate).undefined_value();
}

namespace {
Handle<JSObject> MakeRangeObject(Isolate* isolate, const CoverageBlock& range) {
  Factory* factory = isolate->factory();

  Handle<String> start_string = factory->InternalizeUtf8String("start");
  Handle<String> end_string = factory->InternalizeUtf8String("end");
  Handle<String> count_string = factory->InternalizeUtf8String("count");

  Handle<JSObject> range_obj = factory->NewJSObjectWithNullProto();
  JSObject::AddProperty(isolate, range_obj, start_string,
                        factory->NewNumberFromInt(range.start), NONE);
  JSObject::AddProperty(isolate, range_obj, end_string,
                        factory->NewNumberFromInt(range.end), NONE);
  JSObject::AddProperty(isolate, range_obj, count_string,
                        factory->NewNumberFromUint(range.count), NONE);

  return range_obj;
}
}  // namespace

RUNTIME_FUNCTION(Runtime_DebugCollectCoverage) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());
  // Collect coverage data.
  std::unique_ptr<Coverage> coverage;
  if (isolate->is_best_effort_code_coverage()) {
    coverage = Coverage::CollectBestEffort(isolate);
  } else {
    coverage = Coverage::CollectPrecise(isolate);
  }
  Factory* factory = isolate->factory();
  // Turn the returned data structure into JavaScript.
  // Create an array of scripts.
  int num_scripts = static_cast<int>(coverage->size());
  // Prepare property keys.
  Handle<FixedArray> scripts_array = factory->NewFixedArray(num_scripts);
  Handle<String> script_string = factory->script_string();
  for (int i = 0; i < num_scripts; i++) {
    const auto& script_data = coverage->at(i);
    HandleScope inner_scope(isolate);

    std::vector<CoverageBlock> ranges;
    int num_functions = static_cast<int>(script_data.functions.size());
    for (int j = 0; j < num_functions; j++) {
      const auto& function_data = script_data.functions[j];
      ranges.emplace_back(function_data.start, function_data.end,
                          function_data.count);
      for (size_t k = 0; k < function_data.blocks.size(); k++) {
        const auto& block_data = function_data.blocks[k];
        ranges.emplace_back(block_data.start, block_data.end, block_data.count);
      }
    }

    int num_ranges = static_cast<int>(ranges.size());
    Handle<FixedArray> ranges_array = factory->NewFixedArray(num_ranges);
    for (int j = 0; j < num_ranges; j++) {
      Handle<JSObject> range_object = MakeRangeObject(isolate, ranges[j]);
      ranges_array->set(j, *range_object);
    }

    Handle<JSArray> script_obj =
        factory->NewJSArrayWithElements(ranges_array, PACKED_ELEMENTS);
    JSObject::AddProperty(isolate, script_obj, script_string,
                          handle(script_data.script->source(), isolate), NONE);
    scripts_array->set(i, *script_obj);
  }
  return *factory->NewJSArrayWithElements(scripts_array, PACKED_ELEMENTS);
}

RUNTIME_FUNCTION(Runtime_DebugTogglePreciseCoverage) {
  SealHandleScope shs(isolate);
  CONVERT_BOOLEAN_ARG_CHECKED(enable, 0);
  Coverage::SelectMode(isolate, enable ? debug::CoverageMode::kPreciseCount
                                       : debug::CoverageMode::kBestEffort);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugToggleBlockCoverage) {
  SealHandleScope shs(isolate);
  CONVERT_BOOLEAN_ARG_CHECKED(enable, 0);
  Coverage::SelectMode(isolate, enable ? debug::CoverageMode::kBlockCount
                                       : debug::CoverageMode::kBestEffort);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_IncBlockCounter) {
  UNREACHABLE();  // Never called. See the IncBlockCounter builtin instead.
}

RUNTIME_FUNCTION(Runtime_DebugAsyncFunctionEntered) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  CONVERT_ARG_HANDLE_CHECKED(JSPromise, promise, 0);
  isolate->RunPromiseHook(PromiseHookType::kInit, promise,
                          isolate->factory()->undefined_value());
  if (isolate->debug()->is_active()) isolate->PushPromise(promise);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugAsyncFunctionSuspended) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  CONVERT_ARG_HANDLE_CHECKED(JSPromise, promise, 0);
  isolate->PopPromise();
  isolate->OnAsyncFunctionStateChanged(promise, debug::kAsyncFunctionSuspended);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugAsyncFunctionResumed) {
  DCHECK_EQ(1, args.length());
  HandleScope scope(isolate);
  CONVERT_ARG_HANDLE_CHECKED(JSPromise, promise, 0);
  isolate->PushPromise(promise);
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_DebugAsyncFunctionFinished) {
  DCHECK_EQ(2, args.length());
  HandleScope scope(isolate);
  CONVERT_BOOLEAN_ARG_CHECKED(has_suspend, 0);
  CONVERT_ARG_HANDLE_CHECKED(JSPromise, promise, 1);
  isolate->PopPromise();
  if (has_suspend) {
    isolate->OnAsyncFunctionStateChanged(promise,
                                         debug::kAsyncFunctionFinished);
  }
  return *promise;
}

RUNTIME_FUNCTION(Runtime_LiveEditPatchScript) {
  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, script_function, 0);
  CONVERT_ARG_HANDLE_CHECKED(String, new_source, 1);

  Handle<Script> script(Script::cast(script_function->shared().script()),
                        isolate);
  v8::debug::LiveEditResult result;
  LiveEdit::PatchScript(isolate, script, new_source, false, &result);
  switch (result.status) {
    case v8::debug::LiveEditResult::COMPILE_ERROR:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: COMPILE_ERROR"));
    case v8::debug::LiveEditResult::BLOCKED_BY_RUNNING_GENERATOR:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_RUNNING_GENERATOR"));
    case v8::debug::LiveEditResult::BLOCKED_BY_FUNCTION_ABOVE_BREAK_FRAME:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_FUNCTION_ABOVE_BREAK_FRAME"));
    case v8::debug::LiveEditResult::
        BLOCKED_BY_FUNCTION_BELOW_NON_DROPPABLE_FRAME:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_FUNCTION_BELOW_NON_DROPPABLE_FRAME"));
    case v8::debug::LiveEditResult::BLOCKED_BY_ACTIVE_FUNCTION:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_ACTIVE_FUNCTION"));
    case v8::debug::LiveEditResult::BLOCKED_BY_NEW_TARGET_IN_RESTART_FRAME:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: BLOCKED_BY_NEW_TARGET_IN_RESTART_FRAME"));
    case v8::debug::LiveEditResult::FRAME_RESTART_IS_NOT_SUPPORTED:
      return isolate->Throw(*isolate->factory()->NewStringFromAsciiChecked(
          "LiveEdit failed: FRAME_RESTART_IS_NOT_SUPPORTED"));
    case v8::debug::LiveEditResult::OK:
      return ReadOnlyRoots(isolate).undefined_value();
  }
  return ReadOnlyRoots(isolate).undefined_value();
}

RUNTIME_FUNCTION(Runtime_ProfileCreateSnapshotDataBlob) {
  HandleScope scope(isolate);
  DCHECK_EQ(0, args.length());

  // Used only by the test/memory/Memory.json benchmark. This creates a snapshot
  // blob and outputs various statistics around it.

  DCHECK(FLAG_profile_deserialization);

  DisableEmbeddedBlobRefcounting();

  v8::StartupData blob = CreateSnapshotDataBlobInternal(
      v8::SnapshotCreator::FunctionCodeHandling::kClear, nullptr);
  delete[] blob.data;

  // Track the embedded blob size as well.
  {
    i::EmbeddedData d = i::EmbeddedData::FromBlob();
    PrintF("Embedded blob is %d bytes\n",
           static_cast<int>(d.code_size() + d.metadata_size()));
  }

  FreeCurrentEmbeddedBlob();

  return ReadOnlyRoots(isolate).undefined_value();
}

extern uint64_t* gProgressCounter;
extern bool gRecordReplayAssertValues;

// Define this to check preconditions for using record/replay opcodes.
//#define RECORD_REPLAY_CHECK_OPCODES

#ifdef RECORD_REPLAY_CHECK_OPCODES

extern bool RecordReplayIgnoreScript(Script script);

extern "C" bool V8RecordReplayHasDivergedFromRecording();

static inline bool RecordReplayBytecodeAllowed() {
  return IsMainThread()
      && (!recordreplay::AreEventsDisallowed() || V8RecordReplayHasDivergedFromRecording());
}

#else // !RECORD_REPLAY_CHECK_OPCODES

static inline bool RecordReplayIgnoreScript(Script script) {
  return false;
}

static inline bool RecordReplayBytecodeAllowed() {
  return true;
}

#endif // !RECORD_REPLAY_CHECK_OPCODES

extern bool gRecordReplayHasCheckpoint;

RUNTIME_FUNCTION(Runtime_RecordReplayAssertExecutionProgress) {
  ++*gProgressCounter;

  if (!gRecordReplayAssertValues) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  HandleScope scope(isolate);
  DCHECK_EQ(1, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);

  Handle<SharedFunctionInfo> shared(function->shared(), isolate);
  Handle<Script> script(Script::cast(shared->script()), isolate);
  CHECK(!RecordReplayIgnoreScript(*script));

  Script::PositionInfo info;
  Script::GetPositionInfo(script, shared->StartPosition(), &info, Script::WITH_OFFSET);

  std::string name;
  if (script->name().IsUndefined()) {
    name = "<none>";
  } else {
    std::unique_ptr<char[]> name_raw = String::cast(script->name()).ToCString();
    name = name_raw.get();
  }

  if (!RecordReplayBytecodeAllowed()) {
    recordreplay::Diagnostic("RecordReplayAssertExecutionProgress not allowed %s:%d:%d",
                             name.c_str(), info.line + 1, info.column);
  }
  CHECK(RecordReplayBytecodeAllowed());

  if (!gRecordReplayHasCheckpoint) {
    recordreplay::Diagnostic("ExecutionProgress before first checkpoint %s:%d:%d",
                             name.c_str(), info.line + 1, info.column);
    CHECK(gRecordReplayHasCheckpoint);
  }

  recordreplay::Assert("ExecutionProgress %s:%d:%d",
                       name.c_str(), info.line + 1, info.column);

  return ReadOnlyRoots(isolate).undefined_value();
}

static std::string GetStackLocation(Isolate* isolate) {
  char location[1024];
  strcpy(location, "<no frame>");
  for (StackFrameIterator it(isolate); !it.done(); it.Advance()) {
    StackFrame* frame = it.frame();
    if (frame->type() != StackFrame::OPTIMIZED && frame->type() != StackFrame::INTERPRETED) {
      continue;
    }
    std::vector<FrameSummary> frames;
    StandardFrame::cast(frame)->Summarize(&frames);
    auto& summary = frames.back();
    CHECK(summary.IsJavaScript());
    auto const& js = summary.AsJavaScript();

    Handle<SharedFunctionInfo> shared(js.function()->shared(), isolate);

    // Sometimes the SharedFunctionInfo has what appears to be a bogus
    // script for an unknown reason. We check the positions of the function
    // to watch for this.
    if (!shared->StartPosition() && !shared->EndPosition()) {
      continue;
    }

    Handle<Script> script(Script::cast(shared->script()), isolate);

    if (script->id() == 0) {
      continue;
    }

    int source_position = js.SourcePosition();
    Script::PositionInfo info;
    Script::GetPositionInfo(script, source_position, &info, Script::WITH_OFFSET);

    if (script->name().IsUndefined()) {
      snprintf(location, sizeof(location), "<none>:%d:%d", info.line + 1, info.column);
    } else {
      std::unique_ptr<char[]> name = String::cast(script->name()).ToCString();
      snprintf(location, sizeof(location), "%s:%d:%d", name.get(), info.line + 1, info.column);
    }
    location[sizeof(location) - 1] = 0;
    break;
  }

  return std::string(location);
}

void RecordReplayAssertScriptedCaller(Isolate* isolate, const char* aWhy) {
  if (recordreplay::IsRecordingOrReplaying()) {
    std::string location = GetStackLocation(isolate);
    recordreplay::Assert("ScriptedCaller %s %s", aWhy, location.c_str());
  }
}

// Assertion and instrumentation site indexes embedded in bytecodes are offset
// by this value. This forces the bytecode emitter to always use four bytes to
// encode the index, so that bytecode offsets will be stable between recording
// and replaying (or different replays) even if the indexes themselves aren't.
static const int BytecodeSiteOffset = 1 << 16;

// Locations for each assertion site, filled in lazily.
struct AssertionSite {
  std::string desc_;
  int source_position_;
  std::string location_;
};
typedef std::vector<AssertionSite> AssertionSiteVector;
static AssertionSiteVector* gAssertionSites;

int RegisterAssertValueSite(const std::string& desc, int source_position) {
  CHECK(IsMainThread());
  if (!gAssertionSites) {
    gAssertionSites = new AssertionSiteVector();
  }
  int index = (int)gAssertionSites->size();
  gAssertionSites->push_back({ desc, source_position, "" });
  return index + BytecodeSiteOffset;
}

extern std::string RecordReplayBasicValueContents(Handle<Object> value);

RUNTIME_FUNCTION(Runtime_RecordReplayAssertValue) {
  CHECK(gRecordReplayAssertValues);
  CHECK(RecordReplayBytecodeAllowed());

  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  CONVERT_NUMBER_CHECKED(int32_t, index, Int32, args[1]);
  CONVERT_ARG_HANDLE_CHECKED(Object, value, 2);

  Handle<Script> script(Script::cast(function->shared().script()), isolate);
  CHECK(!RecordReplayIgnoreScript(*script));

  index -= BytecodeSiteOffset;
  CHECK(gAssertionSites && (size_t)index < gAssertionSites->size());
  AssertionSite& site = (*gAssertionSites)[index];

  if (!site.location_.length()) {
    Script::PositionInfo info;
    Script::GetPositionInfo(script, site.source_position_, &info, Script::WITH_OFFSET);

    char buf[1024];
    if (script->name().IsUndefined()) {
      snprintf(buf, sizeof(buf), "<none>:%d:%d", info.line + 1, info.column);
    } else {
      std::unique_ptr<char[]> name = String::cast(script->name()).ToCString();
      snprintf(buf, sizeof(buf), "%s:%d:%d", name.get(), info.line + 1, info.column);
    }
    buf[sizeof(buf) - 1] = 0;

    site.location_ = buf;
  }

  std::string contents = RecordReplayBasicValueContents(value);

  recordreplay::Assert("%s %s Value %s", site.location_.c_str(),
                       site.desc_.c_str(),contents.c_str());
  return *value;
}

struct InstrumentationSite {
  const char* kind_ = nullptr;
  int source_position_ = 0;
  int bytecode_offset_ = 0;

  // Set on the first use of the instrumentation site.
  std::string function_id_;
};

// Main thread only.
typedef std::vector<InstrumentationSite> InstrumentationSiteVector;
static InstrumentationSiteVector* gInstrumentationSites;

int RegisterInstrumentationSite(const char* kind, int source_position,
                                int bytecode_offset) {
  CHECK(IsMainThread());
  InstrumentationSite site;
  site.kind_ = kind;
  site.source_position_ = source_position;
  site.bytecode_offset_ = bytecode_offset;
  if (!gInstrumentationSites) {
    gInstrumentationSites = new InstrumentationSiteVector();
  }
  int index = (int)gInstrumentationSites->size();
  gInstrumentationSites->push_back(site);
  return index + BytecodeSiteOffset;
}

static InstrumentationSite& GetInstrumentationSite(const char* why, int index) {
  CHECK(IsMainThread());
  CHECK(gInstrumentationSites);
  index -= BytecodeSiteOffset;
  if ((size_t)index >= gInstrumentationSites->size()) {
    recordreplay::Diagnostic("BadInstrumentationSite %s %d %d",
                             why, index, gInstrumentationSites->size());
  }
  CHECK((size_t)index < gInstrumentationSites->size());
  return (*gInstrumentationSites)[index];
}

const char* InstrumentationSiteKind(int index) {
  return GetInstrumentationSite("Kind", index).kind_;
}

int InstrumentationSiteSourcePosition(int index) {
  return GetInstrumentationSite("SourcePosition", index).source_position_;
}

int InstrumentationSiteBytecodeOffset(int index) {
  return GetInstrumentationSite("BytecodeOffset", index).bytecode_offset_;
}

extern void RecordReplayInstrument(const char* kind, const char* function, int offset);

// Enable to dump locations of each function to stderr.
static bool gDumpFunctionLocations;

std::string GetRecordReplayFunctionId(Handle<SharedFunctionInfo> shared) {
  Script script = Script::cast(shared->script());

  std::ostringstream os;

  // When recording/replaying we use a function ID we can parse to a script
  // and source location later.
  os << script.id() << ":" << shared->StartPosition();

  if (gDumpFunctionLocations) {
    std::unique_ptr<char[]> url;
    if (!script.name().IsUndefined()) {
      url = String::cast(script.name()).ToCString();
    }

    Script::PositionInfo info;
    Handle<Script> handleScript(script, Isolate::Current());
    Script::GetPositionInfo(handleScript, shared->StartPosition(), &info, Script::WITH_OFFSET);
    recordreplay::Print("FunctionId %s -> %s:%d:%d",
                        os.str().c_str(), url.get() ? url.get() : "<none>",
                        info.line + 1, info.column);
  }

  return os.str();
}

void ParseRecordReplayFunctionId(const std::string& function_id,
                                 int* script_id, int* source_position) {
  const char* raw = function_id.c_str();
  *script_id = atoi(raw);
  *source_position = atoi(strchr(raw, ':') + 1);
}

static inline void OnInstrumentation(Isolate* isolate,
                                     Handle<JSFunction> function, int32_t index) {
  CHECK(RecordReplayBytecodeAllowed());

  Handle<Script> script(Script::cast(function->shared().script()), isolate);
  CHECK(!RecordReplayIgnoreScript(*script));

  InstrumentationSite& site = GetInstrumentationSite("Callback", index);

  if (!site.function_id_.length()) {
    Handle<SharedFunctionInfo> shared(function->shared(), isolate);
    site.function_id_ = GetRecordReplayFunctionId(shared);
  }

  RecordReplayInstrument(site.kind_, site.function_id_.c_str(),
                         site.bytecode_offset_);
}

extern bool gRecordReplayInstrumentationEnabled;

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentation) {
  if (!gRecordReplayInstrumentationEnabled) {
    return ReadOnlyRoots(isolate).undefined_value();
  }

  HandleScope scope(isolate);
  DCHECK_EQ(2, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  CONVERT_NUMBER_CHECKED(int32_t, index, Int32, args[1]);

  OnInstrumentation(isolate, function, index);

  return ReadOnlyRoots(isolate).undefined_value();
}

extern int RecordReplayObjectId(Handle<Object> internal_object);

static int gCurrentGeneratorId;

int RecordReplayCurrentGeneratorIdRaw() {
  return gCurrentGeneratorId;
}

RUNTIME_FUNCTION(Runtime_RecordReplayInstrumentationGenerator) {
  HandleScope scope(isolate);
  DCHECK_EQ(3, args.length());
  CONVERT_ARG_HANDLE_CHECKED(JSFunction, function, 0);
  CONVERT_NUMBER_CHECKED(int32_t, index, Int32, args[1]);
  CONVERT_ARG_HANDLE_CHECKED(JSGeneratorObject, generator_object, 2);

  // Note: RecordReplayObjectId calls have to occur in the same places when
  // replaying as when recording (regardless of whether instrumentation is
  // enabled) so that objects will be assigned consistent IDs.
  CHECK(!gCurrentGeneratorId);
  gCurrentGeneratorId = RecordReplayObjectId(generator_object);

  if (gRecordReplayInstrumentationEnabled) {
    OnInstrumentation(isolate, function, index);
  }

  gCurrentGeneratorId = 0;

  return ReadOnlyRoots(isolate).undefined_value();
}

}  // namespace internal
}  // namespace v8
