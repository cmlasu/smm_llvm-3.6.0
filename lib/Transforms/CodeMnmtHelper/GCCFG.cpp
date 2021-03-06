#include "llvm/Pass.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"


#include <iostream>
#include <fstream>
#include <stack>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include "GCCFG.h"
#include "../SMMCommon/Helper.h"

#define DEBUG_TYPE "smmecm"

using namespace llvm;

static unsigned long numRegions;
static std::unordered_map <Function *, unsigned long> overlayMap;
static std::unordered_map <Function *, GCCFGFunction *> funcMap;
static std::unordered_map <BasicBlock *, GCCFGBasicBlock *> bbMap;

std::ostream& operator<<(std::ostream& out, AnalysisResult& ar) {
    switch(ar) {
	case Uncategorized:
	    out << "Uncategorized";
	    break;
	case FirstMiss:
	    out << "FirstMiss";
	    break;
	case AlwaysHit:
	    out << "AlwaysHit";
	    break;
	default:
	    assert(false);
	    break;
    }
    return out;
}

std::ostream& operator<<(std::ostream& out, RegionStatus& r) {
    for (unsigned long i = 0; i < numRegions; ++i) {
	out << i << ":";
	for (auto ii = r[i].begin(), ie = r[i].end(); ii != ie; ++ii) {
	    Function *func = *ii;
	    out << func->getName().str() << " ";
	}
	out << " ";
    }
    return out;
}

/* Class GCCFG definition begins */
GCCFG::GCCFG(Pass *p) {
    this->p = p;
    cg = &p->getAnalysis<CallGraphWrapperPass>().getCallGraph();
}

void GCCFG::build(std::string overlaying) {
    std::ifstream ifs;
    Module &mod = cg->getModule();
    Function *func_main = mod.getFunction("main");
    Function *func_smm_main = mod.getFunction("smm_main");
    // Use smm_main as the root node if it exists
    if (func_smm_main) 
	root = func_smm_main;
    else 
	root = func_main;
    // Read the file that specifies the mapping from functions to regions 
    ifs.open(overlaying, std::fstream::in);
    assert(ifs.good());

    // Read function mapping 
    ifs >> numRegions;
    while (ifs.good()) {
	unsigned long regionID;
	std::string funcName;
	Function *func;
	ifs >> funcName >> regionID;
	if (funcName.empty())
	    continue;
	if (funcName != "main")
	    func = mod.getFunction(funcName);
	else 
	    func = root;
	assert(func);
	// Ignore white spaces after the last line
	overlayMap[func] = regionID;
    }

    ifs.close();

    // Add functions
    for (CallGraph::iterator cgi = cg->begin(), cge = cg->end(); cgi != cge; cgi++) {
	if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
	    Function *func = cgn->getFunction();
	    // Skip external nodes (inline asm and function pointers)
	    if(!func)
		continue;
	    // Skip code management functions
	    if (isManagementFunction(func))
		continue;
	    // Skip library functions
	    if (isLibraryFunction(func))
		continue;
	    // Skip the main function
	    if (func_smm_main && func == func_main)
		continue;

	    /*
	       std::unordered_set<CallInst *> userCalls;
	       for (CallGraphNode::iterator cgni = cgn->begin(), cgne = cgn->end(); cgni != cgne; ++cgni) {
	       userCalls.insert(dyn_cast<CallInst>(cgni->first));
	       }
	     */

	    BasicBlock *entry = &func->getEntryBlock();
	    GCCFGFunction *gf = new GCCFGFunction(this, func, overlayMap[func]);
	    funcMap[func] = gf;
	    addFunction(gf);

	    // Add basic blocks
	    for (Function::iterator bi = func->begin(), be = func->end(); bi != be; ++bi) {
		GCCFGInstruction *gInst; 
		BasicBlock *bb = &*bi;
		GCCFGBasicBlock *gbb = new GCCFGBasicBlock(bb);
		bbMap[bb] = gbb;
		gf->addBasicBlock(gbb);
		// Set the entry basic block
		if (bb == entry)
		    gf->setEntryBlock(gbb);
		// Add call instructions to the current basic block
		for (BasicBlock::iterator ii = bb->begin(), ie = bb->end(); ii != ie; ++ii) {
		    if (CallInst *callInst = dyn_cast <CallInst> (ii)) {
			Function *callee = callInst->getCalledFunction();
			if(!callee) {
			    bool isFunctionPointer = true;
			    // Skip ineline assemblies (function pointers should be handled)
			    if (callInst->isInlineAsm())
				isFunctionPointer = false;
			    else {
				//DEBUG(dbgs() << func->getName() << " : " << *callInst << "\n");
				for (User::op_iterator oi = callInst->op_begin(), oe = callInst->op_end(); oi != oe; ++oi) {
				    Value *val = oi->get();
				    //DEBUG(dbgs() << "\t" << *val << "\t" << dyn_cast<ConstantExpr> (val) << "\n"); 
				    if (dyn_cast<ConstantExpr>(val)) {
					isFunctionPointer = false;
					break;
				    }
				}
			    }
			    if (isFunctionPointer)
				assert(false);
			    continue;
			} else {
			    // Skip code management functions
			    if (isManagementFunction(callee))
				continue;
			    // Skip library functions
			    if (isLibraryFunction(callee))
				continue;
			}

			gInst = new GCCFGInstruction(&*ii);
			gbb->addInstruction(gInst);
			gf->increaseNumberOfCalls();

		    }
		    // Add an exit block if this block contains the return instruction
		    if (dyn_cast<ReturnInst>(ii)) {
			gf->addExitBlock(gbb);
		    }
		}
	    }

	}
    }


    for (CallGraph::iterator cgi = cg->begin(), cge = cg->end(); cgi != cge; cgi++) {
	if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
	    Function *func = cgn->getFunction();
	    // Skip external nodes (inline asm and function pointers)
	    if(!func)
		continue;
	    // Skip code management functions
	    if (isManagementFunction(func))
		continue;
	    // Skip library functions
	    if (isLibraryFunction(func))
		continue;
	    // SKip the main function
	    if (func_smm_main && func == func_main)
		continue;

	    // Connect basic blocks
	    for (Function::iterator bi = func->begin(), be = func->end(); bi != be; ++bi) {
		BasicBlock *bb = &*bi;
		GCCFGBasicBlock *gbb = bbMap[bb];

		for (pred_iterator pi = pred_begin(bb), pe = pred_end(bb); pi != pe; ++pi) {
		    BasicBlock *pred = *pi;
		    GCCFGBasicBlock *gpred = bbMap[pred];
		    gbb->addPredecessor(gpred);
		}

		for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si != se; ++si) {
		    BasicBlock *succ = *si;
		    GCCFGBasicBlock *gsucc = bbMap[succ];
		    gbb->addSuccessor(gsucc);
		}

	    }
	}

    }

    //print();

    return;
}

void GCCFG::addFunction(GCCFGFunction *gf) {
    gFuncs.push_back(gf);
}

void GCCFG::runOnce() {

    GCCFGFunction *gFunc = funcMap[root];
    gFunc->runOnce();
    for (std::vector<GCCFGFunction *>::iterator ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gFunc = *ii;
	gFunc->initialize();
    }
}

void GCCFG::analyze(std::string overlaying) {
    std::unordered_map <CallInst *, std::pair<AnalysisResult, AnalysisResult> > result1, result2, result;

    // Build GCCFG
    build(overlaying);
    // Initialize states of functions
    runOnce();
    // Run Must analysis
    DEBUG(dbgs() << "################### Must Analysis starts########################\n");
    analyzeAlwaysHit();
    DEBUG(dbgs() << "################### Must Analysis ends########################\n");
    //printCategory();
    // Reset the graph for the next analysis
    resetForNextAnalysis();

    // Run Persistence analysis
    DEBUG(dbgs() << "################### Persistence Analysis starts########################\n");
    analyzeFirstMiss();
    DEBUG(dbgs() << "################### Persistence Analysis ends########################\n");
    //printCategory();

    // Calculate analysis result
    calculateAnalysisResult();

}

