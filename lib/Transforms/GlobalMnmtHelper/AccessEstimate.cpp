//===- --------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements several methods that are used to extract functions,
// loops, or portions of a module from the rest of the module.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"
#include "llvm/PassManager.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/InlineAsm.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Value.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ValueMapper.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include <fstream>
#include <queue>
#include <tuple>
#include <stack>
#include <utility>
#include <unordered_map>
#include <unordered_set>

#include "../SMMCommon/Helper.h"

#define DEBUG_TYPE "smmimh"

using namespace llvm;

cl::opt<std::string> size_constraint("size-constraint", cl::desc("Specify the size of available SPM space for data"), cl::value_desc("a string"));
cl::opt<std::string> output("output", cl::desc("Specify the output fil"), cl::value_desc("a string"));

bool comp(std::pair <Value *, long> p1, std::pair <Value *, long> p2) {
    long v1 = p1.second;
    long v2 = p2.second;
    return v1 < v2;
}

namespace {

    struct AccessEstimate : public ModulePass {
	static char ID; // Pass identification, replacement for typeid

	AccessEstimate() : ModulePass(ID) {
	}

	virtual void getAnalysisUsage(AnalysisUsage &AU) const {
	    AU.addRequired<CallGraphWrapperPass>();
	    AU.addRequired<LoopInfo>();
	    AU.addRequired<ScalarEvolution> ();
	}


	// Get the memory instruction of the specified value
	std::unordered_set <Instruction *> getMemInsts(Value *val) {
	    std::unordered_set <Instruction *> results;
	    for (Value::use_iterator ui = val->use_begin(), ue = val->use_end(); ui != ue; ++ui) {
		User *user = ui->getUser();
		//dbgs () << "\t" << *user << "\n";
		if(Instruction *inst = dyn_cast<Instruction>(user)) {
		    unsigned opcode = inst->getOpcode();
		    switch(opcode) {
			case Instruction::Load:
			    results.insert(inst);
			    break;
			case Instruction::Store:
			    results.insert(inst);
			    break;
			case Instruction::GetElementPtr: 
			case Instruction::BitCast: 
			    std::unordered_set <Instruction *> sub_results = getMemInsts (inst);
			    results.insert(sub_results.begin(), sub_results.end());
			    break;
		    }

		} else if (ConstantExpr *expr = dyn_cast<ConstantExpr>(user)){
		    std::unordered_set <Instruction *> sub_results = getMemInsts (expr);
		    results.insert(sub_results.begin(), sub_results.end());
		} else
		    assert(false);

	    }
	    return results;
	}

	// Estimate the number of accesses of the specified value
	std::unordered_map <Function *, long> estimateNumAccess(Value *val) {
	    std::unordered_map <Function *, long> num_accesses;
	    std::unordered_set <Instruction *> mem_insts = getMemInsts(val);
	    for (auto ii = mem_insts.begin(), ie = mem_insts.end(); ii != ie; ++ii) {
		Instruction *inst = *ii;
		BasicBlock *bb = inst->getParent();
		Function *func = bb->getParent();
		LoopInfo &lpi = getAnalysis<LoopInfo>(*func);
		if (num_accesses.find(func) == num_accesses.end())
		    num_accesses[func] = 0;
		ScalarEvolution &se = getAnalysis<ScalarEvolution> (*func);
		// Check if the value is accessed within any loop
		if (Loop *lp = lpi.getLoopFor(bb)) {
		    unsigned lp_num_accesses = se.getSmallConstantTripCount(lp);
		    if (lp_num_accesses == 0)
			// Assign a constant value if the trip count of the current loop is unknow
			lp_num_accesses = DEFAULT_TRIP_COUNT;
		    // Check if the current loop has any parent loops
		    lp = lp->getParentLoop();
		    while (lp) {
			unsigned trip_count = se.getSmallConstantTripCount(lp);
			if (trip_count == 0)
			    trip_count = DEFAULT_TRIP_COUNT;
			lp_num_accesses *= trip_count;
			lp = lp->getParentLoop();
		    }
		    num_accesses[func] += lp_num_accesses;
		}
		// If the value is accessed outside any loops, then just increase the counter by one
		else
		    ++num_accesses[func];
	    }

	    return num_accesses;
	}

