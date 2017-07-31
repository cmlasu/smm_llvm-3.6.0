//===- Hello.cpp - Example code from "Writing an LLVM Pass" ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements two versions of the LLVM "Hello World" pass described
// in docs/WritingAnLLVMPass.html
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Support/Debug.h"

#include <iostream>

#include "../SMMCommon/Helper.h"


#define DEBUG_TYPE "smmcm"


using namespace llvm;

namespace {
    struct FuncName : public ModulePass { // Insert code management functions
	static char ID; // Pass identification, replacement for typeid
	FuncName() : ModulePass(ID) {}

	virtual void getAnalysisUsage(AnalysisUsage &AU) const {
	    AU.addRequired<CallGraphWrapperPass>();
	}


	virtual bool runOnModule (Module &mod) {
	    std::ostream &ofs = std::cerr;
	    // Call graph
	    CallGraph &cg = getAnalysis<CallGraphWrapperPass>().getCallGraph(); 
	    //ofs.open ("_func_names", std::ofstream::out | std::ofstream::trunc);
	    // Get the function calls within loops
	    for (CallGraph::iterator cgi = cg.begin(), cge = cg.end(); cgi != cge; cgi++) {
		if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
		    Function *fi = cgn->getFunction();
		    // Skip external nodes (inline functions and function pointers)
		    if(!fi)
			continue;
		    // Skip library functions
		    if (isLibraryFunction(fi))
			continue;
		    // Skip code management functions
		    if (isManagementFunction(fi))
			continue;
		    ofs << fi->getName().str() << "\n";
		}
	    }
	    //ofs.close();

	    return false;
	}

    };

}


char FuncName::ID = 2;
static RegisterPass<FuncName> Z("funcname", "Print out the names of all the user functions");