void GCCFG::analyzeAlwaysHit() {

    GCCFGFunction *gFunc = funcMap[root];
    analysisType = MustAnalysis;

    RegionStatus temp(numRegions);
    DEBUG(dbgs() << "###########################################\n");
    gFunc->simulateThrough(temp);
    DEBUG(dbgs() << "###########################################\n");

    while (!converge()) {
	resetForNextIteration();

	DEBUG(dbgs() << "###########################################\n");
	gFunc->simulate(temp);
	DEBUG(dbgs() << "###########################################\n");
    }
    // Categorize the call instructions
    categorize();
}

void GCCFG::analyzeFirstMiss() {
    analysisType = PersistenceAnalysis;

    RegionStatus temp(numRegions);
    for (std::vector<GCCFGFunction *>::iterator fi = gFuncs.begin(), fe = gFuncs.end(); fi != fe; ++fi) {
	GCCFGFunction *gFunc = *fi;
	DEBUG(dbgs() << "####################Function starts######################\n");
	gFunc->simulateLoops();
	DEBUG(dbgs() << "####################Function ends  ############\n");
    }
}


AnalysisType GCCFG::getAnalysisType() {
    return analysisType;
}

bool GCCFG::converge() {
    //DEBUG(dbgs() << "GCCFG converge\n");
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gFunc = *ii;
	if (!gFunc->converge())
	    return false;
    }
    return true;
}

void GCCFG::resetForNextIteration() {
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gFunc = *ii;
	gFunc->resetForNextIteration();
    }
}


void GCCFG::resetForNextAnalysis() {
    //DEBUG(dbgs() << "GCCFG reset\n");
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gFunc = *ii;
	gFunc->resetForNextAnalysis();
    }
}

void GCCFG::categorize() {
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gFunc = *ii;
	// Skip dead code
	if (gFunc->getNumberAccessed() <= 0)
	    continue;
	std::vector <GCCFGBasicBlock *> gBBs = gFunc->getBasicBlocks();
	for (auto ji = gBBs.begin(), je = gBBs.end(); ji != je; ++ji) {
	    GCCFGBasicBlock *gBB = *ji;
	    std::vector <GCCFGInstruction *> gInsts = gBB->getInstructions();
	    for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
		GCCFGInstruction *gInst = *ki;
		gInst->categorize();
	    }
	}
    }
}


std::unordered_map <CallInst *, std::pair<AnalysisResult, AnalysisResult> > GCCFG::getAnalysisResult() {
    return analysisResult;
}

void GCCFG::calculateAnalysisResult() {
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gf = *ii;
	if (gf->getNumberAccessed() <= 0)
	    continue;
	//DEBUG(dbgs() << gf->getName() << "\n");
	std::vector <GCCFGBasicBlock *> gbbs = gf->getBasicBlocks();
	for (auto ji = gbbs.begin(), je = gbbs.end(); ji != je; ++ji) {
	    GCCFGBasicBlock *gbb = *ji;
	    //DEBUG(dbgs() << "\t" << gbb->getName() << "\n");
	    std::vector <GCCFGInstruction *> gInsts = gbb->getInstructions();
	    for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
		GCCFGInstruction *gInst = *ki;
		CallInst *callInst = cast<CallInst>(gInst->getLLVMInstruction());
		//DEBUG(dbgs() << "\t\t" << *callInst <<  " (" << callInst << ") " << gInst->finalCalleeAnalysis << " " << gInst->finalCallerAnalysis << "\n");


		analysisResult[callInst] = std::make_pair(gInst->finalCalleeAnalysis, gInst->finalCallerAnalysis);
	    }
	}
    }
}


void GCCFG::print() {
    Module &mod = cg->getModule();
    Function *func_main = mod.getFunction("main");
    Function *func_smm_main = mod.getFunction("smm_main");
    DEBUG(dbgs() << "\nOriginal\n");
    for (CallGraph::iterator cgi = cg->begin(), cge = cg->end(); cgi != cge; cgi++) {
	if(CallGraphNode *cgn = dyn_cast<CallGraphNode>(cgi->second)) {
	    Function *func = cgn->getFunction();
	    // Skip external nodes (inline asm and function pointers)
	    if(!func)
		continue;
	    // Skip code management functions
	    if (isManagementFunction(func))
		continue;
	    // Skip library functions
	    if (isLibraryFunction(func))
		continue;
	    // SKip the main function
	    if (func_smm_main && func == func_main)
		continue;

	    DEBUG(dbgs() << "Function: " << func->getName() << "\n\tBasic Blocks:\n");
	    for (Function::iterator bi = func->begin(), be = func->end(); bi != be; ++bi) {
		BasicBlock *bb = &*bi;
		DEBUG(dbgs() << "\t" << bb->getName() << "\n\t\tPredecessors:");
		for (pred_iterator pi = pred_begin(bb), pe = pred_end(bb); pi != pe; ++pi) {
		    BasicBlock *pred = *pi;
		    DEBUG(dbgs() << " " << pred->getName());
		}
		DEBUG(dbgs() << "\n");
		DEBUG(dbgs() << "\t\tSuccessors:");
		for (succ_iterator si = succ_begin(bb), se = succ_end(bb); si != se; ++si) {
		    BasicBlock *succ = *si;
		    DEBUG(dbgs() << " " << succ->getName());
		}
		DEBUG(dbgs() << "\n");
	    }
	}

    }
    DEBUG(dbgs() << "\n");


    DEBUG(dbgs() << "GCCFG\n");
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gf = *ii;
	DEBUG(dbgs() << "Function: " << gf->getName() << "\n\tBasic Blocks:\n");
	std::vector <GCCFGBasicBlock *> gbbs = gf->getBasicBlocks();
	for (auto ji = gbbs.begin(), je = gbbs.end(); ji != je; ++ji) {
	    GCCFGBasicBlock *gbb = *ji;
	    DEBUG(dbgs() << "\t" << gbb->getName() << "\n\t\tPredecessors:");
	    for (unsigned long i = 0; i < gbb->getNumPredecessors(); ++i) {
		GCCFGBasicBlock *gpred = gbb->getPredecessor(i);
		DEBUG(dbgs() << " " << gpred->getName());
	    }
	    DEBUG(dbgs() << "\n");
	    DEBUG(dbgs() << "\t\tSuccessors:");
	    for (unsigned long i = 0; i < gbb->getNumSuccessors(); ++i) {
		GCCFGBasicBlock *gsucc = gbb->getSuccessor(i);
		DEBUG(dbgs() << " " << gsucc->getName());
	    }
	    DEBUG(dbgs() << "\n");
	    /*
	       DEBUG(dbgs() << "\t\t\tCall Instructions:\n");
	       std::vector <GCCFGInstruction *> gInsts = gbb->getInstructions();
	       for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
	       GCCFGInstruction *gInst = *ki;
	       DEBUG(dbgs() << "\t\t\t" << *gInst->getLLVMInstruction() << "\n");
	       }
	     */
	}
    }
    DEBUG(dbgs() << "\n");
}


void GCCFG::printCategory() {
    DEBUG(dbgs() << "GCCFG: print category: ");
    if (analysisType == MustAnalysis) {
	DEBUG(dbgs() << "must analysis");
    } else if (analysisType == PersistenceAnalysis) {
	DEBUG(dbgs() << "persistence analysis");
    }
    DEBUG(dbgs() << "\n");
    for (auto ii = gFuncs.begin(), ie = gFuncs.end(); ii != ie; ++ii) {
	GCCFGFunction *gf = *ii;
	gf->printCategory();

    }
}

