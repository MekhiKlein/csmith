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

#ifdef WIN32 
#pragma warning(disable : 4786)   /* Disable annoying warning messages */
#endif

#include "VariableSelector.h"
#include <cassert>

#include "Common.h"
#include "Block.h"
#include "CGContext.h"
#include "CGOptions.h"
#include "Constant.h"
#include "Effect.h"
#include "Function.h"
#include "Type.h"
#include "Fact.h"
#include "FactMgr.h"
#include "FactPointTo.h"
#include "random.h"
#include "util.h"
#include "Lhs.h"
#include "ExpressionVariable.h"
#include "Bookkeeper.h"
#include "Filter.h"
#include "Error.h"
#include "DepthSpec.h"
#include "CFGEdge.h"
#include "ArrayVariable.h"
#include "Probabilities.h"
#include "ProbabilityTable.h"

using namespace std;

///////////////////////////////////////////////////////////////////////////////

// --------------------------------------------------------------
// static variables 
vector<Variable*> VariableSelector::AllVars; 
vector<Variable*> VariableSelector::GlobalList; 
vector<Variable*> VariableSelector::GlobalNonvolatilesList; 
bool VariableSelector::var_created = false;

class VariableSelectFilter : public Filter
{
public:
	VariableSelectFilter(const CGContext &cg_context);

	virtual ~VariableSelectFilter();

	virtual bool filter(int v) const;

private:
	const CGContext &cg_context_;

};

VariableSelectFilter::VariableSelectFilter(const CGContext &cg_context)
	: cg_context_(cg_context)
{

}

VariableSelectFilter::~VariableSelectFilter()
{

}

bool
VariableSelectFilter::filter(int v) const
{
	if (VariableSelector::scopeTable_->filter(v))
		return true;

	eVariableScope scope = VariableSelector::scopeTable_->get_value(v);
	if (scope == eParentParam) {
		Function &parent = *cg_context_.get_current_func();
		return parent.param.empty();
	}
	return false;
}

ProbabilityTable<unsigned int, eVariableScope> *VariableSelector::scopeTable_ = NULL;

void
VariableSelector::InitScopeTable()
{
	if (scopeTable_ == NULL) {  
		scopeTable_ = new ProbabilityTable<unsigned int, eVariableScope>();
		scopeTable_->add_elem(35, eGlobal);
		scopeTable_->add_elem(65, eParentLocal);
		scopeTable_->add_elem(95, eParentParam);
		scopeTable_->add_elem(100, eNewValue);
	}
}

//////////////////////////////////////////////////////////////////////////////
// --------------------------------------------------------------
static string
RandomGlobalName(void)
{
	return gensym("g_");
}

// --------------------------------------------------------------
static string
RandomLocalName(void)
{
	return gensym("l_");
}

// --------------------------------------------------------------
static string
RandomParamName(void)
{
	return gensym("p_");
}

// -------------------------------------------------------------
/*
 * generate a new variable and store into AllVars
 */
Variable *
VariableSelector::new_variable(const std::string &name, const Type *type, const Expression* init, const CVQualifiers* qfer)
{ 
	Variable *var = Variable::CreateVariable(name, type, init, qfer);
	ERROR_GUARD(NULL);
	AllVars.push_back(var);
	return var;
}

/* 
 *expand each struct field to a single variable
 */
void
VariableSelector::expand_struct_vars(vector<Variable *>& vars, const Type* type)
{
    size_t i;
    size_t len = vars.size();
    for (i=0; i<len; i++) { 
        Variable* tmpvar = vars[i];
		// don't expand virtual variables
		if (tmpvar->is_virtual()) continue;
        // don't break up a struct if it matches the given type
        if (tmpvar->type->eType == eStruct && (tmpvar->type != type)) {
            vars.erase(vars.begin() + i);
            vars.insert(vars.end(), tmpvar->field_vars.begin(), tmpvar->field_vars.end());
            i--;
            len = vars.size();
        }
    }
}

/* 
 *expand each struct field to a single variable
 */
void
VariableSelector::expand_struct_vars(vector<const Variable *>& vars, const Type* type)
{
    size_t i;
    size_t len = vars.size();
    for (i=0; i<len; i++) { 
        const Variable* tmpvar = vars[i];
		// don't expand virtual variables
		if (tmpvar->is_virtual()) continue;
        // don't break up a struct if it matches the given type
        if (tmpvar->type->eType == eStruct && (tmpvar->type != type)) {
            vars.erase(vars.begin() + i);
            vars.insert(vars.end(), tmpvar->field_vars.begin(), tmpvar->field_vars.end());
            i--;
            len = vars.size();
        }
    }
}

/* return true if a variable in the list is a pointer to type "type" */
bool
VariableSelector::has_dereferenceable_var(const vector<Variable *>& vars, const Type* type, const CGContext& cg_context)
{
	FactMgr* fm = get_fact_mgr(&cg_context);
	for (size_t i=0; i<vars.size(); i++) {
		Variable* var = vars[i];
		if (type->is_dereferenced_from(var->type) && FactPointTo::is_valid_ptr(var, fm->global_facts)) {
			return true;
		}
	}
	return false;
}

/*
 * check if a variable is eligible to be selected based on current context and 
 * read/write + volatile + const rules
 */
