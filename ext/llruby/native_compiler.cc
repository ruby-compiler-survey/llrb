#include <memory>
#include "iseq.h"
#include "native_compiler.h"
#include "llvm/ExecutionEngine/ExecutionEngine.h"
#include "llvm/ExecutionEngine/MCJIT.h"

namespace llruby {

uint64_t NativeCompiler::Compile(const Iseq& iseq, bool dry_run) {
  std::unique_ptr<llvm::Module> mod = llvm::make_unique<llvm::Module>("llruby", context);
  llvm::Function *func = CompileIseq(iseq, mod.get());
  if (!func) return 0;

  if (dry_run) {
    mod->dump();
    return 0;
  } else {
    return CreateNativeFunction(func, std::move(mod));
  }
}

uint64_t NativeCompiler::CreateNativeFunction(llvm::Function *func, std::unique_ptr<llvm::Module> mod) {
  llvm::ExecutionEngine *engine = llvm::EngineBuilder(std::move(mod)).create();
  if (engine == NULL) {
    fprintf(stderr, "Failed to create ExecutionEngine...\n");
    return 0;
  }
  return engine->getFunctionAddress(func->getName());
}

llvm::Function* NativeCompiler::CompileIseq(const Iseq& iseq, llvm::Module* mod) {
  std::vector<llvm::Type*> args = { llvm::IntegerType::get(context, 64) };
  llvm::FunctionType *func_type = llvm::FunctionType::get(llvm::Type::getInt64Ty(context), args, false);
  llvm::Function *func = llvm::Function::Create(func_type, llvm::Function::ExternalLinkage, "precompiled_method", mod);

  stack.clear();
  for (const Object& insn : iseq.bytecode) {
    if (insn.klass == "Array") {
      CompileInstruction(insn.array, mod);
    } else if (insn.klass == "Symbol") {
      // label. ignored for now
    } else if (insn.klass == "Fixnum" || insn.klass == "Integer") {
      // lineno. ignored for now
    } else {
      fprintf(stderr, "unexpected insn.klass!: %s\n", insn.klass.c_str());
      return nullptr;
    }
  }

  builder.SetInsertPoint(llvm::BasicBlock::Create(context, "", func));
  if (stack.size() > 0) {
    builder.CreateRet(CompileObject(stack.back()));
  } else {
    builder.CreateRet(builder.getInt64(Qnil)); // TODO: return NULL
  }
  return func;
}

void NativeCompiler::CompileInstruction(const std::vector<Object>& instruction, llvm::Module* mod) {
  const std::string& name = instruction[0].symbol;
  if (name == "putnil") {
    stack.push_back(Object(Qnil));
  } else if (name == "trace") {
    // ignored for now
  } else if (name == "leave") {
    // ignored for now
  }
}

llvm::Value* NativeCompiler::CompileObject(const Object& object) {
  if (object.klass == "NilClass") {
    return builder.getInt64(Qnil);
  } else {
    return builder.getInt64(Qnil); // TODO: return NULL
  }
}

} // namespace llruby
