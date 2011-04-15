// -*- mode: C++ -*-
//
// Copyright (c) 2007, 2008, 2009, 2010, 2011 The University of Utah
// All rights reserved.
//
// This file is part of `csmith', a random generator of C programs.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
//   * Redistributions of source code must retain the above copyright notice,
//     this list of conditions and the following disclaimer.
//
//   * Redistributions in binary form must reproduce the above copyright
//     notice, this list of conditions and the following disclaimer in the
//     documentation and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.

#include "StatementArrayOp.h"
#include <cassert> 
#include "Common.h"
#include "Block.h"
#include "CGContext.h"
#include "CGOptions.h"
#include "Constant.h"
#include "ExpressionFuncall.h"
#include "ExpressionVariable.h"
#include "Function.h" 
#include "FunctionInvocation.h"
#include "FunctionInvocationBinary.h"
#include "VariableSelector.h"
#include "FactMgr.h"
#include "Lhs.h"
#include "SafeOpFlags.h"
#include "Error.h"
#include "PartialExpander.h"
#include "Bookkeeper.h"
#include "DepthSpec.h"
#include "StatementBreak.h"
#include "CFGEdge.h"
#include "ArrayVariable.h"
#include "random.h"

using namespace std;

///////////////////////////////////////////////////////////////////////////////

/*
 * Randomly determine the iteration over an array: initial value, 
 * increment value (negative means going backwards), given the size
 * of array
 */
void
StatementArrayOp::make_random_iter_ctrl(int size, int &init, int &incr)
{
	// We don't have to put error guards here because we are trying
	// to get pure random numbers, and in this case, we cannot get
	// errors
	init = pure_rnd_flipcoin(50) ? 0 : pure_rnd_upto(size);
	incr = pure_rnd_flipcoin(50) ? 1 : pure_rnd_upto(size) + 1; 
}

/*
 *
 */
StatementArrayOp *
StatementArrayOp::make_random(CGContext &cg_context)
{ 
	//int static g = 0;
	//int h = g++;
	bool ary_init = rnd_flipcoin(10);
	ERROR_GUARD(NULL);
	if (ary_init) { 
		return make_random_array_init(cg_context);
	}

	// save a copy of facts env and context
	FactMgr* fm = get_fact_mgr(&cg_context);
	vector<const Fact*> facts_copy = fm->global_facts;
	cg_context.get_effect_stm().clear();

	// select the array to manipulate
	ArrayVariable* av = VariableSelector::select_array(cg_context);
	//cg_context.write_var(av);
	ERROR_GUARD(NULL);
	int init, incr;
	vector<int> inits, incrs;
	vector<const Variable*> cvs;
	size_t i;
	vector<const Variable*> invalid_vars;
	for (i=0; i<av->get_dimension(); i++) {
		Variable *cv = VariableSelector::SelectLoopCtrlVar(cg_context, invalid_vars);
		assert(cv);
		invalid_vars.push_back(cv);
		assert(cg_context.read_indices(cv, fm->global_facts));
		cg_context.write_var(cv);
		make_random_iter_ctrl(av->get_sizes()[i], init, incr);
		ERROR_GUARD(NULL);
		cvs.push_back(cv);
		inits.push_back(init);
		incrs.push_back(incr);
	}
	// find the highest block in which all control vars and array variable itself are visible
	/*vector<const Variable*> tmp_vars = cvs;
	tmp_vars.push_back(av);
	Block* b = VariableSelector::lower_block_for_vars(cg_context.get_current_func()->stack, tmp_vars);*/
	Effect eff = cg_context.get_effect_stm();
	Block* b = cg_context.get_current_block();

	vector<const Variable*> body_no_read_vars;
	CGContext body_cg_context(cg_context.get_current_func(),
							  cg_context.stmt_depth,
							  cg_context.expr_depth,
							  (cg_context.flags | IN_LOOP),
							  cg_context.call_chain,
							  cg_context.curr_blk,
							  cg_context.focus_var,
							  body_no_read_vars,
							  invalid_vars,
							  cg_context.get_effect_context(),
							  cg_context.get_effect_accum()); 
	body_cg_context.focus_var = av->itemize(cvs, b);
	Block* body = Block::make_random(body_cg_context, true); 
	body_cg_context.focus_var = 0;
	cg_context.add_effect(*body_cg_context.get_effect_accum());
	StatementArrayOp* sa = new StatementArrayOp(av, cvs, inits, incrs, body);

	// flow analysis:
	// if the control reached the post-point of a loop ending with must-return statement, it means
	// the loop is never entered. restore facts to pre-loop env
	fm->global_facts = fm->map_facts_in[body];
	if (body->must_return()) {
		fm->restore_facts(facts_copy);
	}	
	// add forward edges introduced by "break"
	for (size_t i=0; i<body->break_stms.size(); i++) {
		const StatementBreak* stm = dynamic_cast<const StatementBreak*>(body->break_stms[i]);
		fm->create_cfg_edge(stm, sa, true, false);
		merge_jump_facts(fm->global_facts, fm->map_facts_out[stm]);
	}
	// compute accumulated effect
	sa->set_accumulated_effect_after_block(eff, body, cg_context);

	return sa;
}

