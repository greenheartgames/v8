// Copyright 2012 the V8 project authors. All rights reserved.
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

#include "api.h"
#include "arguments.h"
#include "ast.h"
#include "code-stubs.h"
#include "gdb-jit.h"
#include "ic-inl.h"
#include "stub-cache.h"
#include "vm-state-inl.h"

namespace v8 {
namespace internal {

// -----------------------------------------------------------------------
// StubCache implementation.


StubCache::StubCache(Isolate* isolate, Zone* zone)
    : isolate_(isolate) {
  ASSERT(isolate == Isolate::Current());
}


void StubCache::Initialize() {
  ASSERT(IsPowerOf2(kPrimaryTableSize));
  ASSERT(IsPowerOf2(kSecondaryTableSize));
  Clear();
}


Code* StubCache::Set(String* name, Map* map, Code* code) {
  // Get the flags from the code.
  Code::Flags flags = Code::RemoveTypeFromFlags(code->flags());

  // Validate that the name does not move on scavenge, and that we
  // can use identity checks instead of string equality checks.
  ASSERT(!heap()->InNewSpace(name));
  ASSERT(name->IsSymbol());

  // The state bits are not important to the hash function because
  // the stub cache only contains monomorphic stubs. Make sure that
  // the bits are the least significant so they will be the ones
  // masked out.
  ASSERT(Code::ExtractICStateFromFlags(flags) == MONOMORPHIC);
  STATIC_ASSERT((Code::ICStateField::kMask & 1) == 1);

  // Make sure that the code type is not included in the hash.
  ASSERT(Code::ExtractTypeFromFlags(flags) == 0);

  // Compute the primary entry.
  int primary_offset = PrimaryOffset(name, flags, map);
  Entry* primary = entry(primary_, primary_offset);
  Code* old_code = primary->value;

  // If the primary entry has useful data in it, we retire it to the
  // secondary cache before overwriting it.
  if (old_code != isolate_->builtins()->builtin(Builtins::kIllegal)) {
    Map* old_map = primary->map;
    Code::Flags old_flags = Code::RemoveTypeFromFlags(old_code->flags());
    int seed = PrimaryOffset(primary->key, old_flags, old_map);
    int secondary_offset = SecondaryOffset(primary->key, old_flags, seed);
    Entry* secondary = entry(secondary_, secondary_offset);
    *secondary = *primary;
  }

  // Update primary cache.
  primary->key = name;
  primary->value = code;
  primary->map = map;
  isolate()->counters()->megamorphic_stub_cache_updates()->Increment();
  return code;
}


Handle<Code> StubCache::ComputeLoadNonexistent(Handle<String> name,
                                               Handle<JSObject> receiver) {
  // If no global objects are present in the prototype chain, the load
  // nonexistent IC stub can be shared for all names for a given map
  // and we use the empty string for the map cache in that case.  If
  // there are global objects involved, we need to check global
  // property cells in the stub and therefore the stub will be
  // specific to the name.
  Handle<String> cache_name = factory()->empty_string();
  Handle<JSObject> current;
  Handle<Object> next = receiver;
  Handle<GlobalObject> global;
  do {
    current = Handle<JSObject>::cast(next);
    next = Handle<Object>(current->GetPrototype(), isolate_);
    if (current->IsGlobalObject()) {
      global = Handle<GlobalObject>::cast(current);
      cache_name = name;
    } else if (!current->HasFastProperties()) {
      cache_name = name;
    }
  } while (!next->IsNull());

  // Compile the stub that is either shared for all names or
  // name specific if there are global objects involved.
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::NONEXISTENT);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*cache_name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadNonexistent(receiver, current, cache_name, global);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *cache_name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *cache_name, *code));
  JSObject::UpdateMapCodeCache(receiver, cache_name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadField(Handle<String> name,
                                         Handle<JSObject> receiver,
                                         Handle<JSObject> holder,
                                         PropertyIndex field) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::FIELD);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileLoadField(receiver, holder, name, field);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadCallback(
    Handle<String> name,
    Handle<JSObject> receiver,
    Handle<JSObject> holder,
    Handle<ExecutableAccessorInfo> callback) {
  ASSERT(v8::ToCData<Address>(callback->getter()) != 0);
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::CALLBACKS);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadCallback(receiver, holder, name, callback);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadViaGetter(Handle<String> name,
                                             Handle<JSObject> receiver,
                                             Handle<JSObject> holder,
                                             Handle<JSFunction> getter) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::CALLBACKS);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadViaGetter(receiver, holder, name, getter);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadConstant(Handle<String> name,
                                            Handle<JSObject> receiver,
                                            Handle<JSObject> holder,
                                            Handle<JSFunction> value) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::CONSTANT_FUNCTION);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
        compiler.CompileLoadConstant(receiver, holder, name, value);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadInterceptor(Handle<String> name,
                                               Handle<JSObject> receiver,
                                               Handle<JSObject> holder) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, Code::INTERCEPTOR);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
        compiler.CompileLoadInterceptor(receiver, holder, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadNormal() {
  return isolate_->builtins()->LoadIC_Normal();
}


Handle<Code> StubCache::ComputeLoadGlobal(Handle<String> name,
                                          Handle<JSObject> receiver,
                                          Handle<GlobalObject> holder,
                                          Handle<JSGlobalPropertyCell> cell,
                                          bool is_dont_delete) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(Code::LOAD_IC);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  LoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadGlobal(receiver, holder, cell, name, is_dont_delete);
  PROFILE(isolate_, CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedLoadField(Handle<String> name,
                                              Handle<JSObject> receiver,
                                              Handle<JSObject> holder,
                                              PropertyIndex field) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_LOAD_IC, Code::kNoExtraICState, Code::FIELD);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileLoadField(receiver, holder, name, field);
  PROFILE(isolate_, CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedLoadConstant(Handle<String> name,
                                                 Handle<JSObject> receiver,
                                                 Handle<JSObject> holder,
                                                 Handle<JSFunction> value) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_LOAD_IC, Code::kNoExtraICState, Code::CONSTANT_FUNCTION);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadConstant(receiver, holder, name, value);
  PROFILE(isolate_, CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedLoadInterceptor(Handle<String> name,
                                                    Handle<JSObject> receiver,
                                                    Handle<JSObject> holder) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_LOAD_IC, Code::kNoExtraICState, Code::INTERCEPTOR);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileLoadInterceptor(receiver, holder, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedLoadCallback(
    Handle<String> name,
    Handle<JSObject> receiver,
    Handle<JSObject> holder,
    Handle<ExecutableAccessorInfo> callback) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_LOAD_IC, Code::kNoExtraICState, Code::CALLBACKS);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate_);
  Handle<Code> code =
      compiler.CompileLoadCallback(receiver, holder, name, callback);
  PROFILE(isolate_, CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_LOAD_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreField(Handle<String> name,
                                          Handle<JSObject> receiver,
                                          int field_index,
                                          Handle<Map> transition,
                                          StrictModeFlag strict_mode) {
  Code::StubType type =
      (transition.is_null()) ? Code::FIELD : Code::MAP_TRANSITION;
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode, type);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  StoreStubCompiler compiler(isolate_, strict_mode);
  Handle<Code> code =
      compiler.CompileStoreField(receiver, field_index, transition, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedLoadElement(Handle<Map> receiver_map) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(Code::KEYED_LOAD_IC);
  Handle<String> name =
      isolate()->factory()->KeyedLoadElementMonomorphic_symbol();

  Handle<Object> probe(receiver_map->FindInCodeCache(*name, flags), isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate());
  Handle<Code> code = compiler.CompileLoadElement(receiver_map);

  PROFILE(isolate(), CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, 0));
  Map::UpdateCodeCache(receiver_map, name, code);
  return code;
}


