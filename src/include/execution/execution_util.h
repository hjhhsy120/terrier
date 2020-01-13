#pragma once

#include <memory>

#include "execution/ast/ast_dump.h"
#include "execution/ast/context.h"
#include "execution/compiler/codegen.h"
#include "execution/compiler/compiler.h"
#include "execution/parsing/parser.h"
#include "execution/parsing/scanner.h"
#include "execution/sema/sema.h"
#include "execution/util/cpu_info.h"
#include "execution/util/region.h"
#include "execution/vm/bytecode_generator.h"
#include "execution/vm/llvm_engine.h"
#include "execution/vm/module.h"
#include "loggers/execution_logger.h"

namespace terrier::execution {

class ExecutableQuery {
 public:
  ExecutableQuery(const common::ManagedPointer<planner::AbstractPlanNode> physical_plan,
                  const common::ManagedPointer<exec::ExecutionContext> exec_ctx) {
    // Compile and check for errors
    compiler::CodeGen codegen(exec_ctx.Get());
    compiler::Compiler compiler(&codegen, physical_plan.Get());
    auto root = compiler.Compile();
    if (codegen.Reporter()->HasErrors()) {
      EXECUTION_LOG_ERROR("Type-checking error! \n {}", codegen.Reporter()->SerializeErrors());
    }

    EXECUTION_LOG_INFO("Converted: \n {}", execution::ast::AstDump::Dump(root));

    // Convert to bytecode
    auto bytecode_module = vm::BytecodeGenerator::Compile(root, exec_ctx.Get(), "tmp-tpl");

    tpl_module_ = std::make_unique<vm::Module>(std::move(bytecode_module));
    region_ = codegen.ReleaseRegion();
    ast_ctx_ = codegen.ReleaseContext();
  }

  /**
 * Compile the TPL source in \a source and run it in both interpreted and JIT
 * compiled mode
 * @param source The TPL source
 * @param name The name of the module/program
 */

  /**
   * Construct the the executable query from a TPL string source
   *
   * @param source The TPL source
   * @param exec_ctx context to execute
   */
  ExecutableQuery(const std::string &source,
                  const common::ManagedPointer<exec::ExecutionContext> exec_ctx) {
    // Let's scan the source
    region_ = std::make_unique<util::Region>("repl-ast");
    util::Region error_region("repl-error");
    sema::ErrorReporter error_reporter(&error_region);
    ast_ctx_ = std::make_unique<ast::Context>(region_.get(), &error_reporter);

    parsing::Scanner scanner(source.data(), source.length());
    parsing::Parser parser(&scanner, ast_ctx_.get());

    // Parse
    ast::AstNode *root = parser.Parse();
    if (error_reporter.HasErrors()) {
      EXECUTION_LOG_ERROR("Parsing errors: \n {}", error_reporter.SerializeErrors());
      throw std::runtime_error("Parsing Error!");
    }

    // Type check
    sema::Sema type_check(ast_ctx_.get());
    type_check.Run(root);
    if (error_reporter.HasErrors()) {
      EXECUTION_LOG_ERROR("Type-checking errors: \n {}", error_reporter.SerializeErrors());
      throw std::runtime_error("Type Checking Error!");
    }

    EXECUTION_LOG_INFO("Converted: \n {}", execution::ast::AstDump::Dump(root));

    // Convert to bytecode
    auto bytecode_module = vm::BytecodeGenerator::Compile(root, exec_ctx.Get(), "tmp-tpl");

    tpl_module_ = std::make_unique<vm::Module>(std::move(bytecode_module));
  }

  void Run(const common::ManagedPointer<exec::ExecutionContext> exec_ctx, const vm::ExecutionMode mode) {
    // Run the main function
    std::function<int64_t(exec::ExecutionContext *)> main;
    if (!tpl_module_->GetFunction("main", mode, &main)) {
      EXECUTION_LOG_ERROR(
          "Missing 'main' entry function with signature "
          "(*ExecutionContext)->int32");
      return;
    }
    auto result = main(exec_ctx.Get());
    EXECUTION_LOG_INFO("main() returned: {}", result);
  }

 private:
  // TPL bytecodes for this query
  std::unique_ptr<vm::Module> tpl_module_;

  // Memory region and AST context from the code generation stage that need to stay alive as long as the TPL module will
  // be executed. Direct access to these objects is likely unneeded from this class, we just want to tie the life cycles
  // together.
  std::unique_ptr<util::Region> region_;
  std::unique_ptr<ast::Context> ast_ctx_;
};

class ExecutionUtil {
 public:
  ExecutionUtil() = delete;

  /**
   * Initialize all TPL subsystems
   */
  static void InitTPL() {
    execution::CpuInfo::Instance();
    execution::vm::LLVMEngine::Initialize();
  }

  /**
   * Shutdown all TPL subsystems
   */
  static void ShutdownTPL() { terrier::execution::vm::LLVMEngine::Shutdown(); }
};

}  // namespace terrier::execution 