	virtual bool runOnModule(Module &mod) {
	    const DataLayout *dl = mod.getDataLayout();
	    CallGraph &cg = getAnalysis<CallGraphWrapperPass>().getCallGraph(); // call graph
	    Function *func_main = mod.getFunction("main");
	    CallGraphNode *cgn_main = cg[func_main];
	    CallGraphNode::CallRecord *root;
	    std::vector<std::vector<CallGraphNode::CallRecord *> > paths;
	    std::unordered_map <Function *, long> stackNumAccesses;
	    std::unordered_map <Function *, long> globalNumAccesses;
	    std::unordered_map <Function *, long> heapNumAccesses;
	    std::unordered_map <Function *, std::vector<CallInst *> > callSites;

	    std::unordered_set <GlobalVariable *> globalVars;
	    std::unordered_set <Value *> allocatedGlobalVars;
	    std::unordered_map <GlobalVariable *, std::unordered_map<Function *, long> > gvarNumAccessesByFunction;
	    std::unordered_map <GlobalVariable *, long > gvarNumAccesses;
	    std::vector< std::pair <GlobalVariable *, long> > gvarAllocPriority;

	    long sizeConstraint = std::stoul(size_constraint);;
	    long spmGlobalSize = sizeConstraint;

	    if (!spmGlobalSize) {
		if (output != "") {
		    std::ofstream ofs;
		    ofs.open (output, std::ofstream::out | std::ofstream::trunc);
		    ofs << "data cache\n";
		    ofs.close();
		}
		return false;
	    }



	    // Step 1: estimate number of accesses to stack, data and heap segments
	    for (Module::global_iterator gi = mod.global_begin(), ge = mod.global_end(); gi != ge; ++gi) {
		GlobalVariable *gvar = &*gi;
		StringRef gvar_name = gvar->getName();
		PointerType *gvar_type = gvar->getType();
		assert(gvar_type);
		if (isManagementVariable(gvar))
		    continue;
		if (gvar_name.startswith(".str"))
		    continue;
		if (gvar_name.startswith("str"))
		    continue;
		if (gvar_name.startswith("_spm"))
		    continue;
		if (gvar_name == "stdin")
		    continue;
		if (gvar_name == "stdout")
		    continue;
		if (gvar_name == "stderr")
		    continue;
		globalVars.insert(gvar);
	    }
	    // Get call sites of user-defined functions with pointer-type parameters
	    for (CallGraph::iterator cgi = cg.begin(), cge = cg.end(); cgi != cge; cgi++) {
		if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
		    Function *caller = cgn->getFunction();
		    // Skip external nodes
		    if(!caller) 
			continue;
		    // Skip library functions
		    if (isLibraryFunction(caller))
			continue;
		    // Skip management functions
		    if (isManagementFunction(caller))
			continue;
		    // Process user-defined functions
		    for (CallGraphNode::iterator cgni = cgn->begin(), cgne = cgn->end(); cgni != cgne; cgni++) {
			CallInst *call_inst = dyn_cast <CallInst> (cgni->first);
			CallGraphNode *callee_cgn = dyn_cast <CallGraphNode> (cgni->second);
			Function *callee = callee_cgn->getFunction();
			assert(call_inst && callee_cgn);
			// Skip inline assembly
			if (call_inst->isInlineAsm())
			    continue;
			if (callee) {
			    // Skip library functions
			    if (isLibraryFunction(callee))
				continue;
			    // Skip calls to management functions
			    if(isManagementFunction(callee))
				continue;
			    // Skip recursive edges
			    if (cgn == callee_cgn) 
				continue;
			}
			// Call instructions to all the external nodes will be mapped to the NULL key
			callSites[callee].push_back(call_inst);
		    }
		}
	    }
	    // Estimate number of accesses to stack, data and heap segments in each function respectively
	    for (CallGraph::iterator cgi = cg.begin(), cge = cg.end(); cgi != cge; cgi++) {
		if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
		    Function *func = cgn->getFunction();
		    // Skip external nodes
		    if(!func) 
			continue;
		    // Skip library functions
		    if (isLibraryFunction(func))
			continue;
		    // Skip management functions
		    if (isManagementFunction(func))
			continue;
		    DEBUG(errs() << "\t" << func->getName() << "\n");
		    ScalarEvolution &se = getAnalysis<ScalarEvolution> (*func);
		    LoopInfo &lpi = getAnalysis<LoopInfo>(*func);
		    stackNumAccesses[func] = 0;
		    globalNumAccesses[func] = 0;
		    heapNumAccesses[func] = 0;
		    // Get all the stack variables in the current function 
		    for (Function::iterator bi = func->begin(), be = func->end(); bi != be; ++bi) {
			for (BasicBlock::iterator ii = bi->begin(), ie = bi->end(); ii != ie; ++ii) {
			    if (Instruction *inst = dyn_cast <Instruction> (ii)) {
				Value *ptr = NULL;
				unsigned opcode = inst->getOpcode();
				if (opcode == Instruction::Load) {
				    LoadInst *ld = cast<LoadInst>(inst);
				    ptr = ld->getPointerOperand();
				} else if (opcode == Instruction::Store) {
				    StoreInst *st = cast <StoreInst> (inst);
				    ptr = st->getPointerOperand();
				} else 
				    continue;
				// Estimate number of iterations of the current call
				unsigned num_iter = 0;
				// Check if the value is accessed within any loop
				if (Loop *lp = lpi.getLoopFor(inst->getParent())) {
				    num_iter = se.getSmallConstantTripCount(lp);
				    // Assign a constant value if the trip count of the current loop is unknow
				    if (num_iter == 0)
					num_iter = DEFAULT_TRIP_COUNT;
				    // Check if the current loop has any parent loops
				    lp = lp->getParentLoop();
				    while (lp) {
					unsigned lp_num_iter = se.getSmallConstantTripCount(lp);
					if (lp_num_iter == 0)
					    lp_num_iter = DEFAULT_TRIP_COUNT;
					num_iter *= lp_num_iter;
					lp = lp->getParentLoop();
				    }
				}
				// If the value is accessed outside any loops, then just increase the counter by one
				else
				    num_iter = 1;
				auto defs = getDeclarations(ptr, callSites);
				//DEBUG(dbgs() << "\t\t\t" << *inst << "\t" << defs.size() << "\n");
				for (size_t i = 0; i < defs.size() ; ++i) {
				    if (defs[i].second == STACK) {
					stackNumAccesses[func] += num_iter;
					//DEBUG(dbgs() << "\t\t\t\t" << ptr->getName() <<"\tstack\n");
				    } else if (defs[i].second == DATA) {
					GlobalVariable *gvar = dyn_cast <GlobalVariable>(defs[i].first);
					if (globalVars.find(gvar) == globalVars.end())
					    continue;
					globalNumAccesses[func] += num_iter;
					//DEBUG(dbgs() << "\t\t\t\t" << ptr->getName() <<"\tglobal\n");
				    } else if (defs[i].second == HEAP) {
					heapNumAccesses[func] += num_iter;
					//DEBUG(dbgs() << "\t\t\t\t" << ptr->getName() <<"\theap\n");
				    }
				}
			    }
			}
		    }
		}
	    }
	    //Step 2: extract all the paths from call graph root at main function
	    // Initialize root node by main function
	    for (CallGraphNode::iterator cgni = cg.begin()->second->begin(), cgne = cg.begin()->second->end(); cgni != cgne; cgni++) {
		if (cgni->second == cgn_main) {
		    root = &*cgni;
		    break;
		}
	    }
	    assert(CallGraphNode::iterator(root) != cg.begin()->second->end());
	    // Get the possible paths of the call graph
	    auto res = getPaths(root);
	    paths = res.first;
	    // Estimate the number of overall accesses to stack, global and heap
	    long stackTotalAccesses = 0;
	    long globalTotalAccesses = 0;
	    long heapTotalAccesses = 0;
	    //calledFuncs.insert(func_main);
	    for (size_t i = 0; i < paths.size(); i++) {
		stackTotalAccesses += stackNumAccesses[func_main];
		globalTotalAccesses += globalNumAccesses[func_main];
		heapTotalAccesses += heapNumAccesses[func_main];
		for (size_t j = 1; j < paths[i].size(); j++) {
		    CallInst * call_inst = dyn_cast <CallInst> (paths[i][j]->first);
		    //dbgs () << *call_inst << " " << "\t(";
		    BasicBlock *bb = call_inst->getParent();
		    Function *caller = bb->getParent();
		    Function *callee = call_inst->getCalledFunction();
		    //calledFuncs.insert(callee);
		    // Estimate number of iterations of the current call
		    unsigned num_iter = 0;
		    ScalarEvolution &se = getAnalysis<ScalarEvolution> (*caller);
		    LoopInfo &lpi = getAnalysis<LoopInfo>(*caller);
		    // Check if the value is accessed within any loop
		    if (Loop *lp = lpi.getLoopFor(bb)) {
			num_iter = se.getSmallConstantTripCount(lp);
			// Assign a constant value if the trip count of the current loop is unknow
			if (num_iter == 0)
			    num_iter = DEFAULT_TRIP_COUNT;
			// Check if the current loop has any parent loops
			lp = lp->getParentLoop();
			while (lp) {
			    unsigned lp_num_iter = se.getSmallConstantTripCount(lp);
			    if (lp_num_iter == 0)
				lp_num_iter = DEFAULT_TRIP_COUNT;
			    num_iter *= lp_num_iter;
			    lp = lp->getParentLoop();
			}
		    }
		    // If the value is accessed outside any loops, then just increase the counter by one
		    else
			num_iter = 1;
		    stackTotalAccesses += num_iter * stackNumAccesses[callee];
		    globalTotalAccesses += num_iter * globalNumAccesses[callee];
		    heapTotalAccesses += num_iter * heapNumAccesses[callee];
		}
	    }