/* Class GCCFG definition ends */


/* Class GCCFGFunction defnition begins */

GCCFGFunction::GCCFGFunction(GCCFG *gccfg, Function *f, unsigned long regionID) {
    this->gccfg = gccfg;
    func = f;
    name = f->getName();
    numCalls = 0;
    this->regionID = regionID;
    accessNum = -1;
    numAccessed = 0;

}

GCCFG* GCCFGFunction::getParent() {
    return gccfg;
}

std::string GCCFGFunction::getName() {
    return name;
}

long GCCFGFunction::getNumberOfCalls() {
    return numCalls;
}

void GCCFGFunction::increaseNumberOfCalls() {
    ++numCalls;
}

GCCFGBasicBlock *GCCFGFunction::getEntryBlock() {
    return entryBB;
}

void GCCFGFunction::setEntryBlock(GCCFGBasicBlock *gEntryBlock) {
    entryBB = gEntryBlock;
}

std::vector<GCCFGBasicBlock *> GCCFGFunction::getExitBlocks() {
    return returnBBs;
}

void GCCFGFunction::addExitBlock(GCCFGBasicBlock *gBB) {
    returnBBs.push_back(gBB);
}

std::vector <GCCFGBasicBlock *> GCCFGFunction::getBasicBlocks() {
    return gBBs;
}

void GCCFGFunction::addBasicBlock(GCCFGBasicBlock *gBB) {
    gBBs.push_back(gBB);
}


unsigned long GCCFGFunction::getRegionID() {
    return regionID;
}

void GCCFGFunction::runOnce() {
    unsigned long numPredecessors;
    std::stack <GCCFGBasicBlock *> s;
    std::unordered_set<GCCFGBasicBlock *> discovered;
    GCCFGBasicBlock *entryBB = getEntryBlock();

    numAccessed++;

    // If this function does not return, then return the input status (before updating the region this function is mapped to)

    // Label the entry block as discovered
    discovered.insert(entryBB);

    // Simulate the entry block
    entryBB->runOnce();
    // Push the neighbouring nodes of the entry block to the stack
    for (unsigned long i = 0; i < entryBB->getNumSuccessors(); ++i) {
	s.push(entryBB->getSuccessor(i));
    }
    // Traverse the CFG of this function with DFS
    while (!s.empty()) {
	// Pick up the node at stack top
	GCCFGBasicBlock *v = s.top();
	s.pop();
	//DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << "\n");
	if (discovered.find(v) == discovered.end()) {
	    // Label the node as discovered
	    discovered.insert(v);
	    numPredecessors = v->getNumPredecessors();
	    assert(numPredecessors >= 1);
	    // Simulate the current basic block
	    v->runOnce();
	    //DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << " " << v->getNumSuccessors() << "\n");
	    // Push all the neighbors of v to the stack
	    for (unsigned long i = 0; i < v->getNumSuccessors(); ++i) {
		s.push(v->getSuccessor(i));
	    }

	}

    }
}

void GCCFGFunction::initialize() {
    if (numAccessed <= 0)
	return;

    for (long i = 0; i < numAccessed; ++i) {
	inputs.push_back(new RegionStatus(numRegions));
	outputs.push_back(new RegionStatus(numRegions));
	previousInputs.push_back(new RegionStatus(numRegions));
	previousOutputs.push_back(new RegionStatus(numRegions));
    }

    for (std::vector <GCCFGBasicBlock *>::iterator ii = gBBs.begin(), ie = gBBs.end(); ii != ie; ++ii) {
	GCCFGBasicBlock * gBB = *ii;
	gBB->initialize();
    }
}

long GCCFGFunction::getNumberAccessed() {
    return numAccessed;
}

long GCCFGFunction::getAccessNumber() {
    return accessNum;
}

void GCCFGFunction::resetAccessNumber() {
    accessNum = -1;
}

AnalysisType GCCFGFunction::getAnalysisType() {
    return gccfg->getAnalysisType();
}

RegionStatus GCCFGFunction::getOutput() {
    return *outputs[accessNum];
}

void GCCFGFunction::simulateLoops() {
    std::unordered_set <BasicBlock*> lpHeaders;

    if (numAccessed <= 0)
	return;

    LoopInfo &lpi = gccfg->p->getAnalysis<LoopInfo>(*func);

    // Go through the top-level loops TODO: Nested loops should be handled later
    for (LoopInfo::iterator li = lpi.begin(), le = lpi.end(); li != le; ++li) {
	Loop *lp = *li;
	BasicBlock *lpHeader = lp->getHeader();
	assert(lpHeaders.find(lpHeader) == lpHeaders.end());
	lpHeaders.insert(lpHeader);
    }

    for (std::unordered_set<BasicBlock *>::iterator li = lpHeaders.begin(), le = lpHeaders.end(); li != le; ++li) {
	BasicBlock *lpHeader = *li;
	DEBUG(dbgs() << "####################Loop starts#######################\n");
	DEBUG(dbgs() << "###########################################\n");
	simulate(lpHeader);
	DEBUG(dbgs() << "###########################################\n");
	while (!gccfg->converge()) {
	    gccfg->resetForNextIteration();
	    DEBUG(dbgs() << "###########################################\n");
	    simulate(lpHeader);
	    DEBUG(dbgs() << "###########################################\n");
	}


	// Categorize the call instructions
	categorize(lpHeader);

	DEBUG(dbgs() << "####################Loop ends#######################\n");
	// Reset the graph for the next analysis
	gccfg->resetForNextAnalysis();
    }
}