bool
VariableSelector::is_eligible_var(const Variable* var, int deref_level, Effect::Access access, const CGContext& cg_context)
{
	// JYTODO: integrate this into get_collective?
	//if (var->isFieldVarOf_ && var->is_array_field()) {
	//	var = var->isFieldVarOf_->get_collective();
	//}
	const Variable* coll = var->get_collective();
	if (coll != var) {
		CGContext cg_tmp(cg_context);
		FactMgr* fm = get_fact_mgr(&cg_context);
		if (!cg_tmp.read_indices(var, fm->global_facts)) {
			return false;
		}
		var = coll;
	}
	const Effect &effect_context = cg_context.get_effect_context();
	bool is_const = var->is_const_after_deref(deref_level);
	bool is_volatile = var->is_volatile_after_deref(deref_level) || var->is_volatile(); 
	 
	// ISSUE: generating "strictly conforming" programs.
	//
	// We cannot read or write a volatile if the current effect context
	// already has a side-effect.
	//
	// TODO: is this too strong for what we want?  Need a cmd-line option?
	if (is_volatile && !effect_context.is_side_effect_free()) {
		return false;
	} 
	// ISSUE: generating "strictly conforming" programs.
	//
	// We can neither read nor write a variable that is being written in
	// the current effect context.
	if (((access == Effect::READ) || (access == Effect::WRITE))
		&& (effect_context.is_written(var) || effect_context.field_is_written(var))) {
		return false;
	}
	// ISSUE: generating "strictly conforming" programs.
	//
	// We cannot write a variable that is being read in the current effect context. 
	// JYTODO: this is too restrictive, with dereference, var is not the variable 
	// being written, but the pointed variable. Nevertheless, we excluded var here
	if ((access == Effect::WRITE && deref_level==0) && (effect_context.is_read(var) || effect_context.field_is_read(var))) {
		return false;
	}
	// ISSUE: generating correct C programs.
	//
	// We cannot write `const' variables.
	if ((access == Effect::WRITE) && is_const) {
		return false;
	}
	// ISSUE: generating "interesting" C programs.
	//
	// We cannot read a variable if the current code-generation context
	// says that we should not.
	if ((access == Effect::READ) && cg_context.is_nonreadable(var)) {
		return false;
	}
	// ISSUE: generating "interesting" C programs.
	//
	// We cannot write a variable if the current code-generation context
	// says that we should not.
	if ((access == Effect::WRITE) && cg_context.is_nonwritable(var)) {
		return false;
	}
	return true;
}

/* return true if a variable in the list is volatile */
bool
VariableSelector::has_eligible_volatile_var(const vector<Variable *>& vars, const Type* type, const CVQualifiers* qfer, Effect::Access access, const CGContext& cg_context)
{ 
	for (size_t i=0; i<vars.size(); i++) {
		Variable* var = vars[i];
		if (type && !type->match(var->type, eFlexible)) { 
            continue;
		}
		if (qfer && !qfer->match_indirect(var->qfer)) {
			continue;
		}
		int deref_level = var->type->get_indirect_level() - type->get_indirect_level();
		if (is_eligible_var(var, deref_level, access, cg_context) && var->is_volatile()) {
			Bookkeeper::volatile_avail++;
			return true;
		}
	}
	return false;
}

Variable *
VariableSelector::choose_ok_var(const vector<Variable *> &vars)
{
	int len = vars.size();
	Variable* v = NULL;
	if (len == 1) {
		v = vars[0];
	}
	else if (len > 1) {
		DEPTH_GUARD_BY_DEPTH_RETURN(1, NULL);
		unsigned int index = rnd_upto(len);
		ERROR_GUARD(NULL);
		v = vars[index];
	}
	// if v is "collective" array variable, return a "itemized" array member
	if (v && v->isArray) {
		ArrayVariable* av = (ArrayVariable*)v;
		if (av->collective == 0) {
			v = av->itemize();
		}
	}
	return v;
}

const Variable *
VariableSelector::choose_ok_var(const vector<const Variable *> &vars)
{
	int len = vars.size();
	const Variable* v = NULL;
	if (len == 1) {
		v = vars[0];
	}
	else if (len > 1) {
		DEPTH_GUARD_BY_DEPTH_RETURN(1, NULL);
		unsigned int index = rnd_upto(len);
		ERROR_GUARD(NULL);
		v = vars[index];
	}
	// if v is "collective" array variable, return a "itemized" array member
	if (v && v->isArray) {
		const ArrayVariable* av = (const ArrayVariable*)v;
		if (av->collective == 0) {
			v = av->itemize();
		}
	}
	return v;
}

const Variable *
VariableSelector::choose_visible_written_var(const Block* b, vector<const Variable*> written_vars, const Type* type)
{
	size_t i;
	vector<const Variable*> ok_vars;
	// include the fields of structs
	expand_struct_vars(written_vars, type);

	for (i=0; i<written_vars.size(); i++) {
		const Variable* v = written_vars[i];
		if (type->match(v->type, eConvert) && 
			(b->is_var_on_stack(v) || v->is_global()) &&
			!v->is_virtual() &&
			!v->is_volatile()) {
			ok_vars.push_back(v);
		}
	}
	return choose_ok_var(ok_vars);
}

// --------------------------------------------------------------
/*
 * Choose a variable from `vars' to read or write.
 * Return null if no suitable variable can be found.
 *
 * Parameter "type"
 * 0 --- To match any type
 * any simple type --- To match any type of integers
 * any struct type --- To match the specific structs
 *
 * Parameter "qfer"
 * to match the const/volatile qualifier(s) 
 * see CVQualifier::match
 */