StatementArrayOp *
StatementArrayOp::make_random_array_init(CGContext &cg_context)
{ 
	// select the array to initialize
	//static int g = 0;
	//int h = g++;
	ArrayVariable* av =  VariableSelector::select_array(cg_context);
	ERROR_GUARD(NULL);
	cg_context.get_effect_stm().clear();
	// Select the loop control variable.
	vector<const Variable*> invalid_vars; 
	vector<const Variable*> cvs;
	ERROR_GUARD(NULL);
	// the iteration settings are simple: start from index 0, step through all members
	vector<int> inits, incrs;
	size_t i;
	cg_context.get_effect_stm().clear(); 
	FactMgr* fm = get_fact_mgr(&cg_context);
	for (i=0; i<av->get_dimension(); i++) {
		inits.push_back(0);
		incrs.push_back(1); 
		Variable *cv = VariableSelector::SelectLoopCtrlVar(cg_context, invalid_vars); 
		invalid_vars.push_back(cv);
		cvs.push_back(cv);
		assert(cg_context.read_indices(cv, fm->global_facts));
		cg_context.write_var(cv);
	}
	cg_context.write_var(av);
	
	// JYTODO: initialize only field(s) of array members if they are of type struct
	Block* b = cg_context.get_current_block()->random_parent_block();
	Expression* init = VariableSelector::make_init_value(Effect::READ, cg_context, av->type, &av->qfer, b);//Expression::make_random(cg_context, av->type, true);
	if (CGOptions::strict_const_arrays())
		assert(av->type->eType != ePointer);
	assert(init->visit_facts(fm->global_facts, cg_context));
	StatementArrayOp* sa = new StatementArrayOp(av, cvs, inits, incrs, init);
	Lhs lhs(*av);
	if (update_fact_for_assign(&lhs, init, fm->global_facts)) {
		cg_context.get_current_func()->fact_changed = true;
	}
	fm->map_stm_effect[sa] = cg_context.get_effect_stm();
	return sa;
}

/*
 *
 */
StatementArrayOp::StatementArrayOp(const ArrayVariable* av, 
				   const std::vector<const Variable*>& cvs, 
				   const std::vector<int>& inits,
				   const std::vector<int>& incrs, 
				   const Block *body)
	: Statement(eArrayOp),
	  array_var(av),
	  ctrl_vars(cvs),
	  inits(inits),
	  incrs(incrs),
	  body(body),
	  init_value(0)
{
	// Nothing else to do.
}

/*
 *
 */
StatementArrayOp::StatementArrayOp(const ArrayVariable* av, 
				   const std::vector<const Variable*>& cvs, 
				   const std::vector<int>& inits,
				   const std::vector<int>& incrs, 
				   const Expression *e)
	: Statement(eArrayOp),
	  array_var(av),
	  ctrl_vars(cvs),
	  inits(inits),
	  incrs(incrs),
	  body(0),
	  init_value(e)
{
	// Nothing else to do.
}

/*
 *
 */
StatementArrayOp::~StatementArrayOp(void)
{
	delete init_value;
	delete body;
}