Handle<Code> StubCache::ComputeKeyedStoreElement(
    Handle<Map> receiver_map,
    KeyedStoreIC::StubKind stub_kind,
    StrictModeFlag strict_mode,
    KeyedAccessGrowMode grow_mode) {
  Code::ExtraICState extra_state =
      Code::ComputeExtraICState(grow_mode, strict_mode);
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_STORE_IC, extra_state);

  ASSERT(stub_kind == KeyedStoreIC::STORE_NO_TRANSITION ||
         stub_kind == KeyedStoreIC::STORE_AND_GROW_NO_TRANSITION);

  Handle<String> name = stub_kind == KeyedStoreIC::STORE_NO_TRANSITION
      ? isolate()->factory()->KeyedStoreElementMonomorphic_symbol()
      : isolate()->factory()->KeyedStoreAndGrowElementMonomorphic_symbol();

  Handle<Object> probe(receiver_map->FindInCodeCache(*name, flags), isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedStoreStubCompiler compiler(isolate(), strict_mode, grow_mode);
  Handle<Code> code = compiler.CompileStoreElement(receiver_map);

  PROFILE(isolate(), CodeCreateEvent(Logger::KEYED_STORE_IC_TAG, *code, 0));
  Map::UpdateCodeCache(receiver_map, name, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreNormal(StrictModeFlag strict_mode) {
  return (strict_mode == kStrictMode)
      ? isolate_->builtins()->Builtins::StoreIC_Normal_Strict()
      : isolate_->builtins()->Builtins::StoreIC_Normal();
}


Handle<Code> StubCache::ComputeStoreGlobal(Handle<String> name,
                                           Handle<GlobalObject> receiver,
                                           Handle<JSGlobalPropertyCell> cell,
                                           StrictModeFlag strict_mode) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  StoreStubCompiler compiler(isolate_, strict_mode);
  Handle<Code> code = compiler.CompileStoreGlobal(receiver, cell, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreCallback(
    Handle<String> name,
    Handle<JSObject> receiver,
    Handle<JSObject> holder,
    Handle<ExecutableAccessorInfo> callback,
    StrictModeFlag strict_mode) {
  ASSERT(v8::ToCData<Address>(callback->setter()) != 0);
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode, Code::CALLBACKS);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  StoreStubCompiler compiler(isolate_, strict_mode);
  Handle<Code> code =
      compiler.CompileStoreCallback(name, receiver, holder, callback);
  PROFILE(isolate_, CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreViaSetter(Handle<String> name,
                                              Handle<JSObject> receiver,
                                              Handle<JSObject> holder,
                                              Handle<JSFunction> setter,
                                              StrictModeFlag strict_mode) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode, Code::CALLBACKS);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  StoreStubCompiler compiler(isolate_, strict_mode);
  Handle<Code> code =
      compiler.CompileStoreViaSetter(name, receiver, holder, setter);
  PROFILE(isolate_, CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreInterceptor(Handle<String> name,
                                                Handle<JSObject> receiver,
                                                StrictModeFlag strict_mode) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode, Code::INTERCEPTOR);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  StoreStubCompiler compiler(isolate_, strict_mode);
  Handle<Code> code = compiler.CompileStoreInterceptor(receiver, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}

Handle<Code> StubCache::ComputeKeyedStoreField(Handle<String> name,
                                               Handle<JSObject> receiver,
                                               int field_index,
                                               Handle<Map> transition,
                                               StrictModeFlag strict_mode) {
  Code::StubType type =
      (transition.is_null()) ? Code::FIELD : Code::MAP_TRANSITION;
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::KEYED_STORE_IC, strict_mode, type);
  Handle<Object> probe(receiver->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedStoreStubCompiler compiler(isolate(), strict_mode,
                                  DO_NOT_ALLOW_JSARRAY_GROWTH);
  Handle<Code> code =
      compiler.CompileStoreField(receiver, field_index, transition, name);
  PROFILE(isolate_, CodeCreateEvent(Logger::KEYED_STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_STORE_IC, *name, *code));
  JSObject::UpdateMapCodeCache(receiver, name, code);
  return code;
}


#define CALL_LOGGER_TAG(kind, type) \
    (kind == Code::CALL_IC ? Logger::type : Logger::KEYED_##type)

Handle<Code> StubCache::ComputeCallConstant(int argc,
                                            Code::Kind kind,
                                            Code::ExtraICState extra_state,
                                            Handle<String> name,
                                            Handle<Object> object,
                                            Handle<JSObject> holder,
                                            Handle<JSFunction> function) {
  // Compute the check type and the map.
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*object, *holder);
  Handle<JSObject> map_holder(
      IC::GetCodeCacheHolder(isolate_, *object, cache_holder));

  // Compute check type based on receiver/holder.
  CheckType check = RECEIVER_MAP_CHECK;
  if (object->IsString()) {
    check = STRING_CHECK;
  } else if (object->IsNumber()) {
    check = NUMBER_CHECK;
  } else if (object->IsBoolean()) {
    check = BOOLEAN_CHECK;
  }

  if (check != RECEIVER_MAP_CHECK &&
      !function->IsBuiltin() &&
      function->shared()->is_classic_mode()) {
    // Calling non-strict non-builtins with a value as the receiver
    // requires boxing.
    return Handle<Code>::null();
  }

  Code::Flags flags = Code::ComputeMonomorphicFlags(
      kind, extra_state, Code::CONSTANT_FUNCTION, argc, cache_holder);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  CallStubCompiler compiler(isolate_, argc, kind, extra_state, cache_holder);
  Handle<Code> code =
      compiler.CompileCallConstant(object, holder, name, check, function);
  code->set_check_type(check);
  ASSERT_EQ(flags, code->flags());
  PROFILE(isolate_,
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_IC_TAG), *code, *name));
  GDBJIT(AddCode(GDBJITInterface::CALL_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeCallField(int argc,
                                         Code::Kind kind,
                                         Code::ExtraICState extra_state,
                                         Handle<String> name,
                                         Handle<Object> object,
                                         Handle<JSObject> holder,
                                         PropertyIndex index) {
  // Compute the check type and the map.
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*object, *holder);
  Handle<JSObject> map_holder(
      IC::GetCodeCacheHolder(isolate_, *object, cache_holder));

  // TODO(1233596): We cannot do receiver map check for non-JS objects
  // because they may be represented as immediates without a
  // map. Instead, we check against the map in the holder.
  if (object->IsNumber() || object->IsBoolean() || object->IsString()) {
    object = holder;
  }

  Code::Flags flags = Code::ComputeMonomorphicFlags(
      kind, extra_state, Code::FIELD, argc, cache_holder);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  CallStubCompiler compiler(isolate_, argc, kind, extra_state, cache_holder);
  Handle<Code> code =
      compiler.CompileCallField(Handle<JSObject>::cast(object),
                                holder, index, name);
  ASSERT_EQ(flags, code->flags());
  PROFILE(isolate_,
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_IC_TAG), *code, *name));
  GDBJIT(AddCode(GDBJITInterface::CALL_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeCallInterceptor(int argc,
                                               Code::Kind kind,
                                               Code::ExtraICState extra_state,
                                               Handle<String> name,
                                               Handle<Object> object,
                                               Handle<JSObject> holder) {
  // Compute the check type and the map.
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*object, *holder);
  Handle<JSObject> map_holder(
      IC::GetCodeCacheHolder(isolate_, *object, cache_holder));

  // TODO(1233596): We cannot do receiver map check for non-JS objects
  // because they may be represented as immediates without a
  // map. Instead, we check against the map in the holder.
  if (object->IsNumber() || object->IsBoolean() || object->IsString()) {
    object = holder;
  }

  Code::Flags flags = Code::ComputeMonomorphicFlags(
      kind, extra_state, Code::INTERCEPTOR, argc, cache_holder);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  CallStubCompiler compiler(isolate(), argc, kind, extra_state, cache_holder);
  Handle<Code> code =
      compiler.CompileCallInterceptor(Handle<JSObject>::cast(object),
                                      holder, name);
  ASSERT_EQ(flags, code->flags());
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_IC_TAG), *code, *name));
  GDBJIT(AddCode(GDBJITInterface::CALL_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


Handle<Code> StubCache::ComputeCallGlobal(int argc,
                                          Code::Kind kind,
                                          Code::ExtraICState extra_state,
                                          Handle<String> name,
                                          Handle<JSObject> receiver,
                                          Handle<GlobalObject> holder,
                                          Handle<JSGlobalPropertyCell> cell,
                                          Handle<JSFunction> function) {
  InlineCacheHolderFlag cache_holder =
      IC::GetCodeCacheForObject(*receiver, *holder);
  Handle<JSObject> map_holder(IC::GetCodeCacheHolder(
      isolate_, *receiver, cache_holder));
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      kind, extra_state, Code::NORMAL, argc, cache_holder);
  Handle<Object> probe(map_holder->map()->FindInCodeCache(*name, flags),
                       isolate_);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  CallStubCompiler compiler(isolate(), argc, kind, extra_state, cache_holder);
  Handle<Code> code =
      compiler.CompileCallGlobal(receiver, holder, cell, function, name);
  ASSERT_EQ(flags, code->flags());
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_IC_TAG), *code, *name));
  GDBJIT(AddCode(GDBJITInterface::CALL_IC, *name, *code));
  JSObject::UpdateMapCodeCache(map_holder, name, code);
  return code;
}