Variable *
VariableSelector::choose_var(vector<Variable *> vars,
		   Effect::Access access,
		   const CGContext &cg_context,
		   const Type* type,
		   const CVQualifiers* qfer,
		   eMatchType mt,
		   const vector<const Variable*>& invalid_vars,
		   bool no_bitfield,
		   bool no_expand_struct)
{
	vector<Variable *> ok_vars;
	vector<Variable *>::iterator i;

	if (!no_expand_struct && type && (type->eType == eSimple || type->eType == eStruct))
		expand_struct_vars(vars, type); 

	bool found = has_dereferenceable_var(vars, type, cg_context);
	if (found) {// && !cg_context.in_call_chain_loop()) {
		Bookkeeper::pointer_avail_for_dereference++;
	}
	// check availability of volatiles
	has_eligible_volatile_var(vars, type, qfer, access, cg_context);

	for (i = vars.begin(); i != vars.end(); ++i) {
        // skip any type mismatched var
        if (no_bitfield && (*i)->isBitfield_)
			continue;
        if (type && !type->match((*i)->type, mt)) { 
            continue;
		}
		if (qfer && !qfer->match_indirect((*i)->qfer)) {
			continue;
		}
		// skip any variable in the invalid_vars list
		if (is_variable_in_set(invalid_vars, *i)) {
			continue;
		}
		int deref_level = (*i)->type->get_indirect_level() - type->get_indirect_level();
		if (is_eligible_var((*i), deref_level, access, cg_context)) {
			// Otherwise, this is an acceptable choice.
			ok_vars.push_back(*i);
		}
	}

    // artificially increase the odds of using volatile variable
	// JY: unncessary now
	if (0) {//ok_vars.size() > 1) {
		vector<Variable *> volatile_vars;
		for (size_t j=0; j<ok_vars.size(); j++) {
			Variable* vv = ok_vars[j];
			int deref_level = vv->type->get_indirect_level() - type->get_indirect_level();
			if (vv->is_volatile_after_deref(deref_level) || vv->is_volatile()) {
				volatile_vars.push_back(vv);
			} 
		}
		Variable *var = choose_ok_var(volatile_vars);
		if (var != NULL)
			return var;
    }

	// artificially increase the odds of dereferencing a pointer
	if (type && ok_vars.size() > 1) {
		vector<Variable *> ptrs;
		for (size_t j=0; j<ok_vars.size(); j++) {
			Variable* vv = ok_vars[j];
			if (type->get_indirect_level() < vv->type->get_indirect_level()) {
				ptrs.push_back(vv);
			}
		}
		Variable *var = choose_ok_var(ptrs);
		if (var != NULL)
			return var;
	}

	// artificially increase the odds of taking address of another variable
	if (type && type->eType == ePointer && ok_vars.size() > 1) {
		vector<Variable *> addressable_vars;
		for (size_t j=0; j<ok_vars.size(); j++) {
			Variable* vv = ok_vars[j];
			// don't take the address of a volatile, for now
			if (type->get_indirect_level() > vv->type->get_indirect_level()) {
				addressable_vars.push_back(vv);
			}
		}
		Variable *var = choose_ok_var(addressable_vars);
		if (var != NULL)
			return var;
	}
	return choose_ok_var(ok_vars);
}

Variable *
VariableSelector::create_and_initialize(Effect::Access access, const CGContext &cg_context, const Type* t, 
					const CVQualifiers* qfer, Block *blk, std::string name)
{
	const Expression* init = NULL;
	Variable* var = NULL;

	if (t->eType == ePointer && CGOptions::strict_const_arrays()) {
		init = make_init_value(access, cg_context, t, qfer, blk);
		ERROR_GUARD(NULL);
		var = new_variable(name, t, init, qfer);
	}
	else {
		init = make_init_value(access, cg_context, t, qfer, blk);
		if (rnd_flipcoin(NewArrayVariableProb)) {
			ERROR_GUARD(NULL);
			var = create_array_and_itemize(blk, name, cg_context, t, init, qfer);
		}
		else {
			ERROR_GUARD(NULL);
			var = new_variable(name, t, init, qfer);
		}	
	}
	ERROR_GUARD(NULL);
	assert(var);
	return var;
}

static int tmp_count = 0;
// --------------------------------------------------------------
 /* Parameter "type"
 * 0 --- To generate any type
 * any simple type --- To generate any type of integers
 * any struct type --- To generate the specific struct typed variable
 */
Variable *
VariableSelector::GenerateNewGlobal(Effect::Access access, const CGContext &cg_context, const Type* t, const CVQualifiers* qfer)
{	
	ERROR_GUARD(NULL);
	CVQualifiers var_qfer = (!qfer || qfer->wildcard) 
								? CVQualifiers::random_qualifiers(t, access, cg_context, false)
								: *qfer;
	ERROR_GUARD(NULL);
	string name = RandomGlobalName();
	tmp_count++;
	Variable* var = create_and_initialize(access, cg_context, t, &var_qfer, 0, name);

	Bookkeeper::record_vars_with_bitfields(t);
	incr_counter(Bookkeeper::struct_depth_cnts, t->get_struct_depth());
	GlobalList.push_back(var);
	// for DFA 
	FactMgr* fm = get_fact_mgr(&cg_context);
	fm->add_new_global_var_fact(var->get_collective());
	cg_context.get_current_func()->new_globals.push_back(var);

	if (!var_qfer.is_volatile()) {
		GlobalNonvolatilesList.push_back(var);
	}
	var_created = true;
	return var;
}

Variable *
VariableSelector::GenerateNewNonArrayGlobal(Effect::Access access, const CGContext &cg_context, const Type* t, const CVQualifiers* qfer)
{	
	ERROR_GUARD(NULL);
	CVQualifiers var_qfer = (!qfer || qfer->wildcard) 
								? CVQualifiers::random_qualifiers(t, access, cg_context, false)
								: *qfer;
	ERROR_GUARD(NULL);
	string name = RandomGlobalName();
	tmp_count++;

	const Expression *init = make_init_value(access, cg_context, t, qfer, NULL);
	ERROR_GUARD(NULL);
	Variable *var = new_variable(name, t, init, qfer);

	Bookkeeper::record_vars_with_bitfields(t);
	incr_counter(Bookkeeper::struct_depth_cnts, t->get_struct_depth());
	GlobalList.push_back(var);
	// for DFA 
	FactMgr* fm = get_fact_mgr(&cg_context);
	fm->add_new_global_var_fact(var->get_collective());
	cg_context.get_current_func()->new_globals.push_back(var);

	if (!var_qfer.is_volatile()) {
		GlobalNonvolatilesList.push_back(var);
	}
	var_created = true;
	return var;
}

Variable*
VariableSelector::eager_create_global_struct(Effect::Access access, const CGContext &cg_context, 
					const Type* type, const CVQualifiers* qfer,
					eMatchType mt, const vector<const Variable*>& invalid_vars)
{
	// We will choose a struct with all of its qualifiers.
	// choose_var() will rule out invalid field vars.
	assert(type);
	int level = type->get_indirect_level();
	const Type *t = 0;
	if (level == 0) {
		t = Type::choose_random_struct_from_type(type, false);
		GenerateNewGlobal(access, cg_context, t, qfer);
	}
	else if (level == 1) {
		t = Type::choose_random_struct_from_type(t->ptr_type, false);
		if (qfer) {
			CVQualifiers qfer1 = qfer->indirect_qualifiers(level);
			GenerateNewGlobal(access, cg_context, t, &qfer1);
		}
		else {
			GenerateNewGlobal(access, cg_context, t, qfer);
		}
	}
	else
		return NULL;

	ERROR_GUARD(NULL);
	return choose_var(GlobalList, access, cg_context, type, qfer, mt, invalid_vars);
}

