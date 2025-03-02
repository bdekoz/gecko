/* -*- Mode: C++; tab-width: 8; indent-tabs-mode: nil; c-basic-offset: 2 -*-
 * vim: set ts=8 sts=2 et sw=2 tw=80:
 *
 * Copyright 2018 Mozilla Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "wasm/WasmCraneliftCompile.h"

#include "mozilla/ScopeExit.h"

#include "js/Printf.h"

#include "wasm/cranelift/baldrapi.h"
#include "wasm/cranelift/clifapi.h"
#include "wasm/WasmGenerator.h"
#if defined(JS_CODEGEN_X64) && defined(JS_JITSPEW) && \
    defined(ENABLE_WASM_CRANELIFT)
#  include "zydis/ZydisAPI.h"
#endif

#include "jit/MacroAssembler-inl.h"

using namespace js;
using namespace js::jit;
using namespace js::wasm;

bool wasm::CraneliftCanCompile() {
#ifdef JS_CODEGEN_X64
  return true;
#else
  return false;
#endif
}

static inline SymbolicAddress ToSymbolicAddress(BD_SymbolicAddress bd) {
  switch (bd) {
    case BD_SymbolicAddress::MemoryGrow:
      return SymbolicAddress::MemoryGrow;
    case BD_SymbolicAddress::MemorySize:
      return SymbolicAddress::MemorySize;
    case BD_SymbolicAddress::FloorF32:
      return SymbolicAddress::FloorF;
    case BD_SymbolicAddress::FloorF64:
      return SymbolicAddress::FloorD;
    case BD_SymbolicAddress::CeilF32:
      return SymbolicAddress::CeilF;
    case BD_SymbolicAddress::CeilF64:
      return SymbolicAddress::CeilD;
    case BD_SymbolicAddress::NearestF32:
      return SymbolicAddress::NearbyIntF;
    case BD_SymbolicAddress::NearestF64:
      return SymbolicAddress::NearbyIntD;
    case BD_SymbolicAddress::TruncF32:
      return SymbolicAddress::TruncF;
    case BD_SymbolicAddress::TruncF64:
      return SymbolicAddress::TruncD;
    case BD_SymbolicAddress::Limit:
      break;
  }
  MOZ_CRASH("unknown baldrdash symbolic address");
}

static void DisassembleCode(uint8_t* code, size_t codeLen) {
#if defined(JS_CODEGEN_X64) && defined(JS_JITSPEW) && \
    defined(ENABLE_WASM_CRANELIFT)
  zydisDisassemble(code, codeLen, [](const char* text) {
                                    JitSpew(JitSpew_Codegen, "%s", text);
                                  });
#else
  JitSpew(JitSpew_Codegen, "*** No disassembly available ***");
#endif
}

static bool GenerateCraneliftCode(WasmMacroAssembler& masm,
                                  const CraneliftCompiledFunc& func,
                                  const FuncTypeIdDesc& funcTypeId,
                                  uint32_t lineOrBytecode,
                                  uint32_t funcBytecodeSize,
                                  FuncOffsets* offsets) {
  wasm::GenerateFunctionPrologue(masm, funcTypeId, mozilla::Nothing(), offsets);

  // Omit the check when framePushed is small and we know there's no
  // recursion.
  if (func.framePushed < MAX_UNCHECKED_LEAF_FRAME_SIZE && !func.containsCalls) {
    masm.reserveStack(func.framePushed);
  } else {
    masm.wasmReserveStackChecked(func.framePushed,
                                 BytecodeOffset(lineOrBytecode));
  }
  MOZ_ASSERT(masm.framePushed() == func.framePushed);

  // Copy the machine code; handle jump tables and other read-only data below.
  uint32_t funcBase = masm.currentOffset();
  if (!masm.appendRawCode(func.code, func.codeSize)) {
    return false;
  }
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
  uint32_t codeEnd = masm.currentOffset();
#endif

  // Cranelift isn't aware of pinned registers in general, so we need to reload
  // both TLS and pinned regs from the stack.
  // TODO(bug 1507820): We should teach Cranelift to reload this register
  // itself, so we don't have to do it manually.
  masm.loadWasmTlsRegFromFrame();
  masm.loadWasmPinnedRegsFromTls();

  wasm::GenerateFunctionEpilogue(masm, func.framePushed, offsets);

  if (func.numRodataRelocs > 0) {
#if defined(JS_CODEGEN_X64) || defined(JS_CODEGEN_X86)
    constexpr size_t jumptableElementSize = 4;

    MOZ_ASSERT(func.jumptablesSize % jumptableElementSize == 0);

    // Align the jump tables properly.
    masm.haltingAlign(jumptableElementSize);

    // Copy over the tables and read-only data.
    uint32_t rodataBase = masm.currentOffset();
    if (!masm.appendRawCode(func.code + func.codeSize,
                            func.totalSize - func.codeSize)) {
      return false;
    }

    uint32_t numElem = func.jumptablesSize / jumptableElementSize;
    uint32_t bias = rodataBase - codeEnd;

    // Bias the jump table(s).  The table values are negative values
    // representing backward jumps.  By shifting the table down we increase the
    // distance and so we add a negative value to reflect the larger distance.
    //
    // Note addToPCRel4() works from the end of the instruction, hence the loop
    // bounds.
    for (uint32_t i = 1; i <= numElem; i++) {
      masm.addToPCRel4(rodataBase + (i * jumptableElementSize), -bias);
    }

    // Patch up the code locations.  These represent forward distances that also
    // become greater, so we add a positive value.
    for (uint32_t i = 0; i < func.numRodataRelocs; i++) {
      MOZ_ASSERT(func.rodataRelocs[i] < func.codeSize);
      masm.addToPCRel4(funcBase + func.rodataRelocs[i], bias);
    }
#else
    MOZ_CRASH("No jump table support on this platform");
#endif
  }

  masm.flush();
  if (masm.oom()) {
    return false;
  }
  offsets->end = masm.currentOffset();

  for (size_t i = 0; i < func.numMetadata; i++) {
    const CraneliftMetadataEntry& metadata = func.metadatas[i];

    CheckedInt<size_t> offset = funcBase;
    offset += metadata.codeOffset;
    if (!offset.isValid()) {
      return false;
    }

#ifdef DEBUG
    // Check code offsets.
    MOZ_ASSERT(offset.value() >= offsets->normalEntry);
    MOZ_ASSERT(offset.value() < offsets->ret);

    // Check bytecode offsets.
    if (metadata.moduleBytecodeOffset > 0 && lineOrBytecode > 0) {
      MOZ_ASSERT(metadata.moduleBytecodeOffset >= lineOrBytecode);
      MOZ_ASSERT(metadata.moduleBytecodeOffset <
                 lineOrBytecode + funcBytecodeSize);
    }
#endif
    // TODO(bug 1532716): Cranelift gives null bytecode offsets for symbolic
    // accesses.
    uint32_t bytecodeOffset = metadata.moduleBytecodeOffset
                                  ? metadata.moduleBytecodeOffset
                                  : lineOrBytecode;

    switch (metadata.which) {
      case CraneliftMetadataEntry::Which::DirectCall: {
        CallSiteDesc desc(bytecodeOffset, CallSiteDesc::Func);
        masm.append(desc, CodeOffset(offset.value()), metadata.extra);
        break;
      }
      case CraneliftMetadataEntry::Which::IndirectCall: {
        CallSiteDesc desc(bytecodeOffset, CallSiteDesc::Dynamic);
        masm.append(desc, CodeOffset(offset.value()));
        break;
      }
      case CraneliftMetadataEntry::Which::Trap: {
        Trap trap = (Trap)metadata.extra;
        BytecodeOffset trapOffset(bytecodeOffset);
        masm.append(trap, wasm::TrapSite(offset.value(), trapOffset));
        break;
      }
      case CraneliftMetadataEntry::Which::MemoryAccess: {
        BytecodeOffset trapOffset(bytecodeOffset);
        masm.appendOutOfBoundsTrap(trapOffset, offset.value());
        break;
      }
      case CraneliftMetadataEntry::Which::SymbolicAccess: {
        SymbolicAddress sym =
            ToSymbolicAddress(BD_SymbolicAddress(metadata.extra));
        masm.append(SymbolicAccess(CodeOffset(offset.value()), sym));
        break;
      }
      default: {
        MOZ_CRASH("unknown cranelift metadata kind");
      }
    }
  }

  return true;
}

// In Rust, a BatchCompiler variable has a lifetime constrained by those of its
// associated StaticEnvironment and ModuleEnvironment. This RAII class ties
// them together, as well as makes sure that the compiler is properly destroyed
// when it exits scope.

class AutoCranelift {
  CraneliftStaticEnvironment staticEnv_;
  CraneliftModuleEnvironment env_;
  CraneliftCompiler* compiler_;

 public:
  explicit AutoCranelift(const ModuleEnvironment& env)
      : env_(env), compiler_(nullptr) {}
  bool init() {
    compiler_ = cranelift_compiler_create(&staticEnv_, &env_);
    return !!compiler_;
  }
  ~AutoCranelift() {
    if (compiler_) {
      cranelift_compiler_destroy(compiler_);
    }
  }
  operator CraneliftCompiler*() { return compiler_; }
};

CraneliftFuncCompileInput::CraneliftFuncCompileInput(
    const FuncCompileInput& func)
    : bytecode(func.begin),
      bytecodeSize(func.end - func.begin),
      index(func.index),
      offset_in_module(func.lineOrBytecode) {}

#ifndef WASM_HUGE_MEMORY
static_assert(offsetof(TlsData, boundsCheckLimit) == sizeof(size_t),
              "fix make_heap() in wasm2clif.rs");
#endif

CraneliftStaticEnvironment::CraneliftStaticEnvironment()
    :
#ifdef JS_CODEGEN_X64
      hasSse2(Assembler::HasSSE2()),
      hasSse3(Assembler::HasSSE3()),
      hasSse41(Assembler::HasSSE41()),
      hasSse42(Assembler::HasSSE42()),
      hasPopcnt(Assembler::HasPOPCNT()),
      hasAvx(Assembler::HasAVX()),
      hasBmi1(Assembler::HasBMI1()),
      hasBmi2(Assembler::HasBMI2()),
      hasLzcnt(Assembler::HasLZCNT()),
#else
      hasSse2(false),
      hasSse3(false),
      hasSse41(false),
      hasSse42(false),
      hasPopcnt(false),
      hasAvx(false),
      hasBmi1(false),
      hasBmi2(false),
      hasLzcnt(false),
#endif
      staticMemoryBound(
#ifdef WASM_HUGE_MEMORY
          // In the huge memory configuration, we always reserve the full 4 GB
          // index space for a heap.
          IndexRange
#else
          // Otherwise, heap bounds are stored in the `boundsCheckLimit` field
          // of TlsData.
          0
#endif
          ),
      memoryGuardSize(OffsetGuardLimit),
      instanceTlsOffset(offsetof(TlsData, instance)),
      interruptTlsOffset(offsetof(TlsData, interrupt)),
      cxTlsOffset(offsetof(TlsData, cx)),
      realmCxOffset(JSContext::offsetOfRealm()),
      realmTlsOffset(offsetof(TlsData, realm)),
      realmFuncImportTlsOffset(offsetof(FuncImportTls, realm)) {
}

// Most of BaldrMonkey's data structures refer to a "global offset" which is a
// byte offset into the `globalArea` field of the  `TlsData` struct.
//
// Cranelift represents global variables with their byte offset from the "VM
// context pointer" which is the `WasmTlsReg` pointing to the `TlsData` struct.
//
// This function translates between the two.

static size_t globalToTlsOffset(size_t globalOffset) {
  return offsetof(wasm::TlsData, globalArea) + globalOffset;
}

CraneliftModuleEnvironment::CraneliftModuleEnvironment(
    const ModuleEnvironment& env)
    : env(&env), min_memory_length(env.minMemoryLength) {}

TypeCode env_unpack(BD_ValType valType) {
  return TypeCode(UnpackTypeCodeType(PackedTypeCode(valType.packed)));
}

const FuncTypeWithId* env_function_signature(
    const CraneliftModuleEnvironment* wrapper, size_t funcIndex) {
  return wrapper->env->funcTypes[funcIndex];
}

size_t env_func_import_tls_offset(const CraneliftModuleEnvironment* wrapper,
                                  size_t funcIndex) {
  return globalToTlsOffset(
      wrapper->env->funcImportGlobalDataOffsets[funcIndex]);
}

bool env_func_is_import(const CraneliftModuleEnvironment* wrapper,
                        size_t funcIndex) {
  return wrapper->env->funcIsImport(funcIndex);
}

const FuncTypeWithId* env_signature(const CraneliftModuleEnvironment* wrapper,
                                    size_t funcTypeIndex) {
  return &wrapper->env->types[funcTypeIndex].funcType();
}

const TableDesc* env_table(const CraneliftModuleEnvironment* wrapper,
                           size_t tableIndex) {
  return &wrapper->env->tables[tableIndex];
}

const GlobalDesc* env_global(const CraneliftModuleEnvironment* wrapper,
                             size_t globalIndex) {
  return &wrapper->env->globals[globalIndex];
}

bool wasm::CraneliftCompileFunctions(const ModuleEnvironment& env,
                                     LifoAlloc& lifo,
                                     const FuncCompileInputVector& inputs,
                                     CompiledCode* code, UniqueChars* error) {
  MOZ_RELEASE_ASSERT(CraneliftCanCompile());

  MOZ_ASSERT(env.tier() == Tier::Optimized);
  MOZ_ASSERT(env.optimizedBackend() == OptimizedBackend::Cranelift);
  MOZ_ASSERT(!env.isAsmJS());

  AutoCranelift compiler(env);
  if (!compiler.init()) {
    return false;
  }

  TempAllocator alloc(&lifo);
  JitContext jitContext(&alloc);
  WasmMacroAssembler masm(alloc);
  MOZ_ASSERT(IsCompilingWasm());

  // Swap in already-allocated empty vectors to avoid malloc/free.
  MOZ_ASSERT(code->empty());
  if (!code->swap(masm)) {
    return false;
  }

  // Disable instruction spew if we're going to disassemble after code
  // generation, or the output will be a mess.

  bool jitSpew = JitSpewEnabled(js::jit::JitSpew_Codegen);
  if (jitSpew) {
    DisableChannel(js::jit::JitSpew_Codegen);
  }
  auto reenableSpew = mozilla::MakeScopeExit([&] {
    if (jitSpew) {
      EnableChannel(js::jit::JitSpew_Codegen);
    }
  });

  for (const FuncCompileInput& func : inputs) {
    Decoder d(func.begin, func.end, func.lineOrBytecode, error);

    size_t funcBytecodeSize = func.end - func.begin;
    if (!ValidateFunctionBody(env, func.index, funcBytecodeSize, d)) {
      return false;
    }

    CraneliftFuncCompileInput clifInput(func);

    CraneliftCompiledFunc clifFunc;
    if (!cranelift_compile_function(compiler, &clifInput, &clifFunc)) {
      *error = JS_smprintf("Cranelift error in clifFunc #%u", clifInput.index);
      return false;
    }

    uint32_t lineOrBytecode = func.lineOrBytecode;
    const FuncTypeIdDesc& funcTypeId = env.funcTypes[clifInput.index]->id;

    FuncOffsets offsets;
    if (!GenerateCraneliftCode(masm, clifFunc, funcTypeId, lineOrBytecode,
                               funcBytecodeSize, &offsets)) {
      return false;
    }

    if (!code->codeRanges.emplaceBack(func.index, lineOrBytecode, offsets)) {
      return false;
    }
  }

  masm.finish();
  if (masm.oom()) {
    return false;
  }

  if (jitSpew) {
    // The disassembler uses the jitspew for output, so re-enable now.
    EnableChannel(js::jit::JitSpew_Codegen);

    uint32_t totalCodeSize = masm.currentOffset();
    uint8_t* codeBuf = (uint8_t*)js_malloc(totalCodeSize);
    if (codeBuf) {
        masm.executableCopy(codeBuf, totalCodeSize);

        for (const FuncCompileInput& func : inputs) {
          JitSpew(JitSpew_Codegen, "# ========================================");
          JitSpew(JitSpew_Codegen, "# Start of wasm cranelift code for index %d",
                  (int)func.index);

          uint32_t codeStart = code->codeRanges[func.index].begin();
          uint32_t codeEnd = code->codeRanges[func.index].end();
          DisassembleCode(codeBuf + codeStart, codeEnd - codeStart);

          JitSpew(JitSpew_Codegen, "# End of wasm cranelift code for index %d",
                  (int)func.index);
        }
        js_free(codeBuf);
    }
  }

  return code->swap(masm);
}

////////////////////////////////////////////////////////////////////////////////
//
// Callbacks from Rust to C++.

// Offsets assumed by the `make_heap()` function.
static_assert(offsetof(wasm::TlsData, memoryBase) == 0, "memory base moved");

// The translate_call() function in wasm2clif.rs depends on these offsets.
static_assert(offsetof(wasm::FuncImportTls, code) == 0,
              "Import code field moved");
static_assert(offsetof(wasm::FuncImportTls, tls) == sizeof(void*),
              "Import tls moved");

// Global

bool global_isConstant(const GlobalDesc* global) {
  return global->isConstant();
}

bool global_isIndirect(const GlobalDesc* global) {
  return global->isIndirect();
}

BD_ConstantValue global_constantValue(const GlobalDesc* global) {
  Val value(global->constantValue());
  BD_ConstantValue v;
  v.t = TypeCode(value.type().code());
  switch (v.t) {
    case TypeCode::I32:
      v.u.i32 = value.i32();
      break;
    case TypeCode::I64:
      v.u.i64 = value.i64();
      break;
    case TypeCode::F32:
      v.u.f32 = value.f32();
      break;
    case TypeCode::F64:
      v.u.f64 = value.f64();
      break;
    default:
      MOZ_CRASH("Bad type");
  }
  return v;
}

TypeCode global_type(const GlobalDesc* global) {
  return TypeCode(global->type().code());
}

size_t global_tlsOffset(const GlobalDesc* global) {
  return globalToTlsOffset(global->offset());
}

// TableDesc

size_t table_tlsOffset(const TableDesc* table) {
  MOZ_RELEASE_ASSERT(
      table->kind == TableKind::FuncRef || table->kind == TableKind::AsmJS,
      "cranelift doesn't support AnyRef tables yet.");
  return globalToTlsOffset(table->globalDataOffset);
}

// Sig

size_t funcType_numArgs(const FuncTypeWithId* funcType) {
  return funcType->args().length();
}

const BD_ValType* funcType_args(const FuncTypeWithId* funcType) {
  static_assert(sizeof(BD_ValType) == sizeof(ValType), "update BD_ValType");
  return (const BD_ValType*)&funcType->args()[0];
}

TypeCode funcType_retType(const FuncTypeWithId* funcType) {
  return TypeCode(funcType->ret().code());
}

FuncTypeIdDescKind funcType_idKind(const FuncTypeWithId* funcType) {
  return funcType->id.kind();
}

size_t funcType_idImmediate(const FuncTypeWithId* funcType) {
  return funcType->id.immediate();
}

size_t funcType_idTlsOffset(const FuncTypeWithId* funcType) {
  return globalToTlsOffset(funcType->id.globalDataOffset());
}
