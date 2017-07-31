#ifndef __MNMT__H__
#define __MNMT__H__

#include "llvm/Analysis/CallGraph.h"
#include "llvm/IR/Module.h"


using namespace llvm;

void l2g_pointer_management_instrumentation(Module &, CallGraphNode *);
void g2l_pointer_management_instrumentation(Module &, CallGraphNode *);
void stack_frame_management_instrumentation (Module &mod, CallInst *call_inst);

#endif