Variable*
VariableSelector::eager_create_local_struct(Block &block, Effect::Access access, const CGContext &cg_context, 
					const Type* type, const CVQualifiers* qfer,
					eMatchType mt, const vector<const Variable*>& invalid_vars)
{
	// We will choose a struct with all of its qualifiers.
	// choose_var() will rule out invalid field vars.
	assert(type);
	int level = type->get_indirect_level();
	const Type *t = 0;
	if (level == 0) {
		t = Type::choose_random_struct_from_type(type, true);
		GenerateNewParentLocal(block, access, cg_context, t, qfer);
	}
	else if (level == 1) {
		t = Type::choose_random_struct_from_type(t->ptr_type, true);
		if (qfer) {
			CVQualifiers qfer1 = qfer->indirect_qualifiers(level);
			GenerateNewParentLocal(block, access, cg_context, t, &qfer1);
		}
		else {
			GenerateNewParentLocal(block, access, cg_context, t, qfer);
		}
	}
	else
		return NULL;
	ERROR_GUARD(NULL);
	if (!t)
		return NULL;

	ERROR_GUARD(NULL);
	return choose_var(block.local_vars, access, cg_context, type, qfer, mt, invalid_vars);
}


// --------------------------------------------------------------
// Select a random global variable.
Variable *
VariableSelector::SelectGlobal(Effect::Access access, const CGContext &cg_context, const Type* type, const CVQualifiers* qfer, eMatchType mt, const vector<const Variable*>& invalid_vars)
{
	Variable* var = choose_var(GlobalList, access, cg_context, type, qfer, mt, invalid_vars);
	ERROR_GUARD(NULL);
	if (var == 0) {
		if (CGOptions::expand_struct()) {
			var = VariableSelector::eager_create_global_struct(access, cg_context, type, qfer, mt, invalid_vars);
			ERROR_GUARD(NULL);
			if (var)
				return var;
		}

		DEPTH_GUARD_BY_TYPE_RETURN(dtSelectGlobal, NULL);
		bool no_volatile = false;
		if (qfer && !qfer->wildcard && !qfer->is_volatile()) {
			no_volatile = true;
		}
		const Type* t = Type::random_type_from_type(type, no_volatile);
		ERROR_GUARD(NULL);
		return GenerateNewGlobal(access, cg_context, t, qfer);
	} 
	return var;
}

void
VariableSelector::find_all_non_bitfield_visible_vars(const Block *b, vector<Variable*> &vars)
{
	vector<Variable *>::iterator i;
	for (i = GlobalList.begin(); i != GlobalList.end(); ++i) {
		if (!((*i)->isBitfield_))
			vars.push_back(*i);
	}	

	while (b) {
		for (size_t j = 0; j < b->local_vars.size(); ++j) {
			if (!((b->local_vars[j])->isBitfield_))
				vars.push_back(b->local_vars[j]);
		}
		b = b->parent;
	}
}

void
VariableSelector::find_all_non_array_visible_vars(const Block *b, vector<Variable*> &vars)
{
	size_t i;
	for (i = 0; i < GlobalList.size(); i++) {
		if (!(GlobalList[i]->isArray))
			vars.push_back(GlobalList[i]);
	}	
	if (b) {
		for (i=0; i<b->func->param.size(); i++) {
			vars.push_back(b->func->param[i]);
		}
		while (b) {
			for (i = 0; i < b->local_vars.size(); i++) {
				if (!((b->local_vars[i])->isArray))
					vars.push_back(b->local_vars[i]);
			}
			b = b->parent;
		}
	}
}

void
VariableSelector::get_all_array_vars(vector<const Variable*> &array_vars)
{
	vector<Variable*> vars = GlobalList;
	for (size_t i=0; i<vars.size(); i++) {
		if (vars[i]->isArray) {
			array_vars.push_back(vars[i]);
		}
	}
}

/* find all visible variables at block b */
vector<Variable*>
VariableSelector::find_all_visible_vars(const Block* b)
{
	vector<Variable*> vars = GlobalList;
	while (b) {
		vars.insert(vars.end(), b->local_vars.begin(), b->local_vars.end());
		b = b->parent;
	} 
	return vars;
}

/* remove not-qualified variables from candidate list, not used for now */
void 
VariableSelector::remove_non_qualified_vars(vector<Variable*>& vars, Effect::Access access, const CGContext &cg_context)
{
	size_t i;
	size_t len = vars.size();
	const Effect& eff = cg_context.get_effect_context();
	for (i=0; i<len; i++) {
		const Variable* v = vars[i];
		bool ok = true;
		if (access==Effect::WRITE) {
			if (cg_context.is_nonwritable(v) || v->is_const()) {
				ok = false;
			}
			if (eff.is_read(v) || eff.is_written(v)) {
				ok = false;
			}
		}
		else if (access==Effect::READ) {
			if (eff.is_written(v)) {
				ok = false;
			}
		}
		if (!eff.is_side_effect_free() && v->is_volatile()) {
			ok = false;
		}
		if (!ok) {
			vars.erase(vars.begin() + i);
			i--;
			len--;
		}
	}
}

/* 
 * enlarge the block to contains both src and dest of jump edges, if there are
 * some destinations in this block. This is used to create a local variable
 * in appropriate block
 */
Block* 
VariableSelector::expand_block_for_goto(Block* b, const CGContext& cg_context) 
{
	FactMgr* fm = get_fact_mgr(&cg_context);
	size_t i;
	while (true) {
		for (i=0; i<fm->cfg_edges.size(); i++) {
			const CFGEdge* edge = fm->cfg_edges[i];
			if (edge->src->eType == eGoto && b->contains_stmt(edge->dest) && !b->contains_stmt(edge->src)) {
				while (b && !b->contains_stmt(edge->src)) {
					b = b->parent;
				}
				assert(b); 
				break;
			}
		}
		// exit loop only when requirement for all edges are satisfied 
		if (i==fm->cfg_edges.size()) {
			break;
		}
	}
	return b;
}
		
