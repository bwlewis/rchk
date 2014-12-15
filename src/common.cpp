
#include "common.h"

#include <llvm/IR/Module.h>
#include <llvm/IR/LLVMContext.h>
#include <llvm/IR/Function.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Linker/Linker.h>
#include <llvm/Support/SourceMgr.h>

#include <llvm/Support/raw_ostream.h>

// supported usage
//   tool
//     processes R.bin.bc
//   tool path/R.bin.bc
//     processes one file at given location (R binary)
//   tool patr/R.bin.bc path/module.bc
//     links module agains given base IR file and then checks only functions
//     from that module (but some tools need to do whole-program analysis
//     which also will include functions from the base
//      IR file not included in the module)
Module *parseArgsReadIR(int argc, char* argv[], FunctionsOrderedSetTy& functionsOfInterest, LLVMContext& context) {

  if (argc > 3) {
    errs() << argv[0] << " base_file.bc [module_file.bc]" << "\n";
    exit(1);
  }

  SMDiagnostic error;
  std::string baseFname;
  
  if (argc == 1) {
    baseFname = "R.bin.bc";  
  } else {
    baseFname = argv[1];
  }
  
  Module *base = ParseIRFile(baseFname, error, context);
  if (!base) {
    errs() << "ERROR: Cannot read base IR file " << baseFname << "\n";
    error.print(argv[0], errs());
    exit(1);
  }
  
  if (argc == 1 || argc == 2) {
    // only a single input file
    for(Module::iterator f = base->begin(), fe = base->end(); f != fe; ++f) {
      functionsOfInterest.insert(f);
    }
    return base;
  }
  
  // have two input files
  std::string moduleFname = argv[2];
  Module *module = ParseIRFile(moduleFname, error, context);
  if (!module) {
    errs() << "ERROR: Cannot read module IR file " << moduleFname << "\n";
    error.print(argv[0], errs());
    exit(1);  
  }
  for(Module::iterator f = module->begin(), fe = module->end(); f != fe; ++f) {
    functionsOfInterest.insert(base->getFunction(f->getName()));
  }
  std::string errorMessage;
  if (Linker::LinkModules(base, module, Linker::DestroySource, &errorMessage)) {	
    errs() << "Linking module " << moduleFname << " with base " << baseFname << " resulted in error " << errorMessage << ".\n";
  }
  return base;
}