static void FillCache(Isolate* isolate, Handle<Code> code) {
  Handle<UnseededNumberDictionary> dictionary =
      UnseededNumberDictionary::Set(isolate->factory()->non_monomorphic_cache(),
                                    code->flags(),
                                    code);
  isolate->heap()->public_set_non_monomorphic_cache(*dictionary);
}


Code* StubCache::FindCallInitialize(int argc,
                                    RelocInfo::Mode mode,
                                    Code::Kind kind) {
  Code::ExtraICState extra_state =
      CallICBase::StringStubState::encode(DEFAULT_STRING_STUB) |
      CallICBase::Contextual::encode(mode == RelocInfo::CODE_TARGET_CONTEXT);
  Code::Flags flags =
      Code::ComputeFlags(kind, UNINITIALIZED, extra_state, Code::NORMAL, argc);
  UnseededNumberDictionary* dictionary =
      isolate()->heap()->non_monomorphic_cache();
  int entry = dictionary->FindEntry(isolate(), flags);
  ASSERT(entry != -1);
  Object* code = dictionary->ValueAt(entry);
  // This might be called during the marking phase of the collector
  // hence the unchecked cast.
  return reinterpret_cast<Code*>(code);
}


Handle<Code> StubCache::ComputeCallInitialize(int argc,
                                              RelocInfo::Mode mode,
                                              Code::Kind kind) {
  Code::ExtraICState extra_state =
      CallICBase::StringStubState::encode(DEFAULT_STRING_STUB) |
      CallICBase::Contextual::encode(mode == RelocInfo::CODE_TARGET_CONTEXT);
  Code::Flags flags =
      Code::ComputeFlags(kind, UNINITIALIZED, extra_state, Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallInitialize(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallInitialize(int argc, RelocInfo::Mode mode) {
  return ComputeCallInitialize(argc, mode, Code::CALL_IC);
}


Handle<Code> StubCache::ComputeKeyedCallInitialize(int argc) {
  return ComputeCallInitialize(argc, RelocInfo::CODE_TARGET,
                               Code::KEYED_CALL_IC);
}


Handle<Code> StubCache::ComputeCallPreMonomorphic(
    int argc,
    Code::Kind kind,
    Code::ExtraICState extra_state) {
  Code::Flags flags =
      Code::ComputeFlags(kind, PREMONOMORPHIC, extra_state, Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallPreMonomorphic(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallNormal(int argc,
                                          Code::Kind kind,
                                          Code::ExtraICState extra_state) {
  Code::Flags flags =
      Code::ComputeFlags(kind, MONOMORPHIC, extra_state, Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallNormal(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallArguments(int argc) {
  Code::Flags flags =
      Code::ComputeFlags(Code::KEYED_CALL_IC, MEGAMORPHIC,
                         Code::kNoExtraICState, Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallArguments(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallMegamorphic(
    int argc,
    Code::Kind kind,
    Code::ExtraICState extra_state) {
  Code::Flags flags =
      Code::ComputeFlags(kind, MEGAMORPHIC, extra_state,
                         Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallMegamorphic(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallMiss(int argc,
                                        Code::Kind kind,
                                        Code::ExtraICState extra_state) {
  // MONOMORPHIC_PROTOTYPE_FAILURE state is used to make sure that miss stubs
  // and monomorphic stubs are not mixed up together in the stub cache.
  Code::Flags flags =
      Code::ComputeFlags(kind, MONOMORPHIC_PROTOTYPE_FAILURE, extra_state,
                         Code::NORMAL, argc, OWN_MAP);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallMiss(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeLoadElementPolymorphic(
    MapHandleList* receiver_maps) {
  Code::Flags flags = Code::ComputeFlags(Code::KEYED_LOAD_IC, POLYMORPHIC);
  Handle<PolymorphicCodeCache> cache =
      isolate_->factory()->polymorphic_code_cache();
  Handle<Object> probe = cache->Lookup(receiver_maps, flags);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedLoadStubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileLoadElementPolymorphic(receiver_maps);
  PolymorphicCodeCache::Update(cache, receiver_maps, flags, code);
  return code;
}


Handle<Code> StubCache::ComputeStoreElementPolymorphic(
    MapHandleList* receiver_maps,
    KeyedAccessGrowMode grow_mode,
    StrictModeFlag strict_mode) {
  Handle<PolymorphicCodeCache> cache =
      isolate_->factory()->polymorphic_code_cache();
  Code::ExtraICState extra_state = Code::ComputeExtraICState(grow_mode,
                                                             strict_mode);
  Code::Flags flags =
      Code::ComputeFlags(Code::KEYED_STORE_IC, POLYMORPHIC, extra_state);
  Handle<Object> probe = cache->Lookup(receiver_maps, flags);
  if (probe->IsCode()) return Handle<Code>::cast(probe);

  KeyedStoreStubCompiler compiler(isolate_, strict_mode, grow_mode);
  Handle<Code> code = compiler.CompileStoreElementPolymorphic(receiver_maps);
  PolymorphicCodeCache::Update(cache, receiver_maps, flags, code);
  return code;
}


#ifdef ENABLE_DEBUGGER_SUPPORT
Handle<Code> StubCache::ComputeCallDebugBreak(int argc,
                                              Code::Kind kind) {
  // Extra IC state is irrelevant for debug break ICs. They jump to
  // the actual call ic to carry out the work.
  Code::Flags flags =
      Code::ComputeFlags(kind, DEBUG_STUB, DEBUG_BREAK,
                         Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallDebugBreak(flags);
  FillCache(isolate_, code);
  return code;
}


Handle<Code> StubCache::ComputeCallDebugPrepareStepIn(int argc,
                                                      Code::Kind kind) {
  // Extra IC state is irrelevant for debug break ICs. They jump to
  // the actual call ic to carry out the work.
  Code::Flags flags =
      Code::ComputeFlags(kind, DEBUG_STUB, DEBUG_PREPARE_STEP_IN,
                         Code::NORMAL, argc);
  Handle<UnseededNumberDictionary> cache =
      isolate_->factory()->non_monomorphic_cache();
  int entry = cache->FindEntry(isolate_, flags);
  if (entry != -1) return Handle<Code>(Code::cast(cache->ValueAt(entry)));

  StubCompiler compiler(isolate_);
  Handle<Code> code = compiler.CompileCallDebugPrepareStepIn(flags);
  FillCache(isolate_, code);
  return code;
}
#endif


void StubCache::Clear() {
  Code* empty = isolate_->builtins()->builtin(Builtins::kIllegal);
  for (int i = 0; i < kPrimaryTableSize; i++) {
    primary_[i].key = heap()->empty_string();
    primary_[i].value = empty;
  }
  for (int j = 0; j < kSecondaryTableSize; j++) {
    secondary_[j].key = heap()->empty_string();
    secondary_[j].value = empty;
  }
}


void StubCache::CollectMatchingMaps(SmallMapList* types,
                                    String* name,
                                    Code::Flags flags,
                                    Handle<Context> native_context,
                                    Zone* zone) {
  for (int i = 0; i < kPrimaryTableSize; i++) {
    if (primary_[i].key == name) {
      Map* map = primary_[i].value->FindFirstMap();
      // Map can be NULL, if the stub is constant function call
      // with a primitive receiver.
      if (map == NULL) continue;

      int offset = PrimaryOffset(name, flags, map);
      if (entry(primary_, offset) == &primary_[i] &&
          !TypeFeedbackOracle::CanRetainOtherContext(map, *native_context)) {
        types->Add(Handle<Map>(map), zone);
      }
    }
  }

  for (int i = 0; i < kSecondaryTableSize; i++) {
    if (secondary_[i].key == name) {
      Map* map = secondary_[i].value->FindFirstMap();
      // Map can be NULL, if the stub is constant function call
      // with a primitive receiver.
      if (map == NULL) continue;

      // Lookup in primary table and skip duplicates.
      int primary_offset = PrimaryOffset(name, flags, map);
      Entry* primary_entry = entry(primary_, primary_offset);
      if (primary_entry->key == name) {
        Map* primary_map = primary_entry->value->FindFirstMap();
        if (map == primary_map) continue;
      }

      // Lookup in secondary table and add matches.
      int offset = SecondaryOffset(name, flags, primary_offset);
      if (entry(secondary_, offset) == &secondary_[i] &&
          !TypeFeedbackOracle::CanRetainOtherContext(map, *native_context)) {
        types->Add(Handle<Map>(map), zone);
      }
    }
  }
}


// ------------------------------------------------------------------------
// StubCompiler implementation.


RUNTIME_FUNCTION(MaybeObject*, StoreCallbackProperty) {
  JSObject* recv = JSObject::cast(args[0]);
  ExecutableAccessorInfo* callback = ExecutableAccessorInfo::cast(args[1]);
  Address setter_address = v8::ToCData<Address>(callback->setter());
  v8::AccessorSetter fun = FUNCTION_CAST<v8::AccessorSetter>(setter_address);
  ASSERT(fun != NULL);
  ASSERT(callback->IsCompatibleReceiver(recv));
  Handle<String> name = args.at<String>(2);
  Handle<Object> value = args.at<Object>(3);
  HandleScope scope(isolate);
  LOG(isolate, ApiNamedPropertyAccess("store", recv, *name));
  CustomArguments custom_args(isolate, callback->data(), recv, recv);
  v8::AccessorInfo info(custom_args.end());
  {
    // Leaving JavaScript.
    VMState state(isolate, EXTERNAL);
    ExternalCallbackScope call_scope(isolate, setter_address);
    fun(v8::Utils::ToLocal(name), v8::Utils::ToLocal(value), info);
  }
  RETURN_IF_SCHEDULED_EXCEPTION(isolate);
  return *value;
}


static const int kAccessorInfoOffsetInInterceptorArgs = 2;


/**
 * Attempts to load a property with an interceptor (which must be present),
 * but doesn't search the prototype chain.
 *
 * Returns |Heap::no_interceptor_result_sentinel()| if interceptor doesn't
 * provide any value for the given name.
 */
RUNTIME_FUNCTION(MaybeObject*, LoadPropertyWithInterceptorOnly) {
  Handle<String> name_handle = args.at<String>(0);
  Handle<InterceptorInfo> interceptor_info = args.at<InterceptorInfo>(1);
  ASSERT(kAccessorInfoOffsetInInterceptorArgs == 2);
  ASSERT(args[2]->IsJSObject());  // Receiver.
  ASSERT(args[3]->IsJSObject());  // Holder.
  ASSERT(args[5]->IsSmi());  // Isolate.
  ASSERT(args.length() == 6);

  Address getter_address = v8::ToCData<Address>(interceptor_info->getter());
  v8::NamedPropertyGetter getter =
      FUNCTION_CAST<v8::NamedPropertyGetter>(getter_address);
  ASSERT(getter != NULL);

  {
    // Use the interceptor getter.
    v8::AccessorInfo info(args.arguments() -
                          kAccessorInfoOffsetInInterceptorArgs);
    HandleScope scope(isolate);
    v8::Handle<v8::Value> r;
    {
      // Leaving JavaScript.
      VMState state(isolate, EXTERNAL);
      r = getter(v8::Utils::ToLocal(name_handle), info);
    }
    RETURN_IF_SCHEDULED_EXCEPTION(isolate);
    if (!r.IsEmpty()) {
      Handle<Object> result = v8::Utils::OpenHandle(*r);
      result->VerifyApiCallResultType();
      return *v8::Utils::OpenHandle(*r);
    }
  }

  return isolate->heap()->no_interceptor_result_sentinel();
}


static MaybeObject* ThrowReferenceError(Isolate* isolate, String* name) {
  // If the load is non-contextual, just return the undefined result.
  // Note that both keyed and non-keyed loads may end up here, so we
  // can't use either LoadIC or KeyedLoadIC constructors.
  IC ic(IC::NO_EXTRA_FRAME, isolate);
  ASSERT(ic.target()->is_load_stub() || ic.target()->is_keyed_load_stub());
  if (!ic.SlowIsUndeclaredGlobal()) return HEAP->undefined_value();

  // Throw a reference error.
  HandleScope scope(isolate);
  Handle<String> name_handle(name);
  Handle<Object> error =
      FACTORY->NewReferenceError("not_defined",
                                  HandleVector(&name_handle, 1));
  return isolate->Throw(*error);
}


static MaybeObject* LoadWithInterceptor(Arguments* args,
                                        PropertyAttributes* attrs) {
  Handle<String> name_handle = args->at<String>(0);
  Handle<InterceptorInfo> interceptor_info = args->at<InterceptorInfo>(1);
  ASSERT(kAccessorInfoOffsetInInterceptorArgs == 2);
  Handle<JSObject> receiver_handle = args->at<JSObject>(2);
  Handle<JSObject> holder_handle = args->at<JSObject>(3);
  ASSERT(args->length() == 6);

  Isolate* isolate = receiver_handle->GetIsolate();

  Address getter_address = v8::ToCData<Address>(interceptor_info->getter());
  v8::NamedPropertyGetter getter =
      FUNCTION_CAST<v8::NamedPropertyGetter>(getter_address);
  ASSERT(getter != NULL);

  {
    // Use the interceptor getter.
    v8::AccessorInfo info(args->arguments() -
                          kAccessorInfoOffsetInInterceptorArgs);
    HandleScope scope(isolate);
    v8::Handle<v8::Value> r;
    {
      // Leaving JavaScript.
      VMState state(isolate, EXTERNAL);
      r = getter(v8::Utils::ToLocal(name_handle), info);
    }
    RETURN_IF_SCHEDULED_EXCEPTION(isolate);
    if (!r.IsEmpty()) {
      *attrs = NONE;
      Handle<Object> result = v8::Utils::OpenHandle(*r);
      result->VerifyApiCallResultType();
      return *result;
    }
  }

  MaybeObject* result = holder_handle->GetPropertyPostInterceptor(
      *receiver_handle,
      *name_handle,
      attrs);
  RETURN_IF_SCHEDULED_EXCEPTION(isolate);
  return result;
}


/**
 * Loads a property with an interceptor performing post interceptor
 * lookup if interceptor failed.
 */
RUNTIME_FUNCTION(MaybeObject*, LoadPropertyWithInterceptorForLoad) {
  PropertyAttributes attr = NONE;
  Object* result;
  { MaybeObject* maybe_result = LoadWithInterceptor(&args, &attr);
    if (!maybe_result->ToObject(&result)) return maybe_result;
  }

  // If the property is present, return it.
  if (attr != ABSENT) return result;
  return ThrowReferenceError(isolate, String::cast(args[0]));
}


RUNTIME_FUNCTION(MaybeObject*, LoadPropertyWithInterceptorForCall) {
  PropertyAttributes attr;
  MaybeObject* result = LoadWithInterceptor(&args, &attr);
  RETURN_IF_SCHEDULED_EXCEPTION(isolate);
  // This is call IC. In this case, we simply return the undefined result which
  // will lead to an exception when trying to invoke the result as a
  // function.
  return result;
}


RUNTIME_FUNCTION(MaybeObject*, StoreInterceptorProperty) {
  ASSERT(args.length() == 4);
  JSObject* recv = JSObject::cast(args[0]);
  String* name = String::cast(args[1]);
  Object* value = args[2];
  ASSERT(args.smi_at(3) == kStrictMode || args.smi_at(3) == kNonStrictMode);
  StrictModeFlag strict_mode = static_cast<StrictModeFlag>(args.smi_at(3));
  ASSERT(recv->HasNamedInterceptor());
  PropertyAttributes attr = NONE;
  MaybeObject* result = recv->SetPropertyWithInterceptor(
      name, value, attr, strict_mode);
  return result;
}


RUNTIME_FUNCTION(MaybeObject*, KeyedLoadPropertyWithInterceptor) {
  JSObject* receiver = JSObject::cast(args[0]);
  ASSERT(args.smi_at(1) >= 0);
  uint32_t index = args.smi_at(1);
  return receiver->GetElementWithInterceptor(receiver, index);
}


Handle<Code> StubCompiler::CompileCallInitialize(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  Code::ExtraICState extra_state = Code::ExtractExtraICStateFromFlags(flags);
  if (kind == Code::CALL_IC) {
    CallIC::GenerateInitialize(masm(), argc, extra_state);
  } else {
    KeyedCallIC::GenerateInitialize(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallInitialize");
  isolate()->counters()->call_initialize_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_INITIALIZE_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_INITIALIZE, *code));
  return code;
}


Handle<Code> StubCompiler::CompileCallPreMonomorphic(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  // The code of the PreMonomorphic stub is the same as the code
  // of the Initialized stub.  They just differ on the code object flags.
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  Code::ExtraICState extra_state = Code::ExtractExtraICStateFromFlags(flags);
  if (kind == Code::CALL_IC) {
    CallIC::GenerateInitialize(masm(), argc, extra_state);
  } else {
    KeyedCallIC::GenerateInitialize(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallPreMonomorphic");
  isolate()->counters()->call_premonomorphic_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_PRE_MONOMORPHIC_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_PRE_MONOMORPHIC, *code));
  return code;
}


Handle<Code> StubCompiler::CompileCallNormal(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  if (kind == Code::CALL_IC) {
    // Call normal is always with a explict receiver.
    ASSERT(!CallIC::Contextual::decode(
        Code::ExtractExtraICStateFromFlags(flags)));
    CallIC::GenerateNormal(masm(), argc);
  } else {
    KeyedCallIC::GenerateNormal(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallNormal");
  isolate()->counters()->call_normal_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_NORMAL_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_NORMAL, *code));
  return code;
}


Handle<Code> StubCompiler::CompileCallMegamorphic(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  Code::ExtraICState extra_state = Code::ExtractExtraICStateFromFlags(flags);
  if (kind == Code::CALL_IC) {
    CallIC::GenerateMegamorphic(masm(), argc, extra_state);
  } else {
    KeyedCallIC::GenerateMegamorphic(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallMegamorphic");
  isolate()->counters()->call_megamorphic_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_MEGAMORPHIC_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_MEGAMORPHIC, *code));
  return code;
}


Handle<Code> StubCompiler::CompileCallArguments(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  KeyedCallIC::GenerateNonStrictArguments(masm(), argc);
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallArguments");
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(Code::ExtractKindFromFlags(flags),
                                          CALL_MEGAMORPHIC_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_MEGAMORPHIC, *code));
  return code;
}


Handle<Code> StubCompiler::CompileCallMiss(Code::Flags flags) {
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  Code::ExtraICState extra_state = Code::ExtractExtraICStateFromFlags(flags);
  if (kind == Code::CALL_IC) {
    CallIC::GenerateMiss(masm(), argc, extra_state);
  } else {
    KeyedCallIC::GenerateMiss(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallMiss");
  isolate()->counters()->call_megamorphic_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(kind, CALL_MISS_TAG),
                          *code, code->arguments_count()));
  GDBJIT(AddCode(GDBJITInterface::CALL_MISS, *code));
  return code;
}


#ifdef ENABLE_DEBUGGER_SUPPORT
Handle<Code> StubCompiler::CompileCallDebugBreak(Code::Flags flags) {
  Debug::GenerateCallICDebugBreak(masm());
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallDebugBreak");
  PROFILE(isolate(),
          CodeCreateEvent(CALL_LOGGER_TAG(Code::ExtractKindFromFlags(flags),
                                          CALL_DEBUG_BREAK_TAG),
                          *code, code->arguments_count()));
  return code;
}


Handle<Code> StubCompiler::CompileCallDebugPrepareStepIn(Code::Flags flags) {
  // Use the same code for the the step in preparations as we do for the
  // miss case.
  int argc = Code::ExtractArgumentsCountFromFlags(flags);
  Code::Kind kind = Code::ExtractKindFromFlags(flags);
  if (kind == Code::CALL_IC) {
    // For the debugger extra ic state is irrelevant.
    CallIC::GenerateMiss(masm(), argc, Code::kNoExtraICState);
  } else {
    KeyedCallIC::GenerateMiss(masm(), argc);
  }
  Handle<Code> code = GetCodeWithFlags(flags, "CompileCallDebugPrepareStepIn");
  PROFILE(isolate(),
          CodeCreateEvent(
              CALL_LOGGER_TAG(kind, CALL_DEBUG_PREPARE_STEP_IN_TAG),
              *code,
              code->arguments_count()));
  return code;
}
#endif  // ENABLE_DEBUGGER_SUPPORT

#undef CALL_LOGGER_TAG


Handle<Code> StubCompiler::GetCodeWithFlags(Code::Flags flags,
                                            const char* name) {
  // Create code object in the heap.
  CodeDesc desc;
  masm_.GetCode(&desc);
  Handle<Code> code = factory()->NewCode(desc, flags, masm_.CodeObject());
#ifdef ENABLE_DISASSEMBLER
  if (FLAG_print_code_stubs) code->Disassemble(name);
#endif
  return code;
}


Handle<Code> StubCompiler::GetCodeWithFlags(Code::Flags flags,
                                            Handle<String> name) {
  return (FLAG_print_code_stubs && !name.is_null())
      ? GetCodeWithFlags(flags, *name->ToCString())
      : GetCodeWithFlags(flags, reinterpret_cast<char*>(NULL));
}


void StubCompiler::LookupPostInterceptor(Handle<JSObject> holder,
                                         Handle<String> name,
                                         LookupResult* lookup) {
  holder->LocalLookupRealNamedProperty(*name, lookup);
  if (lookup->IsFound()) return;
  if (holder->GetPrototype()->IsNull()) return;
  holder->GetPrototype()->Lookup(*name, lookup);
}


#define __ ACCESS_MASM(masm())


Register BaseLoadStubCompiler::HandlerFrontendHeader(Handle<JSObject> object,
                                                     Register object_reg,
                                                     Handle<JSObject> holder,
                                                     Handle<String> name,
                                                     Label* miss,
                                                     FrontendCheckType check) {
  if (check == PERFORM_INITIAL_CHECKS) {
    GenerateNameCheck(name, this->name(), miss);
    // Check that the receiver isn't a smi.
    __ JumpIfSmi(object_reg, miss);
  }

  // Check the prototype chain.
  return CheckPrototypes(object, object_reg, holder,
                         scratch1(), scratch2(), scratch3(),
                         name, miss);
}


Register BaseLoadStubCompiler::HandlerFrontend(Handle<JSObject> object,
                                               Register object_reg,
                                               Handle<JSObject> holder,
                                               Handle<String> name,
                                               Label* success,
                                               FrontendCheckType check) {
  Label miss;

  Register reg = HandlerFrontendHeader(
      object, object_reg, holder, name, &miss, check);

  HandlerFrontendFooter(success, &miss);
  return reg;
}


Handle<Code> BaseLoadStubCompiler::CompileLoadField(Handle<JSObject> object,
                                                    Handle<JSObject> holder,
                                                    Handle<String> name,
                                                    PropertyIndex index) {
  Label success;
  Register reg = HandlerFrontend(object, receiver(), holder, name,
                                 &success, PERFORM_INITIAL_CHECKS);
  __ bind(&success);
  GenerateLoadField(reg, holder, index);

  // Return the generated code.
  return GetCode(Code::FIELD, name);
}


Handle<Code> BaseLoadStubCompiler::CompileLoadConstant(
    Handle<JSObject> object,
    Handle<JSObject> holder,
    Handle<String> name,
    Handle<JSFunction> value) {
  Label success;
  HandlerFrontend(object, receiver(), holder, name,
                  &success, PERFORM_INITIAL_CHECKS);
  __ bind(&success);
  GenerateLoadConstant(value);

  // Return the generated code.
  return GetCode(Code::CONSTANT_FUNCTION, name);
}


Handle<Code> BaseLoadStubCompiler::CompileLoadCallback(
    Handle<JSObject> object,
    Handle<JSObject> holder,
    Handle<String> name,
    Handle<ExecutableAccessorInfo> callback) {
  Label success;

  Register reg = CallbackHandlerFrontend(
      object, receiver(), holder, name, &success,
      PERFORM_INITIAL_CHECKS, callback);
  __ bind(&success);
  GenerateLoadCallback(reg, callback);

  // Return the generated code.
  return GetCode(Code::CALLBACKS, name);
}


Handle<Code> BaseLoadStubCompiler::CompileLoadInterceptor(
    Handle<JSObject> object,
    Handle<JSObject> holder,
    Handle<String> name) {
  Label success;

  LookupResult lookup(isolate());
  LookupPostInterceptor(holder, name, &lookup);

  Register reg = HandlerFrontend(object, receiver(), holder, name,
                                 &success, PERFORM_INITIAL_CHECKS);
  __ bind(&success);
  // TODO(368): Compile in the whole chain: all the interceptors in
  // prototypes and ultimate answer.
  GenerateLoadInterceptor(reg, object, holder, &lookup, name);

  // Return the generated code.
  return GetCode(Code::INTERCEPTOR, name);
}


void BaseLoadStubCompiler::GenerateLoadPostInterceptor(
    Register interceptor_reg,
    Handle<JSObject> interceptor_holder,
    Handle<String> name,
    LookupResult* lookup) {
  Label success;
  Handle<JSObject> holder(lookup->holder());
  if (lookup->IsField()) {
    // We found FIELD property in prototype chain of interceptor's holder.
    // Retrieve a field from field's holder.
    Register reg = HandlerFrontend(interceptor_holder, interceptor_reg, holder,
                                   name, &success, SKIP_INITIAL_CHECKS);
    __ bind(&success);
    GenerateLoadField(reg, holder, lookup->GetFieldIndex());
  } else {
    // We found CALLBACKS property in prototype chain of interceptor's
    // holder.
    ASSERT(lookup->type() == CALLBACKS);
    Handle<ExecutableAccessorInfo> callback(
        ExecutableAccessorInfo::cast(lookup->GetCallbackObject()));
    ASSERT(callback->getter() != NULL);

    Register reg = CallbackHandlerFrontend(
        interceptor_holder, interceptor_reg, holder,
        name, &success, SKIP_INITIAL_CHECKS, callback);
    __ bind(&success);
    GenerateLoadCallback(reg, callback);
  }
}


Handle<Code> LoadStubCompiler::CompileLoadViaGetter(
    Handle<JSObject> object,
    Handle<JSObject> holder,
    Handle<String> name,
    Handle<JSFunction> getter) {
  Label success;
  HandlerFrontend(object, receiver(), holder, name,
                  &success, PERFORM_INITIAL_CHECKS);

  __ bind(&success);
  GenerateLoadViaGetter(masm(), getter);

  // Return the generated code.
  return GetCode(Code::CALLBACKS, name);
}


#undef __


Handle<Code> LoadStubCompiler::GetCode(Code::StubType type,
                                       Handle<String> name,
                                       InlineCacheState state) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::LOAD_IC, Code::kNoExtraICState, type);
  Handle<Code> code = GetCodeWithFlags(flags, name);
  PROFILE(isolate(), CodeCreateEvent(Logger::LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  return code;
}


Handle<Code> KeyedLoadStubCompiler::GetCode(Code::StubType type,
                                            Handle<String> name,
                                            InlineCacheState state) {
  Code::Flags flags = Code::ComputeFlags(
      Code::KEYED_LOAD_IC, state, Code::kNoExtraICState, type);
  Handle<Code> code = GetCodeWithFlags(flags, name);
  PROFILE(isolate(), CodeCreateEvent(Logger::KEYED_LOAD_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::LOAD_IC, *name, *code));
  return code;
}


Handle<Code> KeyedLoadStubCompiler::CompileLoadElementPolymorphic(
    MapHandleList* receiver_maps) {
  CodeHandleList handler_ics(receiver_maps->length());
  for (int i = 0; i < receiver_maps->length(); ++i) {
    Handle<Map> receiver_map = receiver_maps->at(i);
    Handle<Code> cached_stub;

    if ((receiver_map->instance_type() & kNotStringTag) == 0) {
      cached_stub = isolate()->builtins()->KeyedLoadIC_String();
    } else {
      bool is_js_array = receiver_map->instance_type() == JS_ARRAY_TYPE;
      ElementsKind elements_kind = receiver_map->elements_kind();

      if (IsFastElementsKind(elements_kind) ||
          IsExternalArrayElementsKind(elements_kind)) {
        cached_stub =
            KeyedLoadFastElementStub(is_js_array,
                                     elements_kind).GetCode(isolate());
      } else {
        ASSERT(elements_kind == DICTIONARY_ELEMENTS);
        cached_stub = KeyedLoadDictionaryElementStub().GetCode(isolate());
      }
    }

    handler_ics.Add(cached_stub);
  }
  Handle<Code> code = CompileLoadPolymorphic(receiver_maps, &handler_ics);
  isolate()->counters()->keyed_load_polymorphic_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(Logger::KEYED_LOAD_POLYMORPHIC_IC_TAG, *code, 0));
  return code;
}



Handle<Code> StoreStubCompiler::GetCode(Code::StubType type,
                                        Handle<String> name) {
  Code::Flags flags = Code::ComputeMonomorphicFlags(
      Code::STORE_IC, strict_mode_, type);
  Handle<Code> code = GetCodeWithFlags(flags, name);
  PROFILE(isolate(), CodeCreateEvent(Logger::STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::STORE_IC, *name, *code));
  return code;
}


Handle<Code> KeyedStoreStubCompiler::GetCode(Code::StubType type,
                                             Handle<String> name,
                                             InlineCacheState state) {
  Code::ExtraICState extra_state =
      Code::ComputeExtraICState(grow_mode_, strict_mode_);
  Code::Flags flags =
      Code::ComputeFlags(Code::KEYED_STORE_IC, state, extra_state, type);
  Handle<Code> code = GetCodeWithFlags(flags, name);
  PROFILE(isolate(), CodeCreateEvent(Logger::KEYED_STORE_IC_TAG, *code, *name));
  GDBJIT(AddCode(GDBJITInterface::KEYED_STORE_IC, *name, *code));
  return code;
}


Handle<Code> KeyedStoreStubCompiler::CompileStoreElementPolymorphic(
    MapHandleList* receiver_maps) {
  // Collect MONOMORPHIC stubs for all |receiver_maps|.
  CodeHandleList handler_ics(receiver_maps->length());
  MapHandleList transitioned_maps(receiver_maps->length());
  for (int i = 0; i < receiver_maps->length(); ++i) {
    Handle<Map> receiver_map(receiver_maps->at(i));
    Handle<Code> cached_stub;
    Handle<Map> transitioned_map =
        receiver_map->FindTransitionedMap(receiver_maps);

    // TODO(mvstanton): The code below is doing pessimistic elements
    // transitions. I would like to stop doing that and rely on Allocation Site
    // Tracking to do a better job of ensuring the data types are what they need
    // to be. Not all the elements are in place yet, pessimistic elements
    // transitions are still important for performance.
    bool is_js_array = receiver_map->instance_type() == JS_ARRAY_TYPE;
    ElementsKind elements_kind = receiver_map->elements_kind();
    if (!transitioned_map.is_null()) {
      cached_stub = ElementsTransitionAndStoreStub(
          elements_kind,
          transitioned_map->elements_kind(),
          is_js_array,
          strict_mode_,
          grow_mode_).GetCode(isolate());
    } else {
      cached_stub = KeyedStoreElementStub(
          is_js_array,
          elements_kind,
          grow_mode_).GetCode(isolate());
    }
    ASSERT(!cached_stub.is_null());
    handler_ics.Add(cached_stub);
    transitioned_maps.Add(transitioned_map);
  }
  Handle<Code> code =
      CompileStorePolymorphic(receiver_maps, &handler_ics, &transitioned_maps);
  isolate()->counters()->keyed_store_polymorphic_stubs()->Increment();
  PROFILE(isolate(),
          CodeCreateEvent(Logger::KEYED_STORE_POLYMORPHIC_IC_TAG, *code, 0));
  return code;
}


void KeyedStoreStubCompiler::GenerateStoreDictionaryElement(
    MacroAssembler* masm) {
  KeyedStoreIC::GenerateSlow(masm);
}


CallStubCompiler::CallStubCompiler(Isolate* isolate,
                                   int argc,
                                   Code::Kind kind,
                                   Code::ExtraICState extra_state,
                                   InlineCacheHolderFlag cache_holder)
    : StubCompiler(isolate),
      arguments_(argc),
      kind_(kind),
      extra_state_(extra_state),
      cache_holder_(cache_holder) {
}


bool CallStubCompiler::HasCustomCallGenerator(Handle<JSFunction> function) {
  if (function->shared()->HasBuiltinFunctionId()) {
    BuiltinFunctionId id = function->shared()->builtin_function_id();
#define CALL_GENERATOR_CASE(name) if (id == k##name) return true;
    CUSTOM_CALL_IC_GENERATORS(CALL_GENERATOR_CASE)
#undef CALL_GENERATOR_CASE
  }

  CallOptimization optimization(function);
  return optimization.is_simple_api_call();
}


Handle<Code> CallStubCompiler::CompileCustomCall(
    Handle<Object> object,
    Handle<JSObject> holder,
    Handle<JSGlobalPropertyCell> cell,
    Handle<JSFunction> function,
    Handle<String> fname) {
  ASSERT(HasCustomCallGenerator(function));

  if (function->shared()->HasBuiltinFunctionId()) {
    BuiltinFunctionId id = function->shared()->builtin_function_id();
#define CALL_GENERATOR_CASE(name)                               \
    if (id == k##name) {                                        \
      return CallStubCompiler::Compile##name##Call(object,      \
                                                   holder,      \
                                                   cell,        \
                                                   function,    \
                                                   fname);      \
    }
    CUSTOM_CALL_IC_GENERATORS(CALL_GENERATOR_CASE)
#undef CALL_GENERATOR_CASE
  }
  CallOptimization optimization(function);
  ASSERT(optimization.is_simple_api_call());
  return CompileFastApiCall(optimization,
                            object,
                            holder,
                            cell,
                            function,
                            fname);
}


Handle<Code> CallStubCompiler::GetCode(Code::StubType type,
                                       Handle<String> name) {
  int argc = arguments_.immediate();
  Code::Flags flags = Code::ComputeMonomorphicFlags(kind_,
                                                    extra_state_,
                                                    type,
                                                    argc,
                                                    cache_holder_);
  return GetCodeWithFlags(flags, name);
}


Handle<Code> CallStubCompiler::GetCode(Handle<JSFunction> function) {
  Handle<String> function_name;
  if (function->shared()->name()->IsString()) {
    function_name = Handle<String>(String::cast(function->shared()->name()));
  }
  return GetCode(Code::CONSTANT_FUNCTION, function_name);
}


Handle<Code> ConstructStubCompiler::GetCode() {
  Code::Flags flags = Code::ComputeFlags(Code::STUB);
  Handle<Code> code = GetCodeWithFlags(flags, "ConstructStub");
  PROFILE(isolate(), CodeCreateEvent(Logger::STUB_TAG, *code, "ConstructStub"));
  GDBJIT(AddCode(GDBJITInterface::STUB, "ConstructStub", *code));
  return code;
}


CallOptimization::CallOptimization(LookupResult* lookup) {
  if (lookup->IsFound() &&
      lookup->IsCacheable() &&
      lookup->type() == CONSTANT_FUNCTION) {
    // We only optimize constant function calls.
    Initialize(Handle<JSFunction>(lookup->GetConstantFunction()));
  } else {
    Initialize(Handle<JSFunction>::null());
  }
}

CallOptimization::CallOptimization(Handle<JSFunction> function) {
  Initialize(function);
}


int CallOptimization::GetPrototypeDepthOfExpectedType(
    Handle<JSObject> object,
    Handle<JSObject> holder) const {
  ASSERT(is_simple_api_call());
  if (expected_receiver_type_.is_null()) return 0;
  int depth = 0;
  while (!object.is_identical_to(holder)) {
    if (object->IsInstanceOf(*expected_receiver_type_)) return depth;
    object = Handle<JSObject>(JSObject::cast(object->GetPrototype()));
    if (!object->map()->is_hidden_prototype()) return kInvalidProtoDepth;
    ++depth;
  }
  if (holder->IsInstanceOf(*expected_receiver_type_)) return depth;
  return kInvalidProtoDepth;
}


void CallOptimization::Initialize(Handle<JSFunction> function) {
  constant_function_ = Handle<JSFunction>::null();
  is_simple_api_call_ = false;
  expected_receiver_type_ = Handle<FunctionTemplateInfo>::null();
  api_call_info_ = Handle<CallHandlerInfo>::null();

  if (function.is_null() || !function->is_compiled()) return;

  constant_function_ = function;
  AnalyzePossibleApiFunction(function);
}


void CallOptimization::AnalyzePossibleApiFunction(Handle<JSFunction> function) {
  if (!function->shared()->IsApiFunction()) return;
  Handle<FunctionTemplateInfo> info(function->shared()->get_api_func_data());

  // Require a C++ callback.
  if (info->call_code()->IsUndefined()) return;
  api_call_info_ =
      Handle<CallHandlerInfo>(CallHandlerInfo::cast(info->call_code()));

  // Accept signatures that either have no restrictions at all or
  // only have restrictions on the receiver.
  if (!info->signature()->IsUndefined()) {
    Handle<SignatureInfo> signature =
        Handle<SignatureInfo>(SignatureInfo::cast(info->signature()));
    if (!signature->args()->IsUndefined()) return;
    if (!signature->receiver()->IsUndefined()) {
      expected_receiver_type_ =
          Handle<FunctionTemplateInfo>(
              FunctionTemplateInfo::cast(signature->receiver()));
    }
  }

  is_simple_api_call_ = true;
}


} }  // namespace v8::internal