/* 
 * enlarge the block to contains all variables in the list. This is used to create 
 * itemized array variable
 */
Block* 
VariableSelector::lower_block_for_vars(const vector<Block*>& blks, vector<const Variable*>& vars) 
{ 
	size_t i, j, len;
	Block* b = 0;
	for (j=0; j<blks.size(); j++) {
		b = blks[j];
		len = vars.size();
		for (i=0; i<len; i++) {
			if (find_variable_in_set(b->local_vars, vars[i]) != -1) {
				vars.erase(vars.begin() + i);
				i--;
				len--;
			}
		}
		if (vars.empty()) {
			return b;
		}
	}
	// we break out of loop when either all vars have been covered, or
	// there is no more higher block to go - most likely there are global
	// variables or pameters in the list - in that case, returning 0
	// properly indicate only global scope covers all variables
	return 0;
}

/*************************************************************************************
 * find an initializing value for a new variable
 * for non-pointers, we just use constants
 * for pointers we need to find another variable to take address with a random chance. 
 *    If no much variable is available, we have to create a suitable variable (which 
 *    might call this function again)
 *************************************************************************************/
Expression* 
VariableSelector::make_init_value(Effect::Access access, const CGContext &cg_context, const Type* t, const CVQualifiers* qf, Block* b)
{
	assert(qf && qf->sanity_check(t));
	CVQualifiers qfer(*qf);
	// the initialzer should always be less restricting than the variable to be initialized
	qfer.accept_stricter = false;

	if (t->eType != ePointer || rnd_flipcoin(20)) {
		ERROR_GUARD(NULL);
		if (t->eType == eSimple)
			assert(t->simple_type != eVoid);
		return Constant::make_random(t);
	}
	ERROR_GUARD(NULL);
	// for pointers, take address of a random visible local variable 
	const Type* type = t->ptr_type;
	assert(type);

	vector<Variable*> vars = find_all_visible_vars(b);
	vector<const Variable*> dummy;
	
	Variable *var = NULL;
	// b == NULL means we are generating init for globals
	if (!b && CGOptions::ccomp()) {
		get_all_array_vars(dummy);
		var = choose_var(vars, access, cg_context, type, &qfer, eExact, dummy, true, true);
	}
	else {
		var = choose_var(vars, access, cg_context, type, &qfer, eExact, dummy, true);
	}
	ERROR_GUARD(NULL);
	
	// if no such var, create a new one   
	if (var == 0) {  
		DEPTH_GUARD_BY_TYPE_RETURN(dtInitPointerValue, NULL);
		// current context has no impact on variable initialization, which happens at declare time?
		bool no_volatile = false; //!cg_context.get_effect_context().is_side_effect_free();
		CVQualifiers qfer_deref = qfer.random_loose_qualifiers(no_volatile, access, cg_context);
		qfer_deref.remove_qualifiers(1);
		qfer_deref.accept_stricter = false; 
		bool use_local = (b != 0 && type->eType == ePointer && !qfer_deref.is_volatile());
		const Type* tt = use_local ? Type::random_type_from_type(type, true, true) : Type::random_type_from_type(type, false, true);
		ERROR_GUARD(NULL);
		// create a local if it's not a volatile, and it's a pointer, and block is specified
		if (use_local) {  
			var = GenerateNewParentLocal(*b, Effect::READ, cg_context, tt, &qfer_deref);
			ERROR_GUARD(NULL);
			Bookkeeper::record_volatile_access(var, var->type->get_indirect_level() - tt->get_indirect_level(), false);
		}
		else { 
			if (CGOptions::ccomp()) {
				var = GenerateNewNonArrayGlobal(Effect::READ, cg_context, tt, &qfer_deref);
			}
			else {
				var = GenerateNewGlobal(Effect::READ, cg_context, tt, &qfer_deref);
			}
			ERROR_GUARD(NULL);
		}
		Bookkeeper::record_address_taken(var);
	}
	assert(var);
	return new ExpressionVariable(*var, t);
}

// --------------------------------------------------------------
Variable *
VariableSelector::GenerateNewParentLocal(Block &block,
					   Effect::Access access,
					   const CGContext &cg_context,
                       const Type* t,
					   const CVQualifiers* qfer)
{
	ERROR_GUARD(NULL);
	assert(t);
	// if this is for a struct with volatile field(s), create a global variable instead
	if (t->eType==eStruct && !Type::is_nonvolatile_struct(t)) {
		return GenerateNewGlobal(access, cg_context, t, qfer);  
	}
	// if there are "goto" in block (and sub-blocks), find the jump source statement,
	// and make sure the variable we are creating is visible in both this block and jump
	// source block
	Block* blk = expand_block_for_goto(&block, cg_context);

	// make a local variable with initializer
	CVQualifiers var_qfer = (!qfer || qfer->wildcard) 
								? CVQualifiers::random_qualifiers(t, access, cg_context, true) 
								: *qfer;
	ERROR_GUARD(NULL);
	var_qfer.restrict(access, cg_context); 
	assert(var_qfer.sanity_check(t));
	string name = RandomLocalName();

	Variable* var = create_and_initialize(access, cg_context, t, &var_qfer, blk, name);

	Bookkeeper::record_vars_with_bitfields(t);
	incr_counter(Bookkeeper::struct_depth_cnts, t->get_struct_depth());
	blk->local_vars.push_back(var);
	FactMgr* fm = get_fact_mgr(&cg_context);
	fm->add_new_local_var_fact(blk, var->get_collective());
	var_created = true;
	return var;
}

/*
 * generate parameter for func_1
 */
Variable *
VariableSelector::GenerateParameterVariable(const Type *type, const CVQualifiers *qfer)
{
	return new_variable(RandomParamName(), type, 0, qfer);
}

// --------------------------------------------------------------
// choose a random type for parameter
// --------------------------------------------------------------
void
VariableSelector::GenerateParameterVariable(Function &curFunc)
{
	// Add this type to our parameter list. 
	const Type* t = 0;
	bool rnd = rnd_flipcoin(40);
	ERROR_RETURN();
	if (Type::has_pointer_type() && rnd) {
		t= Type::choose_random_pointer_type();
	}
	else {
		t = Type::choose_random_nonvoid_nonvolatile();
	}
	ERROR_RETURN();
	if (t->eType == eSimple)
		assert(t->simple_type != eVoid);

	CVQualifiers qfer = CVQualifiers::random_qualifiers(t); 
	ERROR_RETURN();
	Variable *param = new_variable(RandomParamName(), t, 0, &qfer);
	ERROR_RETURN();
	curFunc.param.push_back(param);
}