void GCCFGFunction::simulate(BasicBlock *lpHeader) {
    unsigned long numPredecessors;
    std::stack <GCCFGBasicBlock *> s;
    std::unordered_set<GCCFGBasicBlock *> discovered;
    std::unordered_set<GCCFGBasicBlock *> lpGBBs;

    if (accessNum < 0)
	accessNum = 0;
    else 
	accessNum++;

    assert(accessNum == 0);


    LoopInfo &lpi = gccfg->p->getAnalysis<LoopInfo>(*func);
    Loop *lp = lpi.getLoopFor(lpHeader);

    // Get basic blocks of the current loop
    for (Loop::block_iterator bi = lp->block_begin(), be = lp->block_end(); bi != be; ++bi) {
	lpGBBs.insert(bbMap[*bi]);
    }

    // Get the header block of the current loop
    GCCFGBasicBlock* gHeader = bbMap[lpHeader];
    Instruction *lpHeaderFirstInst = lpHeader->getFirstNonPHI();


    // Label the header block as discovered
    discovered.insert(gHeader);

    // Simulate the header block of the current loop
    for (pred_iterator pi = pred_begin(lpHeader), pe = pred_end(lpHeader); pi != pe; ++pi) {
	BasicBlock *pred = *pi;

	DominatorTree &dt = gccfg->p->getAnalysis<DominatorTreeWrapperPass>(*func).getDomTree();
	if (dt.dominates(lpHeaderFirstInst, pred) || pred == lpHeader) { // Found a back edge
	    GCCFGBasicBlock *gTemp = bbMap[pred];
	    RegionStatus regStat = gTemp->getOutput();
	    // Simulate through the header block
	    gHeader->simulateThrough(regStat);
	    // Push the neighbouring nodes of the entry block to the stack
	    for (unsigned long i = 0; i < gHeader->getNumSuccessors(); ++i) {
		GCCFGBasicBlock *gSucc =  gHeader->getSuccessor(i);
		s.push(gSucc);
	    }
	    break;
	}
    }


    // Traverse the CFG of this function with DFS
    while (!s.empty()) {
	// Pick up the node at stack top
	GCCFGBasicBlock *v = s.top();
	s.pop();
	//DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << "\n");
	if (discovered.find(v) == discovered.end() && lpGBBs.find(v) != lpGBBs.end()) {
	    BasicBlock *bb = v->getLLVMBasicBlock();
	    Instruction *firstInst = bb->getFirstNonPHI();
	    // Label the node as discovered
	    discovered.insert(v);

	    numPredecessors = v->getNumPredecessors();
	    assert(numPredecessors >= 1);
	    // Set up the input region status of the current basic block
	    RegionStatus regStat(numRegions);
	    if (numPredecessors == 1) {
		RegionStatus temp = v->getPredecessor(0)->getOutput();
		regStat = temp;
	    } else {
		RegionStatus temp1(numRegions);
		// Join the output region status of all the predecessors if there are more than one predecessor
		bool unitialized = true;
		for (unsigned long i = 0; i < v->getNumPredecessors(); ++i) {
		    GCCFGBasicBlock *pred = v->getPredecessor(i);
		    BasicBlock *pred_bb = pred->getLLVMBasicBlock();
		    DominatorTree &dt = gccfg->p->getAnalysis<DominatorTreeWrapperPass>(*func).getDomTree();
		    //Instruction * pred_first_inst = pred_bb->getFirstNonPHI();
		    //std::cerr << std::hex << bb->getName().str() << " " << ( dt.dominates(firstInst, pred_bb)  ? " dominates " : " does not dominate ") << pred_bb->getName().str() << " "  << "\n";
		    // Ignore back edges 
		    if (!dt.dominates(firstInst, pred_bb) && (pred_bb != bb)) {
			RegionStatus temp2 = pred->getOutput();
			if (unitialized) {
			    temp1 = temp2;
			    unitialized = false;
			    continue;
			}
			temp1 = temp1 && temp2;
		    }
		}
		regStat = temp1;
		if (regStat.empty()) {
		    //regStat[regionID].clear();
		    regStat[regionID].insert(func);
		} else {
		    if(regStat[regionID].size() != 1)
			assert (false);
		    if(*regStat[regionID].begin() != func)
			assert(false);
		}
	    }

	    // Simulate the current basic block
	    v->simulateThrough(regStat);
	    //DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << " " << v->getNumSuccessors() << "\n");
	    // Push all the neighbors of v to the stack
	    for (unsigned long i = 0; i < v->getNumSuccessors(); ++i) {
		s.push(v->getSuccessor(i));
	    }

	}

    }
}

RegionStatus GCCFGFunction::simulateThrough(RegionStatus &rs) {
    unsigned long numPredecessors;
    unsigned long regionID;
    //AnalysisType analysisType;
    std::stack <GCCFGBasicBlock *> s;
    std::unordered_set<GCCFGBasicBlock *> discovered;
    GCCFGBasicBlock *entryBB = getEntryBlock();

    if (accessNum < 0)
	accessNum = 0;
    else 
	accessNum++;

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;

    //if (name == root->getName())
	DEBUG(dbgs() << name << " accessed for the " << accessNum << " time\n");

    // If this function does not return, then return the input status (before updating the region this function is mapped to)
    if (returnBBs.size() == 0) {
	output = input;
	return output;
    }

    // Set this function as the current function in the region it maps to
    regionID = overlayMap[func];
    //input[regionID] = func;
    input[regionID].clear();
    input[regionID].insert(func);
    //if (name == root->getName())
    //std::cerr << name << " input: "<< input << "\n";
    if (numCalls <= 0) {
	output = input;
	//if (name == root->getName())
	//std::cerr << name << " output: " << output << "\n";
	return output;
    }

    // Label the entry block as discovered
    discovered.insert(entryBB);
    // Simulate the entry block
    entryBB->simulateThrough(input);
    // Push the neighbouring nodes of the entry block to the stack
    for (unsigned long i = 0; i < entryBB->getNumSuccessors(); ++i) {
	s.push(entryBB->getSuccessor(i));
    }
    // Traverse the CFG of this function with DFS
    while (!s.empty()) {
	// Pick up the node at stack top
	GCCFGBasicBlock *v = s.top();
	s.pop();
	//DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << "\n");
	if (discovered.find(v) == discovered.end()) {
	    // The flag that shows if all its predecessors have been visited (excluding back edges)
	    bool ready = true;

	    BasicBlock *bb = v->getLLVMBasicBlock();
	    Instruction *firstInst = bb->getFirstNonPHI();

	    numPredecessors = v->getNumPredecessors();
	    assert(numPredecessors >= 1);
	    // Set up the input region status of the current basic block
	    RegionStatus regStat(numRegions);
	    if (numPredecessors == 1) {
		GCCFGBasicBlock *pred = v->getPredecessor(0);
		assert(discovered.find(pred) != discovered.end());
		RegionStatus temp = pred->getOutput();
		regStat = temp;
	    } else {
		RegionStatus temp1(numRegions);
		// Join the output region status of all the predecessors if there are more than one predecessor
		bool unitialized = true;
		for (unsigned long i = 0; i < v->getNumPredecessors(); ++i) {
		    GCCFGBasicBlock *pred = v->getPredecessor(i);
		    BasicBlock *pred_bb = pred->getLLVMBasicBlock();


		    // Ignore back edges 
		    DominatorTree &dt = gccfg->p->getAnalysis<DominatorTreeWrapperPass>(*func).getDomTree();
		    if (dt.dominates(firstInst, pred_bb) || (pred_bb == bb)) 
			continue;

		    // Check if any forward predecessors have not been visited
		    if (discovered.find(pred) == discovered.end()) {
			ready = false;
			break;;
		    }


		    RegionStatus temp2 = pred->getOutput();
		    if (unitialized) {
			temp1 = temp2;
			unitialized = false;
			continue;
		    }
		    temp1 = temp1 && temp2;
		}


		// Do not handle this basic block until all its forward predecessors have been visited
		if (!ready)
		    continue;
		regStat = temp1;
		assert(!regStat.empty());
	    }
	    // Label the node as discovered
	    discovered.insert(v);

	    // Simulate the current basic block
	    v->simulateThrough(regStat);
	    //DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << " " << v->getNumSuccessors() << "\n");
	    // Push all the neighbors of v to the stack
	    for (unsigned long i = 0; i < v->getNumSuccessors(); ++i) {
		s.push(v->getSuccessor(i));
	    }

	}

    }
    // Set the result of joining the output region statuses of all the basic blocks with return instruction as the output region status of this function

    RegionStatus regStat = returnBBs[0]->getOutput();
    for (unsigned long i = 1; i < returnBBs.size(); ++i) {
	RegionStatus temp = returnBBs[i]->getOutput();
	regStat = regStat && temp;
    }
    output = regStat;

    //if (name == root->getName())
	//std::cerr << name << " output: " << output << "\n";

    return output;
}