	    // Step 3: estimate the number of accesses that can be reduced by allocating global variables into SPM
	    // Estimate number of accesses to global variables of each function
	    for (auto gi = globalVars.begin(), ge = globalVars.end(); gi != ge; ++gi) {
		GlobalVariable *gvar = *gi;
		gvarNumAccessesByFunction[gvar] = estimateNumAccess(gvar);
	    }
	    // Count the number of overall accesses
	    for (size_t i = 0; i < paths.size(); i++) {
		Function *func = paths[i][0]->second->getFunction();
		std::string func_name = func->getName();
		unsigned num_overall_iterations = 1;
		// The number of accesses to global variables in this path path
		std::unordered_map <Value *, long> num_overall_accesses;
		// Initiate the number of accesses of global variables in current path with the number of accesses in smm_main
		for (auto gi = globalVars.begin(), ge = globalVars.end(); gi != ge; ++gi) {
		    GlobalVariable *gvar = *gi;
		    num_overall_accesses[gvar] = gvarNumAccessesByFunction[gvar][func_main];
		}
		// Count the number of accesses of the rest of current path
		for (size_t j = 1; j < paths[i].size(); j++) {
		    CallInst *call_inst = dyn_cast <CallInst> (paths[i][j]->first);
		    BasicBlock *bb = call_inst->getParent();
		    Function *caller = bb->getParent();
		    Function *callee = paths[i][j]->second->getFunction();
		    std::string callee_name = callee->getName();
		    LoopInfo &lpi = getAnalysis<LoopInfo>(*caller);
		    ScalarEvolution &se = getAnalysis<ScalarEvolution> (*caller);
		    unsigned num_iterations = 1;
		    // Estimate the number of iterations if the call happens within a loop nest
		    if (Loop *lp = lpi.getLoopFor(bb)) {
			// Estimate the number of iterations of the innermost loop
			num_iterations = se.getSmallConstantTripCount(lp);
			if (num_iterations == 0)
			    num_iterations = DEFAULT_TRIP_COUNT;
			// Estimate the number of overall iterations
			lp = lp->getParentLoop();
			while (lp) {
			    unsigned trip_count = se.getSmallConstantTripCount(lp);
			    if (trip_count == 0)
				trip_count = DEFAULT_TRIP_COUNT;
			    num_iterations *= trip_count;
			    lp = lp->getParentLoop();
			}
		    }
		    // Accummulate the number of iterations
		    num_overall_iterations *= num_iterations;
		    // Accumulate the number of accesses for each global variables
		    for (auto gi = globalVars.begin(), ge = globalVars.end(); gi != ge; ++gi) {
			GlobalVariable *gvar = *gi;
			num_overall_accesses[gvar] += num_overall_iterations * gvarNumAccessesByFunction[gvar][callee];
		    }
		}
		// Add the number of accesses of the current path
		for (auto gi = globalVars.begin(), ge = globalVars.end(); gi != ge; ++gi) {
		    GlobalVariable *gvar = *gi;
		    gvarNumAccesses[gvar] += num_overall_accesses[gvar];
		}
	    }
	    // Calculate the priority of arrays for allocation
	    for (auto  gi = globalVars.begin(), ge = globalVars.end(); gi != ge; ++gi) {
		GlobalVariable *gvar = *gi;
		//PointerType *gvar_type = gvar->getType();
		//assert(gvar_type);
		//size_t gvar_size = getTypeSize(dl, gvar_type->getElementType());
		gvarAllocPriority.push_back(std::make_pair(gvar, gvarNumAccesses[gvar]));
	    }
	    sort(gvarAllocPriority.rbegin(), gvarAllocPriority.rend(), comp);