// --------------------------------------------------------------
Variable *
VariableSelector::SelectParentLocal(Effect::Access access,
				  const CGContext &cg_context,
                  const Type* type,
				  const CVQualifiers* qfer,
				  eMatchType mt,
				  const vector<const Variable*>& invalid_vars)
{
	DEPTH_GUARD_BY_TYPE_RETURN(dtSelectParentLocal, NULL);
	// Select from the local variables of the parent OR any of its block stack.
	Function& parent = *cg_context.get_current_func();
	if (parent.stack.empty()) {
		// We're choosing a local from a function whose body hasn't been built.
		// This should never happen.
		assert(!parent.stack.empty());
		return 0;
	}

	unsigned int index = rnd_upto(parent.stack.size());
	ERROR_GUARD(NULL);
	Block *block = parent.stack[index];
	
	// Should be "generate new block local"...
	const Type *t = NULL;
	if (block->local_vars.empty()) {
		if (CGOptions::expand_struct()) {
			Variable *var = VariableSelector::eager_create_local_struct(*block, access, cg_context, type, qfer, mt, invalid_vars);
			ERROR_GUARD(NULL);
			if (var)
				return var;
		}
		const Type* t = Type::random_type_from_type(type, true, false);
		ERROR_GUARD(NULL);
		return GenerateNewParentLocal(*block, access, cg_context, t, qfer);
	}

	if (type && type->eType == eSimple && (type->simple_type != eVoid)) {
		t = get_int_type();
	}
	else {
		t = Type::random_type_from_type(type, true, false);
		ERROR_GUARD(NULL);
	}

	Variable* var = choose_var(block->local_vars, access, cg_context, t, qfer, mt, invalid_vars);
	ERROR_GUARD(NULL);
	if (var == 0) {
#if 0
		if (CGOptions::expand_struct()) {
			var = VariableSelector::eager_create_local_struct(*block, access, cg_context, type, qfer, mt, invalid_vars);
			ERROR_GUARD(NULL);
			if (var)
				return var;
		}
#endif
		var = GenerateNewParentLocal(*block, access, cg_context, t, qfer);
	}
	return var;
}

// --------------------------------------------------------------
static eVariableScope
VariableSelectionProbability(eVariableScope upper = MAX_VAR_SCOPE, Filter *filter = NULL)
{
	// Should probably modify choice based on current list of params, parent
	// params, parent locals, # of globals, etc.
	//
	// ...but this is easier.
	VariableSelector::InitScopeTable();
	do {
		int i = rnd_upto(100, filter);
		ERROR_GUARD(MAX_VAR_SCOPE);
		eVariableScope scope = VariableSelector::scopeTable_->get_value(i);
		if (scope < upper) {
			return scope;
		}
	} while (true);
	return MAX_VAR_SCOPE;
}

// --------------------------------------------------------------
static eVariableScope
VariableCreationProbability(void)
{
	bool flag = rnd_flipcoin(10);
	ERROR_GUARD(MAX_VAR_SCOPE);
	if (flag)		// 10% chance to create new global var
		return eGlobal;
	else
		return eParentLocal;
}

// --------------------------------------------------------------
Variable *
VariableSelector::SelectParentParam(Effect::Access access,
				  const CGContext &cg_context,
				  const Type* type,
				  const CVQualifiers* qfer,
				  eMatchType mt,
				  const vector<const Variable*>& invalid_vars)
{
	Function &parent = *cg_context.get_current_func();
	if (parent.param.empty())
		return SelectParentLocal(access, cg_context, type, qfer, mt, invalid_vars); 
	Variable* var = choose_var(parent.param, access, cg_context, type, qfer, mt, invalid_vars);
	ERROR_GUARD(NULL);
	return var ? var : SelectParentLocal(access, cg_context, type, qfer, mt, invalid_vars);
}

// --------------------------------------------------------------
Variable *
VariableSelector::GenerateNewVariable(Effect::Access access,
					const CGContext &cg_context,
                    const Type* type,
					const CVQualifiers* qfer)
{
	DEPTH_GUARD_BY_TYPE_RETURN(dtGenerateNewVariable, NULL);
	Variable *var = 0;
	Function &func = *cg_context.get_current_func();
	eVariableScope scope = VariableCreationProbability();
	ERROR_GUARD(NULL);
	const Type* t = 0;
	switch (scope) {
	case eGlobal:
		DEPTH_GUARD_BY_TYPE_RETURN(dtGenerateNewGlobal, NULL);
		// TODO: it's ugly. For dfs_exhaustive mode, we've generate the first variable
		// by SelectGlobal. To reduce the redundant code, we don't re-generate the first
		// variable there.
		if (!CGOptions::is_random() && GlobalList.empty()) {
			Error::set_error(ERROR);
			return NULL;
		}
		t = Type::random_type_from_type(type);
		ERROR_GUARD(NULL);
		var = GenerateNewGlobal(access, cg_context, t, qfer);
		break;
	case eParentLocal:
	{
		DEPTH_GUARD_BY_DEPTH_RETURN(dtGenerateNewParentLocal, NULL);
		unsigned int index = rnd_upto(func.stack.size());
		ERROR_GUARD(NULL);

		// TODO: it's ugly. For dfs_exhaustive mode, we've generate the first variable
		// by SelectGlobal. To reduce the redundant code, we don't re-generate the first
		// variable there.
		if (!CGOptions::is_random() && (*(func.stack[index])).local_vars.empty()) {
			Error::set_error(ERROR);
			return NULL;
		}
		t = Type::random_type_from_type(type, true, false);
		ERROR_GUARD(NULL);
		var = GenerateNewParentLocal(*(func.stack[index]), access, cg_context, t, qfer);
		break;
	}
	default:
		break;
	}
	ERROR_GUARD(NULL);
	var_created = true;
	return var;
}