RegionStatus GCCFGFunction::simulate(RegionStatus &rs) {
    unsigned long numPredecessors;
    unsigned long regionID;
    //AnalysisType analysisType;
    std::stack <GCCFGBasicBlock *> s;
    std::unordered_set<GCCFGBasicBlock *> discovered;
    GCCFGBasicBlock *entryBB = getEntryBlock();

    if (accessNum < 0)
	accessNum = 0;
    else 
	accessNum++;

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;

    //if (name == root->getName())
	DEBUG(dbgs() << name << " accessed for the " << accessNum << " time\n");

    // If this function does not return, then return the input status (before updating the region this function is mapped to)
    if (returnBBs.size() == 0) {
	output = input;
	return output;
    }

    // Set this function as the current function in the region it maps to
    regionID = overlayMap[func];
    //input[regionID] = func;
    input[regionID].clear();
    input[regionID].insert(func);
    //if (name == root->getName())
    //std::cerr << name << " input: " << input << "\n";

    // Return if this function does not call any user functions
    if (numCalls <= 0) {
	output = input;
	//if (name == root->getName())
	//std::cerr << name << " output: " << output << "\n";
	return output;
    }

    // Label the entry block as discovered
    discovered.insert(entryBB);

    // Simulate the entry block
    entryBB->simulateThrough(input);
    // Push the neighbouring nodes of the entry block to the stack
    for (unsigned long i = 0; i < entryBB->getNumSuccessors(); ++i) {
	s.push(entryBB->getSuccessor(i));
    }
    // Traverse the CFG of this function with DFS
    while (!s.empty()) {
	// Pick up the node at stack top
	GCCFGBasicBlock *v = s.top();
	s.pop();
	//DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << "\n");
	if (discovered.find(v) == discovered.end()) {
	    // The flag that shows if all its predecessors have been visited (excluding back edges)
	    bool ready = true;

	    BasicBlock *bb = v->getLLVMBasicBlock();
	    Instruction *firstInst = bb->getFirstNonPHI();

	    numPredecessors = v->getNumPredecessors();
	    assert(numPredecessors >= 1);
	    // Set up the input region status of the current basic block
	    RegionStatus regStat(numRegions);
	    if (numPredecessors == 1) {
		GCCFGBasicBlock *pred = v->getPredecessor(0);
		assert(discovered.find(pred) != discovered.end());
		RegionStatus temp = pred->getOutput();
		regStat = temp;
	    } else {
		RegionStatus temp1(numRegions);
		// Join the output region status of all the predecessors if there are more than one predecessor
		bool unitialized = true;
		for (unsigned long i = 0; i < v->getNumPredecessors(); ++i) {
		    GCCFGBasicBlock *pred = v->getPredecessor(i);
		    BasicBlock *pred_bb = pred->getLLVMBasicBlock();
		    // Check if any forward predecessors have not been visited
		    if (discovered.find(pred) == discovered.end()) {
			DominatorTree &dt = gccfg->p->getAnalysis<DominatorTreeWrapperPass>(*func).getDomTree();
			// Ignore back edges 
			if (!dt.dominates(firstInst, pred_bb) && !(pred_bb == bb)) {
			    ready = false;
			    break;
			}
		    }


		    RegionStatus temp2 = pred->getOutput();
		    if (unitialized) {
			temp1 = temp2;
			unitialized = false;
			continue;
		    }
		    temp1 = temp1 && temp2;
		}


		// Do not handle this basic block until all its forward predecessors have been visited
		if (!ready)
		    continue;
		regStat = temp1;
		assert(!regStat.empty());
	    }
	    // Label the node as discovered
	    discovered.insert(v);

	    // Simulate the current basic block
	    v->simulateThrough(regStat);
	    //DEBUG(dbgs() << "\t" << func->getName() << " " << v->getLLVMBasicBlock()->getName() << " " << v->getNumSuccessors() << "\n");
	    // Push all the neighbors of v to the stack
	    for (unsigned long i = 0; i < v->getNumSuccessors(); ++i) {
		s.push(v->getSuccessor(i));
	    }

	}

    }
    // Set the result of joining the output region statuses of all the basic blocks with return instruction as the output region status of this function

    RegionStatus regStat = returnBBs[0]->getOutput();
    for (unsigned long i = 1; i < returnBBs.size(); ++i) {
	RegionStatus temp = returnBBs[i]->getOutput();
	regStat = regStat && temp;
    }
    output = regStat;

    //if (name == root->getName())
	//std::cerr << name << " output: " << output << "\n";

    return output;
}

bool GCCFGFunction::converge() {
    long size = getAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	if (*previousInputs[i] != *inputs[i]) {
	    return false;
	}
	if (*previousOutputs[i] != *outputs[i]) {
	    return false;
	}
    }

    for (auto ji = gBBs.begin(), je = gBBs.end(); ji != je; ++ji) {
	GCCFGBasicBlock *gBB = *ji;
	    if (!gBB->converge()) {
		//DEBUG(dbgs() << "\t" << gFunc->getName() << " : " << *gInst->getLLVMInstruction() << "\n");
		return false;
	}
    }
    return true;
}

void GCCFGFunction::resetForNextIteration() {
    long size = getAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	*previousInputs[i] = *inputs[i];
	*previousOutputs[i] = *outputs[i];
    }

    for (auto ji = gBBs.begin(), je = gBBs.end(); ji != je; ++ji) {
	GCCFGBasicBlock *gBB = *ji;
	gBB->resetForNextIteration();
    }

    // Reset the number of times functions are visited. It must be done at the last step
   resetAccessNumber();
}

void GCCFGFunction::resetForNextAnalysis() {
    //unsigned long size = inputs.size();
    long size = getAccessNumber()+1;
    // Reset the region status of this function
    for (long i = 0; i < size; ++i) {
	previousInputs[i]->reset();
	previousOutputs[i]->reset();
	inputs[i]->reset();
	outputs[i]->reset();
    }

    // Reset the region status of its basic blocks
    for (auto ii = gBBs.begin(), ie = gBBs.end(); ii != ie; ++ii) {
	GCCFGBasicBlock *gBB = *ii;
	gBB->resetForNextAnalysis();
    }
    // Reset the number of times functions are visited. It must be done at the last step
    resetAccessNumber();

}

void GCCFGFunction::categorize(BasicBlock *lpHeader) {
    // Skip dead code
    if (numAccessed <= 0)
	return;
    LoopInfo &lpi = gccfg->p->getAnalysis<LoopInfo>(*func);
    Loop *lp = lpi.getLoopFor(lpHeader);
    std::unordered_set<GCCFGBasicBlock *> lpGBBs;

    DEBUG(dbgs() << name << "\n");

    // Get basic blocks of the current loop
    for (Loop::block_iterator bi = lp->block_begin(), be = lp->block_end(); bi != be; ++bi) {
	lpGBBs.insert(bbMap[*bi]);
    }

    for (auto ji = lpGBBs.begin(), je = lpGBBs.end(); ji != je; ++ji) {
	GCCFGBasicBlock *gBB = *ji;

	DEBUG(dbgs() << "\t" << gBB->getName() << "\n");
	std::vector <GCCFGInstruction *> gInsts = gBB->getInstructions();
	for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
	    GCCFGInstruction *gInst = *ki;
	    gInst->categorize(lpHeader);
	}
    }
}

void GCCFGFunction::printCategory() {
    if (numAccessed <= 0)
	return;
    DEBUG(dbgs() << name << "\n");
    for (auto ii = gBBs.begin(), ie = gBBs.end(); ii != ie; ++ii) {
	GCCFGBasicBlock *gBB = *ii;
	gBB->printCategory();
    }
}



/* Class GCCFGFunction definition ends */

/* Class GCCFGBasicBlock definition begins */

GCCFGBasicBlock::GCCFGBasicBlock(BasicBlock *bb) {
    this->bb = bb;
    name = bb->getName();
}

BasicBlock *GCCFGBasicBlock::getLLVMBasicBlock() {
    return bb;
}

std::string GCCFGBasicBlock::getName() {
    return name;
}

long GCCFGBasicBlock::getFunctionAccessNumber() {
    Function *func = bb->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    return gFunc->getAccessNumber();
}

unsigned long GCCFGBasicBlock::getNumPredecessors() {
    return preds.size();
}

GCCFGBasicBlock* GCCFGBasicBlock::getPredecessor(unsigned long idx) {
    return preds[idx];
}

void GCCFGBasicBlock::addPredecessor(GCCFGBasicBlock *gbb) {
    preds.push_back(gbb);
}

unsigned long GCCFGBasicBlock::getNumSuccessors() {
    return succs.size();
}

GCCFGBasicBlock* GCCFGBasicBlock::getSuccessor(unsigned long idx) {
    return succs[idx];
}

void GCCFGBasicBlock::addSuccessor(GCCFGBasicBlock *gbb) {
    succs.push_back(gbb);
}

