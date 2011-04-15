// -*- mode: C++ -*-
//
// Copyright (c) 2007, 2008, 2010, 2011 The University of Utah
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

#ifndef CGCONTEXT_H
#define CGCONTEXT_H

///////////////////////////////////////////////////////////////////////////////

#include <vector> 
#include "Effect.h"
using namespace std;

// Flags..
#define NO_RETURN (1)
#define IN_LOOP (2)
#define DO_HASH (4)
#define NO_DANGLING_PTR (8)
#define IN_CALL_CHAIN_LOOP (16)
#define INVISIBLE (9999)
#define INACTIVE (8888)

class Effect;
class Function;
class Variable;
class Fact;
class Block;
class Type;
class Lhs;
class FactMgr;
class Statement;
class ExpressionVariable; 
class ArrayVariable;

/*
 *
 */
class CGContext
{
public:
	typedef std::vector<const Variable *> VariableSet;
	static const VariableSet empty_variable_set;

	// CGContext(void);
	CGContext(Function *current_func,
			  int stmt_depth,
			  int expr_depth,
			  unsigned int flags,
			  const std::vector<const Block*>& callers,
			  const Block* blk,
			  ArrayVariable* fvar,
			  const VariableSet &no_read_vars,
			  const VariableSet &no_write_vars,
			  const Effect &eff_context,
			  Effect *eff_accum);
	// A convenience constructor: `stmt_depth' and `flags' are set to zero,
	// and `no_write_vars' is set to an empty set.
	CGContext(Function *current_func,
			  const Effect &eff_context,
			  Effect *eff_accum);
	CGContext(const CGContext &cgc);
	~CGContext(void);

	bool in_call_chain_loop(void) const { return (flags & IN_LOOP) || (flags & IN_CALL_CHAIN_LOOP);}

	void output_call_chain(std::ostream &out);

	bool in_conflict(const Effect& eff) const;

	bool accept_type(const Type* t) const;

	Function *get_current_func(void) const			{ return current_func; }

	Block *get_current_block(void) const;

	static const CGContext& get_empty_context(void) { return empty_context;}

	const VariableSet &get_no_read_vars(void) const { return no_read_vars; }
	const VariableSet &get_no_write_vars(void) const { return no_write_vars; }

	const Effect &get_effect_context(void) const	{ return effect_context; }
	Effect *get_effect_accum(void) const			{ return effect_accum; }
	Effect get_accum_effect(void) const				{ Effect e; return effect_accum ? *effect_accum : e; }
	Effect& get_effect_stm(void) 					{ return effect_stm; }

	bool is_nonreadable(const Variable *v) const;
	bool is_nonwritable(const Variable *v) const;

	void read_var(const Variable *v);
	void write_var(const Variable *v);
	bool check_read_var(const Variable *v, const std::vector<const Fact*>& facts);
	bool check_write_var(const Variable *v, const std::vector<const Fact*>& facts);
	bool read_indices(const Variable* v, const vector<const Fact*>& facts);
	bool read_pointed(const ExpressionVariable* v, const std::vector<const Fact*>& facts);
	bool write_pointed(const Lhs* v, const std::vector<const Fact*>& facts);
	void add_effect(const Effect &e);
	void add_external_effect(const Effect &e);
	void add_visible_effect(const Effect &e, const Block* b);

    int find_variable_scope(const Variable* var) const;

	void extend_call_chain(const CGContext& cg_context);

	void reset_effect_accum(const Effect& e) { if (effect_accum) *effect_accum = e;}
	void reset_effect_stm(const Effect& e) { effect_stm = e;}
 
	bool allow_volatile() const;
	bool allow_const(Effect::Access access) const;

private:
	Function * const current_func; // may be null.
    

public: // XXX
	int stmt_depth;
	int expr_depth;
	unsigned int flags;
	std::vector<const Block*> call_chain;
	const Block* curr_blk; 
	ArrayVariable* focus_var;

private:
	// The set of variables that should not be read.  Listing a variable here
	// is not about generating conforming code; it is simply about directing
	// the code generator.
	//
	const VariableSet &no_read_vars;

	// The set of variables that should not be written.  Again, this is not
	// about generating conforming code; it is simply about directing the
	// code generator.
	//
	const VariableSet &no_write_vars;

	const Effect &effect_context;
	Effect *effect_accum; // may be null!
	Effect effect_stm;
	// TODO: move `Function::flags' to here.
	// TODO: move `Function::...' to here?

private:
	CGContext &operator=(const CGContext &cgc);	// unimplementable
	void sanity_check(void);
	static const CGContext empty_context;
};

///////////////////////////////////////////////////////////////////////////////

#endif // CGCONTEXT_H

// Local Variables:
// c-basic-offset: 4
// tab-width: 4
// End:

// End of file.