// --------------------------------------------------------------
 /* select a loop control variable, which is restricted to integers
  * only
  *
  * JYTODO: make pointers control variables?
  ************************************************************/
Variable *
VariableSelector::SelectLoopCtrlVar(const CGContext &cg_context, const vector<const Variable*>& invalid_vars)
{
	// Note that many of the functions that select `var' can return null, if
	const Type* type = get_int_type();  
	//eVariableScope scope = VariableSelectionProbability(eNewValue);
	//ERROR_GUARD(NULL);
	vector<Variable*> vars;
	find_all_non_array_visible_vars(cg_context.get_current_block(), vars); 
	Variable* var = choose_var(vars, Effect::WRITE, cg_context, type, 0, eConvert, invalid_vars, true);
	ERROR_GUARD(NULL);
	if (var == NULL) {
		var = GenerateNewGlobal(Effect::WRITE, cg_context, type, 0);
	}
	return var;
	//return select(Effect::WRITE, cg_context, type, 0, invalid_vars, eConvert, scope);
}

// --------------------------------------------------------------
// Select or Create a new variable visible to this scope (new var may be
// global, or local to one of the function's blocks)
Variable *
VariableSelector::select(Effect::Access access,
			   const CGContext &cg_context,
               const Type* type,
			   const CVQualifiers* qfer, 
			   const vector<const Variable*>& invalid_vars,
			   eMatchType mt, eVariableScope scope)
{
	DEPTH_GUARD_BY_TYPE_RETURN_WITH_FLAG(dtSelectVariable, scope, NULL);
	VariableSelectFilter filter(cg_context);
	if (scope == MAX_VAR_SCOPE) {
		scope = VariableSelectionProbability(scope, &filter);
	}
	ERROR_GUARD(NULL); 
	Variable *var = 0;
	var_created = false;

	// Note that many of the functions that select `var' can return null, if
	// they cannot find a suitable variable.  So we loop.
	switch (scope) {
	case eGlobal:
		var = SelectGlobal(access, cg_context, type, qfer, mt, invalid_vars);
		break;
	case eParentLocal:
		// ...a local var from one of its blocks.
		var = SelectParentLocal(access, cg_context, type, qfer, mt, invalid_vars);
		break;
	case eParentParam:
		// ...one of the function's parameters.
		var = SelectParentParam(access, cg_context, type, qfer, mt, invalid_vars);
		break;
	case eNewValue:
		// Must decide where to put the new variable (global or parent
		// local)?
		var = GenerateNewVariable(access, cg_context, type, qfer); 
		if (CGOptions::expand_struct())
			Error::set_error(ERROR);
		break;
	} 
	ERROR_GUARD(NULL);
	if (var && !cg_context.get_effect_context().is_side_effect_free()) {
		assert(!var->is_volatile());
	}
	if (var_created) {
		Bookkeeper::use_new_var_cnt++;
	} else {
		Bookkeeper::use_old_var_cnt++;
	}
	return var;
}

// --------------------------------------------------------------
// specifically select a pointer to be dereferenced 
Variable *
VariableSelector::select_deref_pointer(Effect::Access access, const CGContext &cg_context, const Type* type, const CVQualifiers* qfer, const vector<const Variable*>& invalid_vars)
{ 
	assert(qfer && qfer->sanity_check(type));
	vector<Variable*> vars;
	// add globals
	vars.insert(vars.end(), GlobalNonvolatilesList.begin(), GlobalNonvolatilesList.end()); 
	// add parent locals
	const Block* b = cg_context.get_current_block();
	while (b) {
		vars.insert(vars.end(), b->local_vars.begin(), b->local_vars.end());
		b = b->parent;
	}
	// add function parameters
	const Function* f = cg_context.get_current_func();
	vars.insert(vars.end(), f->param.begin(), f->param.end()); 

	Variable* var = choose_var(vars, access, cg_context, type, qfer, eDereference, invalid_vars);
	ERROR_GUARD(NULL);
	if (var == 0) {
		Type* ptr_type = Type::find_pointer_type(type, true);
		ERROR_GUARD(NULL);
		assert(ptr_type);
		CVQualifiers ptr_qfer = (!qfer || qfer->wildcard) 
								? CVQualifiers::random_qualifiers(ptr_type, access, cg_context, true) 
			                    //: qfer->indirect_qualifiers(-1);
								: qfer->random_add_qualifiers(!cg_context.get_effect_context().is_side_effect_free());
		ERROR_GUARD(NULL);
		ptr_qfer.accept_stricter = false;
		if (access == Effect::WRITE) {
			ptr_qfer.set_const(false, 1);
		} 
		if (ptr_qfer.is_volatile()) {
			if (CGOptions::expand_struct()) {
				var = VariableSelector::eager_create_global_struct(access, cg_context, ptr_type, 
							&ptr_qfer, eDereference, invalid_vars);
				ERROR_GUARD(NULL);
				if (var)
					return var;
				else {
					Error::set_error(ERROR);
					return NULL;
				}
			}
			var = GenerateNewGlobal(access, cg_context, ptr_type, &ptr_qfer);
		}
		else {
			Block *block = cg_context.get_current_block();
			if (CGOptions::expand_struct()) {
				Variable *var = VariableSelector::eager_create_local_struct(*block, access, 
							cg_context, ptr_type, &ptr_qfer, eDereference, invalid_vars);
				ERROR_GUARD(NULL);
				if (var)
					return var;
				else {
					Error::set_error(ERROR);
					return NULL;
				}
			}
			var = GenerateNewParentLocal(*block, access, cg_context, ptr_type, &ptr_qfer);
		}
	}
	ERROR_GUARD(NULL);
	return var;
}

/* 
 * create an array, and return an itemized member 
 */
ArrayVariable* 
VariableSelector::create_array_and_itemize(Block* blk, string name, const CGContext& /*cg_context*/, 
		const Type* t, const Expression* init, const CVQualifiers* qfer)
{
	ArrayVariable* av = ArrayVariable::CreateArrayVariable(blk, name, t, init, qfer, NULL);
	ERROR_GUARD(NULL);
	AllVars.push_back(av);
	return av->itemize();
}