std::vector <GCCFGInstruction *> GCCFGBasicBlock::getInstructions() {
    return gInsts;
}

void GCCFGBasicBlock::addInstruction(GCCFGInstruction * gInst) {
    return gInsts.push_back(gInst);
}

void GCCFGBasicBlock::runOnce() {
    for (unsigned long i = 0; i < gInsts.size(); ++i) {
	gInsts[i]->runOnce();
    }
}

void GCCFGBasicBlock::initialize() {
    Function *func = bb->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    long numAccessed = gFunc->getNumberAccessed();
    for (long i = 0; i < numAccessed; ++i) {
	inputs.push_back(new RegionStatus(numRegions));
	outputs.push_back(new RegionStatus(numRegions));
	previousInputs.push_back(new RegionStatus(numRegions));
	previousOutputs.push_back(new RegionStatus(numRegions));
    }

    for (std::vector <GCCFGInstruction *>::iterator ii = gInsts.begin(), ie = gInsts.end(); ii != ie; ++ii) {
	GCCFGInstruction * gInst = *ii;
	gInst->initialize();
    }
}

void GCCFGBasicBlock::printCategory() {
    DEBUG(dbgs() << "\t " << name << "\n");
    for (auto ii = gInsts.begin(), ie = gInsts.end(); ii != ie; ++ii) {
	GCCFGInstruction *gInst = *ii;
	gInst->printCategory();
    }
}

RegionStatus GCCFGBasicBlock::getOutput() {
    long accessNum = getFunctionAccessNumber();
    if (accessNum >= (long)outputs.size()) {
	//DEBUG(dbgs() << accessNum <<  " " << outputs.size() <<"\n");
	//assert(accessNum == 0);
	RegionStatus temp(numRegions);
	return temp;
    }
    return *outputs[accessNum];
}

RegionStatus GCCFGBasicBlock::simulate(RegionStatus &rs) {
    unsigned long numCallInsts = gInsts.size();
    long accessNum = getFunctionAccessNumber();

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;


    std::string func_name = bb->getParent()->getName();
    //if (bb->getParent()->getName() == root->getName()) {
	DEBUG(dbgs() << "\t" << func_name << "." << name << "\tpredecessors: ");
	for (unsigned long i = 0; i < preds.size(); ++i) {
	    RegionStatus temp = preds[i]->getOutput();
	    //std::cerr << func_name << "." << preds[i]->name << " [" << temp << "] ";
	}
	DEBUG(dbgs() << "\n");
	//std::cerr << "\t\t" << func_name << "." << name << " input [" << input << "]\n";
    //}

    if (numCallInsts > 0) {
	gInsts[0]->simulate(input);
	for (unsigned long i = 1; i < numCallInsts; ++i) {
	    RegionStatus temp = gInsts[i-1]->getOutput();
	    gInsts[i]->simulate(temp);
	}
	RegionStatus temp = gInsts[numCallInsts-1]->getOutput();
	output = temp;
    }
    else {
	output = input;
    }

    //if (bb->getParent()->getName() == root->getName()) 
	//std::cerr << "\t\t" << func_name << "." << name << " output [" << output << "]\n";
    return output;
}

RegionStatus GCCFGBasicBlock::simulateThrough(RegionStatus &rs) {
    unsigned long numCallInsts = gInsts.size();
    long accessNum = getFunctionAccessNumber();

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;


    std::string func_name = bb->getParent()->getName();
    //if (bb->getParent()->getName() == root->getName()) {
	DEBUG(dbgs() << "\t" << func_name << "." << name << "\tpredecessors: ");
	for (unsigned long i = 0; i < preds.size(); ++i) {
	    RegionStatus temp = preds[i]->getOutput();
	    //std::cerr << func_name << "." << preds[i]->name << " [" << temp << "] ";
	}
	DEBUG(dbgs() << "\n");
	//std::cerr << "\t\t" << func_name << "." << name << " input [" << input << "]\n";
    //}

    if (numCallInsts > 0) {
	gInsts[0]->simulateThrough(input);
	for (unsigned long i = 1; i < numCallInsts; ++i) {
	    RegionStatus temp = gInsts[i-1]->getOutput();
	    gInsts[i]->simulateThrough(temp);
	}
	RegionStatus temp = gInsts[numCallInsts-1]->getOutput();
	output = temp;
    }
    else {
	output = input;
    }

    //if (bb->getParent()->getName() == root->getName()) 
	//std::cerr << "\t\t" << func_name << "." << name << " output [" << output << "]\n";
    return output;
}

bool GCCFGBasicBlock::converge() {
    long size = getFunctionAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	if (*previousInputs[i] != *inputs[i]) {
	    return false;
	}
	if (*previousOutputs[i] != *outputs[i]) {
	    return false;
	}
    }
    for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
	GCCFGInstruction *gInst = *ki;
	if (!gInst->converge()) {
	    //DEBUG(dbgs() << "\t" << gFunc->getName() << " : " << *gInst->getLLVMInstruction() << "\n");
	    return false;
	}
    }
    return true;
}

void GCCFGBasicBlock::resetForNextIteration() {
    long size = getFunctionAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	*previousInputs[i] = *inputs[i];
	*previousOutputs[i] = *outputs[i];
    }

    for (auto ki = gInsts.begin(), ke = gInsts.end(); ki != ke; ++ki) {
	GCCFGInstruction *gInst = *ki;
	gInst->resetForNextIteration();
    }
}

void GCCFGBasicBlock::resetForNextAnalysis() {
    //unsigned long num = inputs.size();
    long size = getFunctionAccessNumber()+1;
    // Reset the region status of this basic block
    for (long i = 0; i < size; ++i) {
	previousInputs[i]->reset();
	previousOutputs[i]->reset();
	inputs[i]->reset();
	outputs[i]->reset();
    }


    // Reset the region status of its instructions
    for (auto ii = gInsts.begin(), ie = gInsts.end(); ii != ie; ++ii) {
	GCCFGInstruction *gInst = *ii;
	gInst->resetForNextAnalysis();
    }
}

AnalysisType GCCFGBasicBlock::getAnalysisType() {
    Function *func = bb->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    return gFunc->getAnalysisType();
}

/* Class GCCFGBasicBlock definition ends */


/* Class GCCFGInstruction definition begins */

GCCFGInstruction::GCCFGInstruction(Instruction *inst) {
    this->inst = inst;
}

Instruction *GCCFGInstruction::getLLVMInstruction() {
    return inst;
}

void GCCFGInstruction::runOnce() {
    CallInst *callInst = cast <CallInst>(inst);
    Function *caller = callInst->getParent()->getParent();
    Function *callee = callInst->getCalledFunction();
    GCCFGFunction *gCallee = funcMap[callee];

    // Simulate the execution of the called function  if the called function is not self-recursive
    if (caller != callee) 
	gCallee->runOnce();
}

void GCCFGInstruction::initialize() {
    Function *func = inst->getParent()->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    long numAccessed = gFunc->getNumberAccessed();
    for (long i = 0; i < numAccessed; ++i) {
	inputs.push_back(new RegionStatus(numRegions));
	intermediates.push_back(new RegionStatus(numRegions));
	outputs.push_back(new RegionStatus(numRegions));
	previousInputs.push_back(new RegionStatus(numRegions));
	previousOutputs.push_back(new RegionStatus(numRegions));
	calleeAnalysis.push_back(Uncategorized);
	callerAnalysis.push_back(Uncategorized);
    }
    finalCalleeAnalysis = Uncategorized;
    finalCallerAnalysis = Uncategorized;
}




long GCCFGInstruction::getFunctionAccessNumber() {
    Function *func = inst->getParent()->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    return gFunc->getAccessNumber();
}

RegionStatus GCCFGInstruction::getInput() {
    long accessNum = getFunctionAccessNumber();
    return *inputs[accessNum];
}