void
StatementArrayOp::output_header(std::ostream& out, int& indent) const
{
	size_t i;
	for (i=0; i<array_var->get_dimension(); i++) {
		if (i > 0) {
			output_tab(out, indent);
			out << "{";
			outputln(out);
			indent++;
		}
		output_tab(out, indent);
		out << "for (";
		ctrl_vars[i]->Output(out);
		out << " = " << inits[i] << "; ";
		ctrl_vars[i]->Output(out);
		(incrs[i] > 0) ? out << " < " << array_var->get_sizes()[i] : out << " >= 0";
		out << "; ";
		ctrl_vars[i]->Output(out);
		if (CGOptions::ccomp()) {
			// ccomp disable something like g += 1, where g is volatile
			out << " = ";
			ctrl_vars[i]->Output(out);
			out << " + " << incrs[i] << ")";
		}
		else {
			out << " += " << incrs[i] << ")";
		}
		outputln(out); 
	} 
}

/*
 *
 */
void
StatementArrayOp::Output(std::ostream &out, FactMgr* fm, int indent) const
{
	size_t i;
	output_header(out, indent);

	if (body) {
		body->Output(out, fm, indent);
	}
	else if (init_value) {
		output_tab(out, indent);
		out << "{";
		outputln(out);
		// cannot assign array members to a struct constant directly, has to create a "fake" struct var first
		if (init_value->term_type == eConstant && array_var->type->eType == eStruct) {
			output_tab(out, indent+1);
			array_var->type->Output(out);
			out << " tmp = ";
			init_value->Output(out);
			out << ";";
			outputln(out);
			output_tab(out, indent+1);
			array_var->output_with_indices(out, ctrl_vars);
			out << " = tmp;";
			outputln(out);
		}
		else {
			output_tab(out, indent+1);
			array_var->output_with_indices(out, ctrl_vars);
			out << " = ";
			init_value->Output(out);
			out << ";";
			outputln(out);
		}
		output_tab(out, indent);
		out << "}";
		outputln(out);
	}
	// output the closing bracelets
	for (i=1; i<array_var->get_dimension(); i++) {
		indent--;
		output_tab(out, indent);
		out << "}";
		outputln(out);
	}
}

bool 
StatementArrayOp::visit_facts(vector<const Fact*>& inputs, CGContext& cg_context) const
{   
	// walk the iterations
	size_t i;
	for (i=0; i<array_var->get_dimension(); i++) { 
		const Variable *cv = ctrl_vars[i];  
		if (!cg_context.check_write_var(cv, inputs)) {
			return false;
		}
	}
	FactMgr* fm = get_fact_mgr(&cg_context);
	// walk the body (if there is one)
	if (body) {
		FactVec facts_copy = inputs;
		Effect eff = cg_context.get_effect_stm();
		if (!body->visit_facts(inputs, cg_context)) {
			return false;
		}
		// if body must return, means the control reached end of for-loop with pre-loop env
		if (body->must_return()) {
			inputs = facts_copy;
		} else {
			inputs = fm->map_facts_in[body];
		}
		// include the facts from "break" statements 
		// find edges leading to the end of this statement, and merge 
		vector<const CFGEdge*> edges;
		find_edges_in(edges, true, false);
		for (i=0; i<edges.size(); i++) { 
			const Statement* src = edges[i]->src;
			merge_jump_facts(inputs, fm->map_facts_out[src]);
		}
		// compute accumulated effect
		set_accumulated_effect_after_block(eff, body, cg_context);
	}
	// walk the initializing value, if this is an array initialization
	else if (init_value) {
		Lhs lhs(*array_var);
		if (!init_value->visit_facts(inputs, cg_context)) {
			return false;
		} 
		if (!lhs.visit_facts(inputs, cg_context)) {
			return false;
		}
		update_fact_for_assign(&lhs, init_value, inputs);
		fm->map_stm_effect[this] = cg_context.get_effect_stm();
	} 
	return true;
} 

///////////////////////////////////////////////////////////////////////////////

// Local Variables:
// c-basic-offset: 4
// tab-width: 4
// End:

// End of file.