	    // Allocate as many global variables to the SPM as possible
	    long remainSpmGlobalSize = spmGlobalSize;
	    for (auto ii = gvarAllocPriority.begin(), ie = gvarAllocPriority.end(); ii != ie; ++ii) {
		GlobalVariable *gvar = ii->first;
		PointerType *gvar_type = gvar->getType();
		assert(gvar_type);
		size_t gvar_size = getTypeSize(dl, gvar_type->getElementType());
		remainSpmGlobalSize -= gvar_size;
		if (remainSpmGlobalSize < 0) 
		    break;
		//StringRef gvar_name = gvar->getName();
		//dbgs() << gvar_name << "\t" << *gvar_type << "\t" << gvar_size << "\n";
		allocatedGlobalVars.insert(gvar);
	    }

	    // Revaluate number of accesses to global variables in each function
	    for (CallGraph::iterator cgi = cg.begin(), cge = cg.end(); cgi != cge; cgi++) {
		if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
		    Function *func = cgn->getFunction();
		    // Skip external nodes
		    if(!func) 
			continue;
		    // Skip library functions
		    if (isLibraryFunction(func))
			continue;
		    // Skip management functions
		    if (isManagementFunction(func))
			continue;
		    //DEBUG(errs() << "\t" << func->getName() << "\n");
		    ScalarEvolution &se = getAnalysis<ScalarEvolution> (*func);
		    LoopInfo &lpi = getAnalysis<LoopInfo>(*func);
		    globalNumAccesses[func] = 0;
		    // Get all the stack variables in the current function 
		    for (Function::iterator bi = func->begin(), be = func->end(); bi != be; ++bi) {
			for (BasicBlock::iterator ii = bi->begin(), ie = bi->end(); ii != ie; ++ii) {
			    if (Instruction *inst = dyn_cast <Instruction> (ii)) {
				Value *ptr = NULL;
				unsigned opcode = inst->getOpcode();
				if (opcode == Instruction::Load) {
				    LoadInst *ld = cast<LoadInst>(inst);
				    ptr = ld->getPointerOperand();
				} else if (opcode == Instruction::Store) {
				    StoreInst *st = cast <StoreInst> (inst);
				    ptr = st->getPointerOperand();
				} else 
				    continue;


				// Estimate number of iterations of the current call
				unsigned num_iter = 0;
				// Check if the value is accessed within any loop
				if (Loop *lp = lpi.getLoopFor(inst->getParent())) {
				    num_iter = se.getSmallConstantTripCount(lp);
				    // Assign a constant value if the trip count of the current loop is unknow
				    if (num_iter == 0)
					num_iter = DEFAULT_TRIP_COUNT;
				    // Check if the current loop has any parent loops
				    lp = lp->getParentLoop();
				    while (lp) {
					unsigned lp_num_iter = se.getSmallConstantTripCount(lp);
					if (lp_num_iter == 0)
					    lp_num_iter = DEFAULT_TRIP_COUNT;
					num_iter *= lp_num_iter;
					lp = lp->getParentLoop();
				    }
				}
				// If the value is accessed outside any loops, then just increase the counter by one
				else
				    num_iter = 1;
				auto defs = getDeclarations(ptr, callSites);
				//DEBUG(dbgs() << "\t\t\t" << *inst << "\t" << defs.size() << "\n");
				for (size_t i = 0; i < defs.size() ; ++i) {
				    if (defs[i].second == DATA) {
					GlobalVariable *gvar = dyn_cast<GlobalVariable> (defs[i].first);
					if (globalVars.find(gvar) == globalVars.end())
					    continue;
					if (allocatedGlobalVars.find(gvar) == allocatedGlobalVars.end()) {
					    globalNumAccesses[func] += num_iter;
					    //DEBUG(dbgs() << "\t\t\t\t" << ptr->getName() <<"\tglobal\n");
					}
				    }
				}
			    }
			}
		    }
		}
	    }

	    // Revaluate the number of overall accesses to global 
	    long newGlobalTotalAccesses = 0;
	    //calledFuncs.insert(func_main);
	    for (size_t i = 0; i < paths.size(); i++) {
		newGlobalTotalAccesses += globalNumAccesses[func_main];
		for (size_t j = 1; j < paths[i].size(); j++) {
		    CallInst * call_inst = dyn_cast <CallInst> (paths[i][j]->first);
		    //dbgs () << *call_inst << " " << "\t(";
		    BasicBlock *bb = call_inst->getParent();
		    Function *caller = bb->getParent();
		    Function *callee = call_inst->getCalledFunction();
		    //calledFuncs.insert(callee);
		    // Estimate number of iterations of the current call
		    unsigned num_iter = 0;
		    ScalarEvolution &se = getAnalysis<ScalarEvolution> (*caller);
		    LoopInfo &lpi = getAnalysis<LoopInfo>(*caller);
		    // Check if the value is accessed within any loop
		    if (Loop *lp = lpi.getLoopFor(bb)) {
			num_iter = se.getSmallConstantTripCount(lp);
			// Assign a constant value if the trip count of the current loop is unknow
			if (num_iter == 0)
			    num_iter = DEFAULT_TRIP_COUNT;
			// Check if the current loop has any parent loops
			lp = lp->getParentLoop();
			while (lp) {
			    unsigned lp_num_iter = se.getSmallConstantTripCount(lp);
			    if (lp_num_iter == 0)
				lp_num_iter = DEFAULT_TRIP_COUNT;
			    num_iter *= lp_num_iter;
			    lp = lp->getParentLoop();
			}
		    }
		    // If the value is accessed outside any loops, then just increase the counter by one
		    else
			num_iter = 1;
		    newGlobalTotalAccesses += num_iter * globalNumAccesses[callee];
		}
	    }
	    if (output != "") {
		std::ofstream ofs;
		ofs.open (output, std::ofstream::out | std::ofstream::trunc);
		// Check if placing some of global variables in SPM will benefit (assume stack data is also managed in the SPM)
		if ((double)(newGlobalTotalAccesses+stackTotalAccesses+heapTotalAccesses) / (globalTotalAccesses+stackTotalAccesses+heapTotalAccesses) <= 0.4) {
		    ofs << "data spm\n";
		    ofs << "global " << spmGlobalSize << "\n";
		} else {
		    ofs << "data cache\n";
		}
		ofs.close();
	    }
	    //dbgs() << "stack count " << stackTotalAccesses << "\n";
	    //dbgs() << "global count " << globalTotalAccesses << "\t" << newGlobalTotalAccesses << "\n";
	    //dbgs() << "heap count " << heapTotalAccesses << "\n";

	    return false;
	}
    };
}

char AccessEstimate::ID = 0; //Id the pass.
static RegisterPass<AccessEstimate> X("estimate-accesses", "Estimate the number of accesses"); //Register the pass.