RegionStatus GCCFGInstruction::getOutput() {
    long accessNum = getFunctionAccessNumber();
    return *outputs[accessNum];
}

AnalysisType GCCFGInstruction::getAnalysisType() {
    Function *func = inst->getParent()->getParent();
    GCCFGFunction *gFunc = funcMap[func];
    return gFunc->getAnalysisType();
}

RegionStatus GCCFGInstruction::simulate(RegionStatus &rs) {
    unsigned long callerRegionID;
    long accessNum = getFunctionAccessNumber();

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &intermediate = *intermediates[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;


    CallInst *callInst = cast <CallInst>(inst);
    Function *callee = callInst->getCalledFunction();
    Function *caller = inst->getParent()->getParent();
    callerRegionID = overlayMap[caller];
    GCCFGFunction *gCallee = funcMap[callee];

    //if (inst->getParent()->getParent()->getName() == root->getName()) {
    DEBUG(dbgs() << "\t\t\t" << *inst << "\n"); 
    //std::cerr << "\t\t\t\tinput: " << input << "\n";
    //}

    // If the function call is self recursive, check if it is the only function call within the caller function
    if (callee == caller) {
	//GCCFGFunction *gCaller = funcMap[caller];
	//assert(gCaller->getNumberOfCalls() == 1);
	intermediate = input;
	//intermediate[callerRegionID].clear();
	//intermediate[callerRegionID].insert(caller);
	output = intermediate;
    } else {
	// Simulate the execution of the called function 
	RegionStatus temp = gCallee->simulate(input);
	// Get the output region status after the callee is brought to the SPM and executed
	intermediate = temp;
	// Get the output region status after the caller is brought to the SPM
	//callerRegionID = overlayMap[caller];
	output = intermediate;
	//output[callerRegionID] = caller; 
	output[callerRegionID].clear();
	output[callerRegionID].insert(caller);
    }

    //if (inst->getParent()->getParent()->getName() == root->getName()) {
    //std::cerr << "\t\t\t\tintermediate: " << intermediate << "\n";
    //std::cerr << "\t\t\t\toutput: " << output << "\n";
    //}
    return output;
}

RegionStatus GCCFGInstruction::simulateThrough(RegionStatus &rs) {
    unsigned long callerRegionID;
    long accessNum = getFunctionAccessNumber();

    RegionStatus &input = *inputs[accessNum];
    RegionStatus &intermediate = *intermediates[accessNum];
    RegionStatus &output = *outputs[accessNum];

    input = rs;


    CallInst *callInst = cast <CallInst>(inst);
    Function *callee = callInst->getCalledFunction();
    Function *caller = inst->getParent()->getParent();
    GCCFGFunction *gCallee = funcMap[callee];

    //if (inst->getParent()->getParent()->getName() == root->getName()) {
    DEBUG(dbgs() << "\t\t\t" << *inst << "\n"); 
    //std::cerr << "\t\t\t\tinput: " << input << "\n";
    //}

    // If the function call is self recursive, check if it is the only function call within the caller function
    if (callee == caller) {
	//GCCFGFunction *gCaller = funcMap[caller];
	//assert(gCaller->getNumberOfCalls() == 1);
	intermediate = input;
	//intermediate[callerRegionID].clear();
	//intermediate[callerRegionID].insert(caller);
	output = intermediate;
    } else {
	// Simulate the execution of the called function 
	RegionStatus temp = gCallee->simulateThrough(input);
	// Get the output region status after the callee is brought to the SPM and executed
	intermediate = temp;
	// Get the output region status after the caller is brought to the SPM
	callerRegionID = overlayMap[caller];
	output = intermediate;
	//output[callerRegionID] = caller; 
	output[callerRegionID].clear();
	output[callerRegionID].insert(caller);
    }

    //if (inst->getParent()->getParent()->getName() == root->getName()) {
    //std::cerr << "\t\t\t\tintermediate: " << intermediate << "\n";
    //std::cerr << "\t\t\t\toutput: " << output << "\n";
    //}
    return output;
}

void GCCFGInstruction::resetForNextIteration() {
    //unsigned long size = inputs.size();
    long size = getFunctionAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	*previousInputs[i] = *inputs[i];
	*previousOutputs[i] = *outputs[i];
	//DEBUG(dbgs() << "\t\tinput : " << i << " "; previousInputs[i]->print(); errs() << "\t"; inputs[i]->print();  errs() << "\n");
	//DEBUG(dbgs() << "\t\toutput : " << i << " "; previousOutputs[i]->print(); errs() << "\t"; outputs[i]->print(); errs() << "\n"); 
    }
}

void GCCFGInstruction::resetForNextAnalysis() {
    //unsigned long size = inputs.size();
    long size = getFunctionAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	previousInputs[i]->reset();
	previousOutputs[i]->reset();
	inputs[i]->reset();
	intermediates[i]->reset();
	outputs[i]->reset();
    }
}

bool GCCFGInstruction::converge() {
    //unsigned long size = inputs.size();
    long size = getFunctionAccessNumber()+1;
    for (long i = 0; i < size; ++i) {
	if (*previousInputs[i] != *inputs[i]) {
	    //DEBUG(dbgs() << "\t\tinput : " << i << " "; previousInputs[i]->print(); errs() << " "; inputs[i]->print();  errs() << "\n");
	    //DEBUG(dbgs() << "\t\toutput : " << i << " "; previousOutputs[i]->print(); errs() << " "; outputs[i]->print(); errs() << "\n"); 
	    return false;
	}
	if (*previousOutputs[i] != *outputs[i]) {
	    //DEBUG(dbgs() << "\t\tinput : " << i << " "; previousInputs[i]->print(); errs() << " "; inputs[i]->print();  errs() << "\n");
	    //DEBUG(dbgs() << "\t\toutput : " << i << " "; previousOutputs[i]->print(); errs() << " "; outputs[i]->print(); errs() << "\n"); 
	    return false;
	}
    }
    return true;
}



void GCCFGInstruction::categorize() {
    //AnalysisType analysisType = getAnalysisType();
    // The number of times the instruction is traversed have not been reset at this point
    Function *caller = inst->getParent()->getParent();
    Function *callee = (dyn_cast<CallInst>(inst))->getCalledFunction();
    GCCFGFunction *gCaller = funcMap[caller];
    GCCFGFunction *gCallee = funcMap[callee];
    unsigned long callerRegionID = gCaller->getRegionID();
    unsigned long calleeRegionID = gCallee->getRegionID();

    long size = getFunctionAccessNumber() + 1;

    // Perform the analsis on the called and the caller function of this instruction
    for (long i = 0; i < size; ++i) {
	RegionStatus &input = *inputs[i];
	RegionStatus &intermediate = *intermediates[i];

	if (input[calleeRegionID].find(callee) != input[calleeRegionID].end())
	    if (calleeAnalysis[i] == Uncategorized)
		calleeAnalysis[i] = AlwaysHit;

	if (intermediate[callerRegionID].find(caller) != intermediate[callerRegionID].end())
	    if (callerAnalysis[i] == Uncategorized)
		callerAnalysis[i] = AlwaysHit;
    }

    // Summarize the analysis result for the called function
    if (finalCalleeAnalysis == Uncategorized) {
	for (long i = 0; i < size; ++i) {
	    AnalysisResult analysisResult = calleeAnalysis[i];
	    if(analysisResult == Uncategorized) {
		finalCalleeAnalysis = Uncategorized;
		break;
	    } else if (analysisResult == FirstMiss) {
		finalCalleeAnalysis = FirstMiss;
	    } else {
		if (finalCalleeAnalysis == Uncategorized)
		    finalCalleeAnalysis = AlwaysHit;
	    }
	}
    }

    // Summarize the analysis result for the caller function
    if (finalCallerAnalysis == Uncategorized) {
	for (long i = 0; i < size; ++i) {
	    AnalysisResult analysisResult = callerAnalysis[i];
	    if(analysisResult == Uncategorized) {
		finalCallerAnalysis = Uncategorized;
		break;
	    } else if (analysisResult == FirstMiss) {
		assert(false);
	    } else {
		if (finalCallerAnalysis == Uncategorized)
		    finalCallerAnalysis = AlwaysHit;
	    }
	}
    }

    assert(finalCalleeAnalysis == Uncategorized || finalCalleeAnalysis == FirstMiss || finalCalleeAnalysis == AlwaysHit);
    assert(finalCallerAnalysis == Uncategorized || finalCallerAnalysis == AlwaysHit);
}