/* 
 * create an array of random type, non-qualifier, with random initial value, in a random block (or as global)
 */
ArrayVariable*
VariableSelector::create_random_array(const CGContext& cg_context)
{
	bool as_global = rnd_flipcoin(30);
	ERROR_GUARD(NULL);
	string name;
	Block* blk = 0;
	Function* func = cg_context.get_current_func();
	if (as_global) {
		name = RandomGlobalName();
	}
	else {
		name = RandomLocalName();
		size_t index = rnd_upto(func->stack.size());
		ERROR_GUARD(NULL);
		blk = func->stack[index];
		blk = expand_block_for_goto(blk, cg_context);
	}
	const Type* type = 0;
	do {
		// don't make life complicated, restrict local variables to non-volatile
		type = as_global ? Type::choose_random_nonvoid() : Type::choose_random_nonvoid_nonvolatile();
		ERROR_GUARD(NULL);
	} while (type->is_const_struct() || !cg_context.accept_type(type));
	CVQualifiers qfer;
	qfer.add_qualifiers(false, false);

	Expression* init = Constant::make_random(type);
	ArrayVariable* av = ArrayVariable::CreateArrayVariable(blk, name, type, init, &qfer, NULL);
	AllVars.push_back(av);
	return av;
}

/*
 * select a random array variable, or generate a new one if none available
 */
ArrayVariable*
VariableSelector::select_array(const CGContext &cg_context)
{
	//static int g = 0;
	//int h = g++;
	const Block* b = cg_context.get_current_block();
	vector<Variable*> vars = find_all_visible_vars(b);
	vector<ArrayVariable*> array_vars;
	size_t i, len;
	for (i=0; i<vars.size(); i++) {
		if (vars[i]->isArray) {
			ArrayVariable* av = dynamic_cast<ArrayVariable*>(vars[i]);
			assert(av);
			if (av->collective == 0) {
				if (!cg_context.get_effect_context().is_read(av) && 
					!cg_context.get_effect_context().field_is_read(av) &&
					!cg_context.get_effect_context().is_written(av) &&
					(cg_context.get_effect_context().is_side_effect_free() || !av->is_volatile()) &&
					!av->is_const() &&
					!cg_context.is_nonwritable(av) &&
					!av->type->is_const_struct()) {
					array_vars.push_back(av);
				}
			}
		}
	}
	len = array_vars.size();
	if (len == 0) {
		return create_random_array(cg_context);
	}
	if (len == 1) return array_vars[0];
	size_t index = rnd_upto(len);
	ERROR_GUARD(NULL);
	return array_vars[index];
}	

/******************************************************************************
 * focus variable is used to direct the generation of array operations. A focus
 * variable denotes which member of which array when are interested to read and/or 
 * write.
 *
 * focus var can be selected with choices: 
 *
 * 1) For focus var "ary[i]", we could select neighboring array members, or
 * any member whose index depends on i, for example "ary[i-1]"
 *
 * 2) for focus var that is a struct, we can select any field of it
 *******************************************************************************/
const ArrayVariable*
VariableSelector::select_random_focus_var(Effect::Access access,
			   const CGContext &cg_context,
               const Type* type,
			   const CVQualifiers* /*qfer*/, 
			   const vector<const Variable*>& invalid_vars,
			   eMatchType /*mt*/)
{
	ArrayVariable* av = cg_context.focus_var;
	int deref_level = av->type->get_indirect_level() - type->get_indirect_level();
	// check with constraints on const and volatile qualification of the array variable
	if (access == Effect::WRITE && av->is_const_after_deref(deref_level)) {
		return 0;
	}
	if (!cg_context.get_effect_context().is_side_effect_free() && av->is_volatile_after_deref(deref_level)) {
		return 0;
	}
	if (find_variable_in_set(invalid_vars, av->get_collective()) != -1) {
		return 0;
	}
	if (av) {
		return av->rnd_mutate();
	}
	return 0;
}

ArrayVariable*
VariableSelector::create_mutated_array_var(const ArrayVariable* av, const vector<const Expression*>& new_indices)
{
	size_t i;
	ArrayVariable* new_av = new ArrayVariable(*av);
	for (i=0; i<new_indices.size(); i++) {
		new_av->set_index(i, new_indices[i]);
	}
	// add new variable to local list and all-variable list
	AllVars.push_back(new_av);
	av->parent->local_vars.push_back(new_av);
	return new_av;
}

Variable *
VariableSelector::make_dummy_variable(const string &name, const CVQualifiers* qfer)
{
	Variable *var = new Variable(name, 0, 0, qfer);
	AllVars.push_back(var);
	return var;
}

Variable *
VariableSelector::make_dummy_static_variable(const string &name)
{
	CVQualifiers dummy;
	Variable *var = new Variable(name, 0, 0, &dummy);
	return var;
}


const Variable* 
VariableSelector::find_var_by_name(string name)
{
	size_t i;
	for (i=0; i<AllVars.size(); i++) {
		const Variable* v = AllVars[i]->match_var_name(name);
		if (v) {
			return v;
		}
	}
	return NULL;
}

/*
 *
 */
void
VariableSelector::doFinalization(void)
{
	size_t i;
	for(i=0; i<AllVars.size(); i++) {
		delete AllVars[i];
	}
	AllVars.clear();
	GlobalList.clear();
	GlobalNonvolatilesList.clear();
}

// --------------------------------------------------------------
void
OutputGlobalVariables(std::ostream &out)
{
	output_comment_line(out, "--- GLOBAL VARIABLES ---");
	vector<Variable *>& vars = *(VariableSelector::GetGlobalVariables());
	OutputVariableList(vars, out);
}

void
OutputGlobalVariablesDecls(std::ostream &out, std::string prefix)
{
	output_comment_line(out, "--- GLOBAL VARIABLES ---");
	OutputVariableDeclList(*VariableSelector::GetGlobalVariables(), out, prefix);
}

void
HashGlobalVariables(std::ostream &out)
{
	MapVariableList(*VariableSelector::GetGlobalVariables(), out, HashVariable);
}

///////////////////////////////////////////////////////////////////////////////

// Local Variables:
// c-basic-offset: 4
// tab-width: 4
// End:

// End of file.