void GCCFGInstruction::categorize(BasicBlock *lpHeader) {
    //AnalysisType analysisType = getAnalysisType();
    // The number of times the instruction is traversed have not been reset at this point
    Function *caller = inst->getParent()->getParent();
    Function *callee = (dyn_cast<CallInst>(inst))->getCalledFunction();
    GCCFGFunction *gCaller = funcMap[caller];
    GCCFGFunction *gCallee = funcMap[callee];
    unsigned long callerRegionID = gCaller->getRegionID();
    unsigned long calleeRegionID = gCallee->getRegionID();
    GCCFG* gccfg = gCaller->getParent();

    long size = getFunctionAccessNumber() + 1;



    // Perform the analsis on the called and the caller function of this instruction
    for (long i = 0; i < size; ++i) {
	RegionStatus &input = *inputs[i];
	RegionStatus &intermediate = *intermediates[i];

	if (input[calleeRegionID].find(callee) != input[calleeRegionID].end())
	    if (calleeAnalysis[i] == Uncategorized)
		calleeAnalysis[i] = FirstMiss;

	if (intermediate[callerRegionID].find(caller) != intermediate[callerRegionID].end())
	    if (callerAnalysis[i] == Uncategorized)
		callerAnalysis[i] = FirstMiss;
    }

    DEBUG(dbgs() << "\t\t" << *inst << "\n");
    DEBUG(dbgs() << "\t\t\tcalleeAnalysis: ");
    for (long i = 0; i < size; ++i) 
	DEBUG(dbgs() << calleeAnalysis[i] << " ");
    DEBUG(dbgs() << "\n");


    DEBUG(dbgs() << "\t\t\tcallerAnalysis: ");
    for (long i = 0; i < size; ++i) 
	DEBUG(dbgs() << callerAnalysis[i] << " ");
    DEBUG(dbgs() << "\n");

    // Summarize the analysis result for the called function
    if (finalCalleeAnalysis == Uncategorized) {
	for (long i = 0; i < size; ++i) {
	    AnalysisResult analysisResult = calleeAnalysis[i];
	    if(analysisResult == Uncategorized) {
		finalCalleeAnalysis = Uncategorized;
		break;
	    } else if (analysisResult == FirstMiss) {
		finalCalleeAnalysis = FirstMiss;
	    } else {
		if (finalCalleeAnalysis == Uncategorized)
		    finalCalleeAnalysis = AlwaysHit;
	    }
	}
	if (finalCalleeAnalysis == FirstMiss) {

	    if (gccfg->firstMissCalls.find(inst) == gccfg->firstMissCalls.end()) {
		gccfg->firstMissCalls[inst] = lpHeader;
	    }
	}

    }

    // Summarize the analysis result for the caller function
    if (finalCallerAnalysis == Uncategorized) {
	for (long i = 0; i < size; ++i) {
	    AnalysisResult analysisResult = callerAnalysis[i];
	    if(analysisResult == Uncategorized) {
		finalCallerAnalysis = Uncategorized;
		break;
	    } else if (analysisResult == FirstMiss) {
		//DEBUG(dbgs() << caller->getName() << "." << inst->getParent()->getName() << ":\t" << *inst << "\n");
		assert(false);
	    } else {
		if (finalCallerAnalysis == Uncategorized)
		    finalCallerAnalysis = AlwaysHit;
	    }
	}
    }


    assert(finalCalleeAnalysis == Uncategorized || finalCalleeAnalysis == FirstMiss || finalCalleeAnalysis == AlwaysHit);

    assert(finalCallerAnalysis == Uncategorized || finalCallerAnalysis == AlwaysHit);
}


void GCCFGInstruction::printCategory() {
    //size_t size = inputs.size();
    long size = getFunctionAccessNumber()+1;
    errs() << "\t\t" << *inst << "\n";

    errs() << "\t\tinputs (" << size << "):\n ";
    for (long i = 0; i < size; ++i) {
	std::cerr << "\t\t\t" << *inputs[i] << "\n";
    }
    errs() << "\n";
    errs() << "\t\tintermediates (" << size << "):\n ";
    for (long i = 0; i < size; ++i) {
	std::cerr << "\t\t\t" << *intermediates[i] << "\n";
    }
    errs() << "\n";
    errs() << "\t\toutputs (" << size << "):\n ";
    for (long i = 0; i < size; ++i) {
	std::cerr << "\t\t\t" << *outputs[i] << "\n";
    }
    errs() << "\n";

    errs() << "\t\t\tcallee:";
    for (long i = 0; i < size; ++i) {
	errs() << " " << calleeAnalysis[i];
    }
    errs() << " final: " << finalCalleeAnalysis <<  "\n";
    errs() << "\t\t\tcaller:";
    for (long i = 0; i < size; ++i) {
	errs() << " " << callerAnalysis[i];
    }
    errs() << " final: " << finalCallerAnalysis <<  "\n";
}

/* Class GCCFGInstruction definition ends */


/* Class RegionStatus definition begins */

RegionStatus::RegionStatus (unsigned long numRegions) {
    regStat.resize(numRegions);
}

bool RegionStatus::empty() {
    for (unsigned long i = 0; i < numRegions; ++i) 
	if (!regStat[i].empty())
	    return false;
    return true;
}

std::unordered_set <Function *>  & RegionStatus::operator[] (unsigned long regionID) {
    return regStat[regionID];
}

bool RegionStatus::operator==  (RegionStatus rhs) {
    for (unsigned long i = 0; i < numRegions; ++i) {
	if (regStat[i] != rhs[i])
	    return false;
    }
    return true;
}

bool RegionStatus::operator!=  (RegionStatus rhs) {
    return !(*this == rhs);
}

RegionStatus & RegionStatus::operator=  (RegionStatus rhs) {
    for (unsigned long i = 0; i < numRegions; ++i) {
	regStat[i] = rhs[i];
    }
    return *this;
}

RegionStatus RegionStatus::operator&& (RegionStatus rhs) {
    RegionStatus res(numRegions);
    // If corresponding regions keep the same function, the function is kept in the result; otherwise, clear the region
    for (unsigned long i = 0; i < numRegions; ++i) {
	if (regStat[i] == rhs[i])
	    res[i] = regStat[i];
	else
	    res[i].clear();
    }
    return res;
}

RegionStatus RegionStatus::operator|| (RegionStatus rhs) {
    RegionStatus res(numRegions);
    // If corresponding regions keep the same function, the function is kept in the result; otherwise, clear the region if both are not empty
    for (unsigned long i = 0; i < numRegions; ++i) {
	if (regStat[i] == rhs[i]) {
	    res[i] = regStat[i];
	} else {
	    if (!regStat[i].empty() && !rhs[i].empty())
		res[i].clear();
	    else if (!regStat[i].empty())
		res[i] = regStat[i];
	    else 
		res[i] = rhs[i];
	}
    }
    return res;
}

void RegionStatus::reset() {
    for (unsigned long i = 0; i < numRegions; ++i) 
	regStat[i].clear();
}

/* Class RegionStatus definition ends */
