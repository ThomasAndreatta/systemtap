// translation pass
// Copyright (C) 2005-2019 Red Hat Inc.
// Copyright (C) 2005-2008 Intel Corporation.
// Copyright (C) 2010 Novell Corporation.
//
// This file is part of systemtap, and is free software.  You can
// redistribute it and/or modify it under the terms of the GNU General
// Public License (GPL); either version 2, or (at your option) any
// later version.

#include "config.h"
#include "staptree.h"
#include "elaborate.h"
#include "translate.h"
#include "session.h"
#include "tapsets.h"
#include "util.h"
#include "dwarf_wrappers.h"
#include "setupdwfl.h"
#include "task_finder.h"
#include "runtime/k_syms.h"
#include "dwflpp.h"
#include "stapregex.h"
#include "stringtable.h"

#include <byteswap.h>
#include <cstdlib>
#include <iostream>
#include <set>
#include <sstream>
#include <string>
#include <cassert>
#include <cstring>
#include <cerrno>

extern "C" {
#include <dwarf.h>
#include <elfutils/libdwfl.h>
#include <elfutils/libdw.h>
#include <ftw.h>
#define __STDC_FORMAT_MACROS
#include <inttypes.h>
#include <unistd.h>
}

// Max unwind table size (debug or eh) per module. Somewhat arbitrary
// limit (a bit more than twice the .debug_frame size of my local
// vmlinux for 2.6.31.4-83.fc12.x86_64).
// A larger value was recently found in a libxul.so build.
// ... and yet again in libxul.so, PR15162
// ... and yet again w.r.t. oracle db in private communication, 25289196
#define MAX_UNWIND_TABLE_SIZE (32 * 1024 * 1024)

#define STAP_T_01 _("\"Array overflow, check ")
#define STAP_T_02 _("\"MAXNESTING exceeded\";")
#define STAP_T_03 _("\"division by 0\";")
#define STAP_T_04 _("\"MAXACTION exceeded\";")
#define STAP_T_05 _("\"aggregation overflow in ")
#define STAP_T_06 _("\"empty aggregate\";")
#define STAP_T_07 _("\"histogram index out of range\";")

using namespace std;

class var;
struct tmpvar;
struct aggvar;
struct mapvar;
class itervar;

// A null-sink output stream, similar to /dev/null
// (no buffer -> badbit -> quietly suppressed output)
static ostream nullstream(NULL);
static translator_output null_o(nullstream);

struct c_unparser: public unparser, public visitor
{
  systemtap_session* session;
  translator_output* o;

  derived_probe* current_probe;
  functiondecl* current_function;

  const functioncall* assigned_functioncall;
  const string* assigned_functioncall_retval;

  unsigned tmpvar_counter;
  unsigned label_counter;
  unsigned action_counter;
  unsigned fc_counter;
  bool already_checked_action_count;

  varuse_collecting_visitor vcv_needs_global_locks; // tracks union of all probe handler body reads/writes

  map<string, probe*> probe_contents;

  // with respect to current_probe:
  set<statement*> pushdown_lock; // emit_lock() required before/inside these statements
  set<statement*> pushdown_unlock; // emit_unlock() required inside/after these statements
  inline bool pushdown_lock_p(statement* s) {
    if (this->session->verbose > 3)
      clog << "pushdown_lock for " << *s->tok << " "
           << ((pushdown_lock.find(s) != pushdown_lock.end()) ? "" : "not ")
           << "needed"
           << endl;
    return pushdown_lock.find(s) != pushdown_lock.end();
  }
  inline bool pushdown_unlock_p(statement* s) {
    if (this->session->verbose > 3)
      clog << "pushdown_unlock for " << *s->tok << " "
           << ((pushdown_unlock.find(s) != pushdown_unlock.end()) ? "" : "not ")
           << "needed"
           << endl;
    return pushdown_unlock.find(s) != pushdown_unlock.end();
  }
  
  // PR26296
  //
  // The trivial implementation is: every statement-visitor in the sets
  // emits a lock-at-beginning and/or unlock-at-end; never update the sets,
  // so only the outermost probe-handler body statement object does this.
  //
  // But it's better to push operations inward if possible, toward
  // smaller/nested statements, if the lock lifetimes can be safely
  // shortened.  How this is done safely/ideally depends on the
  // statement type, so see those visitors.

  map<pair<bool, string>, string> compiled_printfs;

  c_unparser (systemtap_session* ss, translator_output* op=NULL):
    session (ss), o (op ?: ss->op), current_probe(0), current_function (0),
    assigned_functioncall (0), assigned_functioncall_retval (0),
    tmpvar_counter (0), label_counter (0), action_counter(0), fc_counter(0),
    already_checked_action_count(false), vcv_needs_global_locks (*ss) {}
  ~c_unparser () {}

  // The main c_unparser doesn't write declarations as it traverses,
  // but the c_tmpcounter subclass will.
  virtual void var_declare(string const&, var const&) {}

  // If we've seen a dupe, return it; else remember this and return NULL.
  probe *get_probe_dupe (derived_probe *dp);

  void emit_map_type_instantiations ();
  void emit_common_header ();
  void emit_global (vardecl* v);
  void emit_global_init (vardecl* v);
  void emit_global_init_type (vardecl *v);
  void emit_global_param (vardecl* v);
  void emit_global_init_setters ();
  void emit_functionsig (functiondecl* v);
  void emit_kernel_module_init ();
  void emit_kernel_module_exit ();
  void emit_module_init ();
  void emit_module_refresh ();
  void emit_module_exit ();
  void emit_function (functiondecl* v);
  void emit_lock_decls (const varuse_collecting_visitor& v);
  void emit_lock ();
  bool locks_needed_p (visitable *s);
  void locks_not_needed_argh (statement *s);
  void emit_unlock ();
  void emit_probe (derived_probe* v);
  void emit_probe_condition_update(derived_probe* v);

  void emit_compiled_printfs ();
  void emit_compiled_printf_locals ();
  void declare_compiled_printf (bool print_to_stream, const string& format);
  virtual const string& get_compiled_printf (bool print_to_stream,
					     const string& format);

  // for use by stats (pmap) foreach
  set<string> aggregations_active;

  // values immediately available in foreach_loop iterations
  map<string, string> foreach_loop_values;
  void visit_foreach_loop_value (foreach_loop* s, const string& value="");
  bool get_foreach_loop_value (arrayindex* ai, string& value);

  // for use by looping constructs
  vector<string> loop_break_labels;
  vector<string> loop_continue_labels;

  string c_typename (exp_type e);
  virtual string c_localname (const string& e, bool mangle_oldstyle = false);
  virtual string c_globalname (const string &e);
  virtual string c_funcname (const string &e);
  virtual string c_funcname (const string &e, bool &funcname_shortened);

  string c_arg_define (const string& e);
  string c_arg_undef (const string& e);

  string map_keytypes(vardecl* v);
  void c_global_write_def(vardecl* v);
  void c_global_read_def(vardecl* v);
  void c_global_write_undef(vardecl* v);
  void c_global_read_undef(vardecl* v);

  void c_assign (var& lvalue, const string& rvalue, const token* tok);
  void c_assign (tmpvar& lvalue, expression* rvalue, const char* msg);
  void c_assign (const string& lvalue, expression* rvalue, const char* msg);
  void c_assign (const string& lvalue, const string& rvalue, exp_type type,
                 const char* msg, const token* tok);

  void c_declare(exp_type ty, const string &ident);
  void c_declare_static(exp_type ty, const string &ident);

  void c_strcat (const string& lvalue, const string& rvalue);
  void c_strcat (const string& lvalue, expression* rvalue);

  void c_strcpy (const string& lvalue, const string& rvalue);
  void c_strcpy (const string& lvalue, expression* rvalue);

  bool is_local (vardecl const* r, token const* tok);

  tmpvar gensym(exp_type ty);
  aggvar gensym_aggregate();

  var getvar(vardecl* v, token const* tok = NULL);
  itervar getiter(symbol* s);
  mapvar getmap(vardecl* v, token const* tok = NULL);

  void load_map_indices(arrayindex* e,
			vector<tmpvar> & idx);

  var* load_aggregate (expression *e, aggvar & agg);
  string histogram_index_check(var & vase, tmpvar & idx) const;

  void collect_map_index_types(vector<vardecl* > const & vars,
			       set< pair<vector<exp_type>, exp_type> > & types);

  void record_actions (unsigned actions, const token* tok, bool update=false);

  void visit_block (block* s);
  void visit_try_block (try_block* s);
  void visit_embeddedcode (embeddedcode* s);
  void visit_null_statement (null_statement* s);
  void visit_expr_statement (expr_statement* s);
  void visit_if_statement (if_statement* s);
  void visit_for_loop (for_loop* s);
  void visit_foreach_loop (foreach_loop* s);
  void visit_return_statement (return_statement* s);
  void visit_delete_statement (delete_statement* s);
  void visit_next_statement (next_statement* s);
  void visit_break_statement (break_statement* s);
  void visit_continue_statement (continue_statement* s);
  void visit_literal_string (literal_string* e);
  void visit_literal_number (literal_number* e);
  void visit_embedded_expr (embedded_expr* e);
  void visit_binary_expression (binary_expression* e);
  void visit_unary_expression (unary_expression* e);
  void visit_pre_crement (pre_crement* e);
  void visit_post_crement (post_crement* e);
  void visit_logical_or_expr (logical_or_expr* e);
  void visit_logical_and_expr (logical_and_expr* e);
  void visit_array_in (array_in* e);
  void visit_regex_query (regex_query* e);
  void visit_compound_expression(compound_expression* e);
  void visit_comparison (comparison* e);
  void visit_concatenation (concatenation* e);
  void visit_ternary_expression (ternary_expression* e);
  void visit_assignment (assignment* e);
  void visit_symbol (symbol* e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_target_bitfield (target_bitfield* e);
  void visit_target_symbol (target_symbol* e);
  void visit_arrayindex (arrayindex* e);
  void visit_functioncall (functioncall* e);
  void visit_print_format (print_format* e);
  void visit_stat_op (stat_op* e);
  void visit_hist_op (hist_op* e);
  void visit_cast_op (cast_op* e);
  void visit_autocast_op (autocast_op* e);
  void visit_atvar_op (atvar_op* e);
  void visit_defined_op (defined_op* e);
  void visit_probewrite_op(probewrite_op* e);
  void visit_entry_op (entry_op* e);
  void visit_perf_op (perf_op* e);

  // start/close statements with multiple independent child visits
  virtual void start_compound_statement (const char*, statement*) { }
  virtual void close_compound_statement (const char*, statement*) { }

  // wrap one child visit of a compound statement
  virtual void wrap_compound_visit (expression *e) { if (e) e->visit (this); }
  virtual void wrap_compound_visit (statement *s) { if (s) s->visit (this); }
};

// A shadow visitor, meant to generate temporary variable declarations
// for function or probe bodies.  The output is discarded, but we now do
// real work in var_declare().
struct c_tmpcounter cxx_final: public c_unparser
{
  c_unparser* parent;
  set<string> declared_vars;

  c_tmpcounter (c_unparser* p):
    c_unparser(p->session, &null_o), parent (p)
  { }

  // When vars are created *and used* (i.e. not overridden tmpvars) they call
  // var_declare(), which will forward to the parent c_unparser for output;
  void var_declare(string const&, var const& v) cxx_override;

  void emit_function (functiondecl* fd) cxx_override;
  void emit_probe (derived_probe* dp) cxx_override;

  const string& get_compiled_printf (bool print_to_stream,
				     const string& format) cxx_override;

  void start_compound_statement (const char*, statement*) cxx_override;
  void close_compound_statement (const char*, statement*) cxx_override;

  void wrap_compound_visit (expression *e) cxx_override;
  void wrap_compound_visit (statement *s) cxx_override;

  void start_struct_def (std::ostream::pos_type &before,
                         std::ostream::pos_type &after, const token* tok);
  void close_struct_def (std::ostream::pos_type before,
                         std::ostream::pos_type after);
};

struct c_unparser_assignment:
  public throwing_visitor
{
  c_unparser* parent;
  interned_string op;
  expression* rvalue;
  bool post; // true == value saved before modify operator
  c_unparser_assignment (c_unparser* p, interned_string o, expression* e):
    throwing_visitor ("invalid lvalue type"),
    parent (p), op (o), rvalue (e), post (false) {}
  c_unparser_assignment (c_unparser* p, interned_string o, bool pp):
    throwing_visitor ("invalid lvalue type"),
    parent (p), op (o), rvalue (0), post (pp) {}

  void prepare_rvalue (interned_string op,
		       tmpvar & rval,
		       token const*  tok);

  void c_assignop(tmpvar & res,
		  var const & lvar,
		  tmpvar const & tmp,
		  token const*  tok);

  // The set of valid lvalues are limited.
  void visit_symbol (symbol* e);
  void visit_target_register (target_register* e);
  void visit_target_deref (target_deref* e);
  void visit_arrayindex (arrayindex* e);
};


ostream & operator<<(ostream & o, var const & v);


/*
  Some clarification on the runtime structures involved in statistics:

  The basic type for collecting statistics in the runtime is struct
  stat_data. This contains the count, min, max, sum, and possibly
  histogram fields.

  There are two places struct stat_data shows up.

  1. If you declare a statistic variable of any sort, you want to make
  a struct _Stat. A struct _Stat* is also called a Stat. Struct _Stat
  contains a per-CPU array of struct stat_data values, as well as a
  struct stat_data which it aggregates into. Writes into a Struct
  _Stat go into the per-CPU struct stat. Reads involve write-locking
  the struct _Stat, aggregating into its aggregate struct stat_data,
  unlocking, read-locking the struct _Stat, then reading values out of
  the aggregate and unlocking.

  2. If you declare a statistic-valued map, you want to make a
  pmap. This is a per-CPU array of maps, each of which holds struct
  stat_data values, as well as an aggregate *map*. Writes into a pmap
  go into the per-CPU map. Reads involve write-locking the pmap,
  aggregating into its aggregate map, unlocking, read-locking the
  pmap, then reading values out of its aggregate (which is a normal
  map) and unlocking.

  Because, at the moment, the runtime does not support the concept of
  a statistic which collects multiple histogram types, we may need to
  instantiate one pmap or struct _Stat for each histogram variation
  the user wants to track.
 */

class var
{

protected:
  // Required for accurate mangling:
  c_unparser *u;

  bool local;
  exp_type ty;
  statistic_decl sd;
  string name;
  bool do_mangle;

private:
  mutable bool declaration_needed;

public:

  var(c_unparser *u, bool local, exp_type ty,
      statistic_decl const & sd, string const & name)
    : u(u), local(local), ty(ty), sd(sd), name(name),
      do_mangle(true), declaration_needed(false)
  {}

  var(c_unparser *u, bool local, exp_type ty, string const & name)
    : u(u), local(local), ty(ty), name(name),
      do_mangle(true), declaration_needed(false)
  {}

  var(c_unparser *u, bool local, exp_type ty,
      string const & name, bool do_mangle)
    : u(u), local(local), ty(ty), name(name),
      do_mangle(do_mangle), declaration_needed(false)
  {}

  var(c_unparser *u, bool local, exp_type ty, unsigned & counter)
    : u(u), local(local), ty(ty), name("__tmp" + lex_cast(counter++)),
      do_mangle(false), declaration_needed(true)
  {}

  virtual ~var() {}

  bool is_local() const
  {
    return local;
  }

  statistic_decl const & sdecl() const
  {
    return sd;
  }

  void assert_hist_compatible(hist_op const & hop)
  {
    // Semantic checks in elaborate should have caught this if it was
    // false. This is just a double-check.
    switch (sd.type)
      {
      case statistic_decl::linear:
	assert(hop.htype == hist_linear);
	assert(hop.params.size() == 3);
	assert(hop.params[0] == sd.linear_low);
	assert(hop.params[1] == sd.linear_high);
	assert(hop.params[2] == sd.linear_step);
	break;
      case statistic_decl::logarithmic:
	assert(hop.htype == hist_log);
	assert(hop.params.size() == 0);
	break;
      case statistic_decl::none:
	assert(false);
      }
  }

  exp_type type() const
  {
    return ty;
  }

  string c_name() const
  {
    if (!do_mangle)
      return name;
    else if (local)
      return u->c_localname(name);
    else
      return u->c_globalname(name);
  }

  string stat_op_tokens() const
  {
    string result = "";
    if (sd.stat_ops & STAT_OP_COUNT)
      result += "STAT_OP_COUNT, ";
    if (sd.stat_ops & STAT_OP_SUM)
      result += "STAT_OP_SUM, ";
    if (sd.stat_ops & STAT_OP_MIN)
      result += "STAT_OP_MIN, ";
    if (sd.stat_ops & STAT_OP_MAX)
      result += "STAT_OP_MAX, ";
    if (sd.stat_ops & STAT_OP_AVG)
      result += "STAT_OP_AVG, ";
    if (sd.stat_ops & STAT_OP_VARIANCE)
      result += "STAT_OP_VARIANCE, " + lex_cast(sd.bit_shift) + ", ";

    return result;
  }

  string value() const
  {
    if (declaration_needed)
      {
	u->var_declare (name, *this);
	declaration_needed = false;
      }

    if (local)
      return "l->" + c_name();
    else
      return "global(" + c_name() + ")";
  }

  virtual string hist() const
  {
    assert (ty == pe_stats);
    assert (sd.type != statistic_decl::none);
    return "(&(" + value() + "->hist))";
  }

  virtual string buckets() const
  {
    assert (ty == pe_stats);
    assert (sd.type != statistic_decl::none);
    return "(" + value() + "->hist.buckets)";
  }

  string init() const
  {
    switch (type())
      {
      case pe_string:
        if (! local)
          return ""; // module_param
        else
          return value() + "[0] = '\\0';";
      case pe_long:
        if (! local)
          return ""; // module_param
        else
          return value() + " = 0;";
      case pe_stats:
        {
          // See also mapvar::init().

          if (local)
            throw SEMANTIC_ERROR(_F("unsupported local stats init for %s", value().c_str()));

          string prefix = "global_set(" + c_name() + ", _stp_stat_init (" + stat_op_tokens();
          // Check for errors during allocation.
          string suffix = "if (" + value () + " == NULL) rc = -ENOMEM;";

          switch (sd.type)
            {
            case statistic_decl::none:
              prefix += string("KEY_HIST_TYPE, HIST_NONE, ");
              break;

            case statistic_decl::linear:
              prefix += string("KEY_HIST_TYPE, HIST_LINEAR, ")
                + lex_cast(sd.linear_low) + ", "
                + lex_cast(sd.linear_high) + ", "
                + lex_cast(sd.linear_step) + ", ";
              break;

            case statistic_decl::logarithmic:
              prefix += string("KEY_HIST_TYPE, HIST_LOG, ");
              break;

            default:
              throw SEMANTIC_ERROR(_F("unsupported stats type for %s", value().c_str()));
            }

	  prefix += "NULL";
          prefix = prefix + ")); ";
          return string (prefix + suffix);
        }

      default:
        throw SEMANTIC_ERROR(_F("unsupported initializer for %s", value().c_str()));
      }
  }

  string fini () const
  {
    switch (type())
      {
      case pe_string:
      case pe_long:
	return ""; // no action required
      case pe_stats:
	return "_stp_stat_del (" + value () + ");";
      default:
        throw SEMANTIC_ERROR(_F("unsupported deallocator for %s", value().c_str()));
      }
  }

  virtual void declare(c_unparser &c) const
  {
    c.c_declare(ty, c_name());
  }
};

ostream & operator<<(ostream & o, var const & v)
{
  return o << v.value();
}

void
c_tmpcounter::var_declare (string const& name, var const& v)
{
  if (declared_vars.insert(name).second)
    v.declare (*parent);
}

struct stmt_expr
{
  c_unparser & c;
  stmt_expr(c_unparser & c) : c(c)
  {
    c.o->newline() << "({";
    c.o->indent(1);
  }
  ~stmt_expr()
  {
    c.o->newline(-1) << "})";
  }
};


struct tmpvar
  : public var
{
protected:
  bool overridden;
  string override_value;

public:
  tmpvar(c_unparser *u, exp_type ty, unsigned & counter)
    : var(u, true, ty, counter),
      overridden(false)
  {}

  tmpvar(const var& source)
    : var(source), overridden(false)
  {}

  void override(const string &value)
  {
    overridden = true;
    override_value = value;
  }

  bool is_overridden()
  {
    return overridden;
  }

  string value() const
  {
    if (overridden)
      return override_value;
    else
      return var::value();
  }
};

ostream & operator<<(ostream & o, tmpvar const & v)
{
  return o << v.value();
}

struct aggvar
  : public var
{
  aggvar(c_unparser *u, unsigned & counter)
    : var(u, true, pe_stats, counter)
  {}

  string init() const
  {
    assert (type() == pe_stats);
    return value() + " = NULL;";
  }

  void declare(c_unparser &c) const cxx_override
  {
    assert (type() == pe_stats);
    c.o->newline() << "struct stat_data *" << name << ";";
  }

  string get_hist (var& index) const
  {
    return "(" + value() + "->histogram[" + index.value() + "])";
  }
};

struct mapvar
  : public var
{
  vector<exp_type> index_types;
  int maxsize;
  bool wrap;
  mapvar (c_unparser *u,
          bool local, exp_type ty,
	  statistic_decl const & sd,
	  string const & name,
	  vector<exp_type> const & index_types,
	  int maxsize, bool wrap)
    : var (u, local, ty, sd, name),
      index_types (index_types),
      maxsize (maxsize), wrap(wrap)
  {}

  static string shortname(exp_type e);
  static string key_typename(exp_type e);
  static string value_typename(exp_type e);

  string keysym () const
  {
    string result;
    vector<exp_type> tmp = index_types;
    tmp.push_back (type ());
    for (unsigned i = 0; i < tmp.size(); ++i)
      {
	switch (tmp[i])
	  {
	  case pe_long:
	    result += 'i';
	    break;
	  case pe_string:
	    result += 's';
	    break;
	  case pe_stats:
	    result += 'x';
	    break;
	  default:
	    throw SEMANTIC_ERROR(_("unknown type of map"));
	    break;
	  }
      }
    return result;
  }

  string function_keysym(string const & fname, bool pre_agg=false) const
  {
    string mtype = (is_parallel() && !pre_agg) ? "pmap" : "map";
    string result = "_stp_" + mtype + "_" + fname + "_" + keysym();
    return result;
  }

  string call_prefix (string const & fname, vector<tmpvar> const & indices, bool pre_agg=false) const
  {
    string result = function_keysym(fname, pre_agg) + " (";
    result += pre_agg? fetch_existing_aggregate() : value();
    for (unsigned i = 0; i < indices.size(); ++i)
      {
	if (indices[i].type() != index_types[i])
	  throw SEMANTIC_ERROR(_("index type mismatch"));
	result += ", ";
	result += indices[i].value();
      }

    return result;
  }

  bool is_parallel() const
  {
    return type() == pe_stats;
  }

  string stat_op_tokens() const
  {
    string result = "";
    if (sd.stat_ops & STAT_OP_COUNT)
      result += "STAT_OP_COUNT, ";
    if (sd.stat_ops & STAT_OP_SUM)
      result += "STAT_OP_SUM, ";
    if (sd.stat_ops & STAT_OP_MIN)
      result += "STAT_OP_MIN, ";
    if (sd.stat_ops & STAT_OP_MAX)
      result += "STAT_OP_MAX, ";
    if (sd.stat_ops & STAT_OP_AVG)
      result += "STAT_OP_AVG, ";
    if (sd.stat_ops & STAT_OP_VARIANCE)
      result += "STAT_OP_VARIANCE, " + lex_cast(sd.bit_shift) + ", ";

    return result;
  }

  string stat_op_parms() const
  {
    string result = "";
    result += (sd.stat_ops & (STAT_OP_COUNT|STAT_OP_AVG|STAT_OP_VARIANCE)) ? "1, " : "0, ";
    result += (sd.stat_ops & (STAT_OP_SUM|STAT_OP_AVG|STAT_OP_VARIANCE)) ? "1, " : "0, ";
    result += (sd.stat_ops & STAT_OP_MIN) ? "1, " : "0, ";
    result += (sd.stat_ops & STAT_OP_MAX) ? "1, " : "0, ";
    result += (sd.stat_ops & STAT_OP_VARIANCE) ? "1" : "0";
    return result;
  }

  string calculate_aggregate() const
  {
    if (!is_parallel())
      throw SEMANTIC_ERROR(_("aggregating non-parallel map type"));

    return function_keysym("agg") + " (" + value() + ")";
  }

  string fetch_existing_aggregate() const
  {
    if (!is_parallel())
      throw SEMANTIC_ERROR(_("fetching aggregate of non-parallel map type"));

    return "_stp_pmap_get_agg(" + value() + ")";
  }

  string del (vector<tmpvar> const & indices) const
  {
    return (call_prefix("del", indices) + ")");
  }

  string exists (vector<tmpvar> const & indices) const
  {
    if (type() == pe_long || type() == pe_string)
      return (call_prefix("exists", indices) + ")");
    else if (type() == pe_stats)
      return ("((uintptr_t)" + call_prefix("get", indices)
	      + ") != (uintptr_t) 0)");
    else
      throw SEMANTIC_ERROR(_("checking existence of an unsupported map type"));
  }

  string get (vector<tmpvar> const & indices, bool pre_agg=false) const
  {
    // see also itervar::get_key
    if (type() == pe_string)
        // impedance matching: NULL -> empty strings
      return ("({ char *v = " + call_prefix("get", indices, pre_agg) + ");"
	      + "if (!v) v = \"\"; v; })");
    else if (type() == pe_long || type() == pe_stats)
      return call_prefix("get", indices, pre_agg) + ")";
    else
      throw SEMANTIC_ERROR(_("getting a value from an unsupported map type"));
  }

  string add (vector<tmpvar> const & indices, tmpvar const & val) const
  {
    string res = "{ int rc = ";

    // impedance matching: empty strings -> NULL
    if (type() == pe_stats)
      res += (call_prefix("add", indices) + ", " + val.value() + ", " + stat_op_parms() + ")");
    else
      throw SEMANTIC_ERROR(_("adding a value of an unsupported map type"));

    res += "; if (unlikely(rc)) { c->last_error = ";
    res += STAP_T_01 +
      lex_cast(maxsize > 0 ?
	  "size limit (" + lex_cast(maxsize) + ")" : "MAXMAPENTRIES")
      + "\"; goto out; }}";

    return res;
  }

  string set (vector<tmpvar> const & indices, tmpvar const & val) const
  {
    string res = "{ int rc = ";

    // impedance matching: empty strings -> NULL
    if (type() == pe_string)
      res += (call_prefix("set", indices)
	      + ", (" + val.value() + "[0] ? " + val.value() + " : NULL))");
    else if (type() == pe_long)
      res += (call_prefix("set", indices) + ", " + val.value() + ")");
    else
      throw SEMANTIC_ERROR(_("setting a value of an unsupported map type"));

    res += "; if (unlikely(rc)) { c->last_error = ";
    res += STAP_T_01 +
      lex_cast(maxsize > 0 ?
	  "size limit (" + lex_cast(maxsize) + ")" : "MAXMAPENTRIES")
      + "\"; goto out; }}";

    return res;
  }

  string hist() const
  {
    assert (ty == pe_stats);
    assert (sd.type != statistic_decl::none);
    return "(&(" + fetch_existing_aggregate() + "->hist))";
  }

  string buckets() const
  {
    assert (ty == pe_stats);
    assert (sd.type != statistic_decl::none);
    return "(" + fetch_existing_aggregate() + "->hist.buckets)";
  }

  string init () const
  {
    if (local)
      throw SEMANTIC_ERROR(_F("unsupported local map init for %s", value().c_str()));

    string prefix = "global_set(" + c_name() + ", ";
    prefix += function_keysym("new") + " ("
      + (is_parallel() ? stat_op_tokens() : "")
      + "KEY_MAPENTRIES, " + (maxsize > 0 ? lex_cast(maxsize) : "MAXMAPENTRIES") + ", "
      + ((wrap == true) ? "KEY_STAT_WRAP, " : "");

    // See also var::init().

    // Check for errors during allocation.
    string suffix = "if (" + value () + " == NULL) rc = -ENOMEM;";

    if (type() == pe_stats)
      {
	switch (sdecl().type)
	  {
	  case statistic_decl::none:
	    prefix = prefix + "KEY_HIST_TYPE, HIST_NONE, ";
	    break;

	  case statistic_decl::linear:
	    // FIXME: check for "reasonable" values in linear stats
	    prefix = prefix + "KEY_HIST_TYPE, HIST_LINEAR, "
	      + lex_cast(sdecl().linear_low) + ", "
	      + lex_cast(sdecl().linear_high) + ", "
	      + lex_cast(sdecl().linear_step) + ", ";
	    break;

	  case statistic_decl::logarithmic:
	    prefix = prefix + "KEY_HIST_TYPE, HIST_LOG, ";
	    break;
	  }
      }

    prefix += "NULL";

    prefix = prefix + ")); ";
    return (prefix + suffix);
  }

  string fini () const
  {
    // NB: fini() is safe to call even for globals that have not
    // successfully initialized (that is to say, on NULL pointers),
    // because the runtime specifically tolerates that in its _del
    // functions.

    if (is_parallel())
      return "_stp_pmap_del (" + value() + ");";
    else
      return "_stp_map_del (" + value() + ");";
  }
};


class itervar
  : public var
{
public:

  itervar (c_unparser *u, symbol* e, unsigned & counter)
    : var(u, true, e->referent->type, counter)
  {
    if (type() == pe_unknown)
      throw SEMANTIC_ERROR(_("iterating over unknown reference type"), e->tok);
  }

  void declare(c_unparser &c) const cxx_override
  {
    c.o->newline() << "struct map_node *" << name << ";";
  }

  string start (mapvar const & mv) const
  {
    string res;

    if (mv.type() != type())
      throw SEMANTIC_ERROR(_("inconsistent iterator type in itervar::start()"));

    if (mv.is_parallel())
      return "_stp_map_start (" + mv.fetch_existing_aggregate() + ")";
    else
      return "_stp_map_start (" + mv.value() + ")";
  }

  string next (mapvar const & mv) const
  {
    if (mv.type() != type())
      throw SEMANTIC_ERROR(_("inconsistent iterator type in itervar::next()"));

    if (mv.is_parallel())
      return "_stp_map_iter (" + mv.fetch_existing_aggregate() + ", " + value() + ")";
    else
      return "_stp_map_iter (" + mv.value() + ", " + value() + ")";
  }

  // Cannot handle deleting and iterating on pmaps
  string del_next (mapvar const & mv) const
  {
    if (mv.type() != type())
      throw SEMANTIC_ERROR(_("inconsistent iterator type in itervar::next()"));

    if (mv.is_parallel())
      throw SEMANTIC_ERROR(_("deleting a value of an unsupported map type"));
    else
      return "_stp_map_iterdel (" + mv.value() + ", " + value() + ")";
  }

  string get_key (mapvar const& mv, exp_type ty, unsigned i) const
  {
    // bug translator/1175: runtime uses base index 1 for the first dimension
    // see also mapval::get
    switch (ty)
      {
      case pe_long:
	return mv.function_keysym("key_get_int64", true)
	  + " (" + value() + ", " + lex_cast(i+1) + ")";
      case pe_string:
        // impedance matching: NULL -> empty strings
	return "(" + mv.function_keysym("key_get_str", true)
	  + " (" + value() + ", " + lex_cast(i+1) + ") ?: \"\")";
      default:
	throw SEMANTIC_ERROR(_("illegal key type"));
      }
  }

  string get_value (mapvar const& mv, exp_type ty) const
  {
    if (ty != type())
      throw SEMANTIC_ERROR(_("inconsistent iterator value in itervar::get_value()"));

    switch (ty)
      {
      case pe_long:
	return mv.function_keysym("get_int64", true) + " ("+ value() + ")";
      case pe_string:
        // impedance matching: NULL -> empty strings
	return "(" + mv.function_keysym("get_str", true) + " ("+ value() + ") ?: \"\")";
      case pe_stats:
	return mv.function_keysym("get_stat_data", true) + " ("+ value() + ")";
      default:
	throw SEMANTIC_ERROR(_("illegal value type"));
      }
  }
};

ostream & operator<<(ostream & o, itervar const & v)
{
  return o << v.value();
}

// ------------------------------------------------------------------------

// translator_output moved to translator-output.cxx

// ------------------------------------------------------------------------

struct unmodified_fnargs_checker : public nop_visitor
{
  bool is_embedded;
  bool has_unmodified_fnargs;

  unmodified_fnargs_checker ():
    is_embedded(false), has_unmodified_fnargs(false)
  {}

  void visit_embeddedcode (embeddedcode *e)
    {
      is_embedded = true;
      if (e->tagged_p("/* unmodified-fnargs */"))
	has_unmodified_fnargs = true;
    }
};

bool
is_unmodified_string_fnarg (systemtap_session* sess, functiondecl* fd, vardecl* v)
{
  if (sess->unoptimized || v->type != pe_string)
    return false;

  // if it's an embedded function, trust whether it has unmodified-fnargs
  unmodified_fnargs_checker ufc;
  fd->body->visit(& ufc);
  if (ufc.is_embedded)
    return ufc.has_unmodified_fnargs;

  varuse_collecting_visitor vut (*sess);
  vut.current_function = fd;
  fd->body->visit(& vut);
  return (vut.written.find(v) == vut.written.end());
}

// If we've seen a dupe, return it; else remember this and return NULL.
probe *
c_unparser::get_probe_dupe (derived_probe *dp)
{
  if (session->unoptimized)
    return NULL;

  // Notice we're using the probe body itself instead of the emitted C
  // probe body to compare probes.  We need to do this because the
  // emitted C probe body has stuff in it like:
  //   c->last_stmt = "identifier 'printf' at foo.stp:<line>:<column>";
  //
  // which would make comparisons impossible.

  ostringstream oss;

  dp->print_dupe_stamp (oss);
  dp->body->print(oss);

  // Since the generated C changes based on whether or not the probe
  // needs locks around global variables, this needs to be reflected
  // here.  We don't want to treat as duplicate the handlers of
  // begin/end and normal probes that differ only in need_global_locks.
  oss << "# needs_global_locks: " << dp->needs_global_locks () << endl;

  // NB: dependent probe conditions *could* be listed here, but don't need to
  // be.  That's because they're only dependent on the probe body, which is
  // already "hashed" in above.

  pair<map<string, probe*>::iterator, bool> const& inserted =
    probe_contents.insert(make_pair(oss.str(), dp));

  if (inserted.second)
    return NULL; // it's new!

  // Already seen it; here's the old one:
  return inserted.first->second;
}

void
c_unparser::emit_common_header ()
{
  c_tmpcounter ct (this);

  o->newline();

  // Per CPU context for probes. Includes common shared state held for
  // all probes (defined in common_probe_context), the probe locals (union)
  // and the function locals (union).
  o->newline() << "struct context {";

  // Common state held shared by probes.
  o->newline(1) << "#include \"common_probe_context.h\"";

  // PR10516: probe locals
  o->newline() << "union {";
  o->indent(1);

  for (unsigned i=0; i<session->probes.size(); i++)
    ct.emit_probe (session->probes[i]);

  o->newline(-1) << "} probe_locals;";

  // PR10516: function locals
  o->newline() << "union {";
  o->indent(1);

  for (map<string,functiondecl*>::iterator it = session->functions.begin();
       it != session->functions.end(); it++)
    ct.emit_function (it->second);

  o->newline(-1) << "} locals [MAXNESTING+1];"; 

  // NB: The +1 above for extra room for outgoing arguments of next nested function.
  // If MAXNESTING is set too small, the args will be written, but the MAXNESTING
  // check done at c_unparser::emit_function will reject.
  //
  // This policy wastes memory (one row of locals[] that cannot really
  // be used), but trades that for smaller code (not having to check
  // c->nesting against MAXNESTING at every call site).

  // Try to catch a crazy user dude passing in -DMAXNESTING=-1, leading to a [0]-sized
  // locals[] array.
  o->newline() << "#if MAXNESTING < 0";
  o->newline() << "#error \"MAXNESTING must be positive\"";
  o->newline() << "#endif";

  // Use a separate union for compiled-printf locals, no nesting required.
  emit_compiled_printf_locals ();

  o->newline(-1) << "};\n"; // end of struct context

  o->newline() << "#include \"runtime_context.h\"";

  emit_map_type_instantiations ();

  emit_compiled_printfs();

  if (!session->runtime_usermode_p())
    {
      // Updated in probe handlers to signal that a module refresh is needed.
      // Checked and cleared by common epilogue after scheduling refresh work.
      o->newline( 0)  << "static atomic_t need_module_refresh = ATOMIC_INIT(0);";

      // We will use a workqueue to schedule module_refresh work when we need
      // to enable/disable probes.
      o->newline( 0)  << "static struct work_struct module_refresher_work;";
      o->newline( 0)  << "static void module_refresher(struct work_struct *work) {";
      o->newline( 1)  <<    "systemtap_module_refresh(NULL);";
      o->newline(-1)  << "}";

      o->newline( 0)  << "#ifdef STP_ON_THE_FLY_TIMER_ENABLE";
      o->newline( 0)  << "#include \"timer.h\"";
      o->newline( 0)  << "static struct hrtimer module_refresh_timer;";

      o->newline( 0)  << "#ifndef STP_ON_THE_FLY_INTERVAL";
      o->newline( 0)  << "#define STP_ON_THE_FLY_INTERVAL (100*1000*1000)"; // default to 100 ms
      o->newline( 0)  << "#endif";

      o->newline( 0)  << "hrtimer_return_t module_refresh_timer_cb(struct hrtimer *timer);";
      o->newline( 0)  << "hrtimer_return_t module_refresh_timer_cb(struct hrtimer *timer) {";
      o->newline(+1)  <<   "if (atomic_cmpxchg(&need_module_refresh, 1, 0) == 1)";
      // NB: one might like to invoke systemtap_module_refresh(NULL) directly from
      // here ... however hrtimers are called from an unsleepable context, so no can do.
      o->newline(+1)  <<     "queue_work(systemtap_wq, &module_refresher_work);";
      o->newline(-1)  <<   "hrtimer_set_expires(timer,";
      o->newline( 0)  <<   "  ktime_add(hrtimer_get_expires(timer),";
      o->newline( 0)  <<   "            ktime_set(0, STP_ON_THE_FLY_INTERVAL))); ";
      o->newline( 0)  <<   "return HRTIMER_RESTART;";
      o->newline(-1)  << "}";
      o->newline( 0)  << "#endif /* STP_ON_THE_FLY_TIMER_ENABLE */";
    }

  o->newline(0) << "#include \"namespaces.h\"";

  o->newline();
}


void
c_unparser::declare_compiled_printf (bool print_to_stream, const string& format)
{
  pair<bool, string> index (print_to_stream, format);
  map<pair<bool, string>, string>::iterator it = compiled_printfs.find(index);
  if (it == compiled_printfs.end())
    compiled_printfs[index] = (print_to_stream ? "stp_printf_" : "stp_sprintf_")
      + lex_cast(compiled_printfs.size() + 1);
}

const string&
c_unparser::get_compiled_printf (bool print_to_stream, const string& format)
{
  map<pair<bool, string>, string>::iterator it =
    compiled_printfs.find(make_pair(print_to_stream, format));
  if (it == compiled_printfs.end())
    throw SEMANTIC_ERROR (_("internal error translating printf"));
  return it->second;
}

const string&
c_tmpcounter::get_compiled_printf (bool print_to_stream, const string& format)
{
  parent->declare_compiled_printf (print_to_stream, format);
  return parent->get_compiled_printf (print_to_stream, format);
}

void
c_unparser::emit_compiled_printf_locals ()
{
  o->newline() << "#ifndef STP_LEGACY_PRINT";
  o->newline() << "union {";
  o->indent(1);
  map<pair<bool, string>, string>::iterator it;
  for (it = compiled_printfs.begin(); it != compiled_printfs.end(); ++it)
    {
      bool print_to_stream = it->first.first;
      const string& format_string = it->first.second;
      const string& name = it->second;
      vector<print_format::format_component> components =
	print_format::string_to_components(format_string);

      o->newline() << "struct " << name << "_locals {";
      o->indent(1);

      size_t arg_ix = 0;
      vector<print_format::format_component>::const_iterator c;
      for (c = components.begin(); c != components.end(); ++c)
	{
	  if (c->type == print_format::conv_literal)
	    continue;

	  // Take note of the width and precision arguments, if any.
	  if (c->widthtype == print_format::width_dynamic)
	    o->newline() << "int64_t arg" << arg_ix++ << ";";
	  if (c->prectype == print_format::prec_dynamic)
	    o->newline() << "int64_t arg" << arg_ix++ << ";";

	  // Output the actual argument.
	  switch (c->type)
	    {
	    case print_format::conv_pointer:
	    case print_format::conv_number:
	    case print_format::conv_char:
	    case print_format::conv_memory:
	    case print_format::conv_memory_hex:
	    case print_format::conv_binary:
	      o->newline() << "int64_t arg" << arg_ix++ << ";";
	      break;

	    case print_format::conv_string:
	      // NB: Since we know incoming strings are immutable, we can use
	      // const char* rather than a private char[] copy.  This is a
	      // special case of the sort of optimizations desired in PR11528.
	      o->newline() << "const char* arg" << arg_ix++ << ";";
	      break;

	    default:
	      assert(false); // XXX
	      break;
	    }
	}


      if (!print_to_stream)
	o->newline() << "char * __retvalue;";

      o->newline(-1) << "} " << name << ";";
    }
  o->newline(-1) << "} printf_locals;";
  o->newline() << "#endif // STP_LEGACY_PRINT";
}

void
c_unparser::emit_compiled_printfs ()
{
  o->newline() << "#ifndef STP_LEGACY_PRINT";
  map<pair<bool, string>, string>::iterator it;
  for (it = compiled_printfs.begin(); it != compiled_printfs.end(); ++it)
    {
      bool print_to_stream = it->first.first;
      const string& format_string = it->first.second;
      const string& name = it->second;
      vector<print_format::format_component> components =
	print_format::string_to_components(format_string);

      o->newline();

      // Might be nice to output the format string in a comment, but we'd have
      // to be extra careful about format strings not escaping the comment...
      o->newline() << "static void " << name
		   << " (struct context* __restrict__ c) {";
      o->newline(1) << "struct " << name << "_locals * __restrict__ l = "
		    << "& c->printf_locals." << name << ";";
      o->newline() << "char *str = NULL, *end = NULL;";
      o->newline() << "const char *src;";
      o->newline() << "int width;";
      o->newline() << "int precision;";
      o->newline() << "unsigned long ptr_value;";
      o->newline() << "int num_bytes;";

      if (print_to_stream)
	  o->newline() << "unsigned long irqflags;";

      o->newline() << "(void) width;";
      o->newline() << "(void) precision;";
      o->newline() << "(void) ptr_value;";
      o->newline() << "(void) num_bytes;";

      if (print_to_stream)
        {
	  // Compute the buffer size needed for these arguments.
	  size_t arg_ix = 0;
	  o->newline() << "num_bytes = 0;";
	  vector<print_format::format_component>::const_iterator c;
	  for (c = components.begin(); c != components.end(); ++c)
	    {
	      if (c->type == print_format::conv_literal)
		{
		  literal_string ls(c->literal_string);
		  o->newline() << "num_bytes += sizeof(";
		  visit_literal_string(&ls);
		  o->line() << ") - 1;"; // don't count the '\0'
		  continue;
		}

	      o->newline() << "width = ";
	      if (c->widthtype == print_format::width_dynamic)
		o->line() << "clamp_t(int, l->arg" << arg_ix++
		          << ", 0, STP_BUFFER_SIZE);";
	      else if (c->widthtype == print_format::width_static)
		o->line() << "clamp_t(int, " << c->width
		          << ", 0, STP_BUFFER_SIZE);";
	      else
		o->line() << "-1;";

	      o->newline() << "precision = ";
	      if (c->prectype == print_format::prec_dynamic)
		o->line() << "clamp_t(int, l->arg" << arg_ix++
		          << ", 0, STP_BUFFER_SIZE);";
	      else if (c->prectype == print_format::prec_static)
		o->line() << "clamp_t(int, " << c->precision
		          << ", 0, STP_BUFFER_SIZE);";
	      else
		o->line() << "-1;";

	      string value = "l->arg" + lex_cast(arg_ix++);
	      switch (c->type)
		{
		case print_format::conv_pointer:
		  // NB: stap < 1.3 had odd %p behavior... see _stp_vsnprintf
		  if (strverscmp(session->compatible.c_str(), "1.3") < 0)
		    {
		      o->newline() << "ptr_value = " << value << ";";
		      o->newline() << "if (width == -1)";
		      o->newline(1) << "width = 2 + 2 * sizeof(void*);";
		      o->newline(-1) << "precision = width - 2;";
		      if (!c->test_flag(print_format::fmt_flag_left))
			o->newline() << "precision = min_t(int, precision, 2 * sizeof(void*));";
		      o->newline() << "num_bytes += number_size(ptr_value, "
			<< c->base << ", width, precision, " << c->flags << ");";
		      break;
		    }
		  /* Fallthrough */
		  // else fall-through to conv_number
		case print_format::conv_number:
		  o->newline() << "num_bytes += number_size(" << value << ", "
			       << c->base << ", width, precision, " << c->flags << ");";
		  break;

		case print_format::conv_char:
		  o->newline() << "num_bytes += _stp_vsprint_char_size("
			       << value << ", width, " << c->flags << ");";
		  break;

		case print_format::conv_string:
		  o->newline() << "num_bytes += _stp_vsprint_memory_size("
			       << value << ", width, precision, 's', "
			       << c->flags << ");";
		  break;

		case print_format::conv_memory:
		case print_format::conv_memory_hex:
		  o->newline() << "num_bytes += _stp_vsprint_memory_size("
			       << "(const char*)(intptr_t)" << value
			       << ", width, precision, '"
			       << ((c->type == print_format::conv_memory) ? "m" : "M")
			       << "', " << c->flags << ");";
		  break;

		case print_format::conv_binary:
		  o->newline() << "num_bytes += _stp_vsprint_binary_size("
			       << value << ", width, precision);";
		  break;

		default:
		  assert(false); // XXX
		  break;
		}
	    }

	  o->newline() << "num_bytes = clamp(num_bytes, 0, STP_BUFFER_SIZE);";
	  o->newline() << "if (!_stp_print_trylock_irqsave(&irqflags))";
	  o->newline(1) << "return;";
	  o->newline(-1) << "str = (char*)_stp_reserve_bytes(num_bytes);";
	  o->newline() << "end = str ? str + num_bytes - 1 : 0;";
        }
      else // !print_to_stream
	{
	  // String results are a known buffer and size;
	  o->newline() << "str = l->__retvalue;";
	  o->newline() << "end = str + MAXSTRINGLEN - 1;";
	}

      o->newline() << "if (str && str <= end) {";
      o->indent(1);

      // Generate code to print the actual arguments.
      size_t arg_ix = 0;
      vector<print_format::format_component>::const_iterator c;
      for (c = components.begin(); c != components.end(); ++c)
	{
	  if (c->type == print_format::conv_literal)
	    {
	      literal_string ls(c->literal_string);
	      o->newline() << "src = ";
	      visit_literal_string(&ls);
	      o->line() << ";";
	      o->newline() << "while (*src && str <= end)";
	      o->newline(1) << "*str++ = *src++;";
              o->indent(-1);
	      continue;
	    }

	  o->newline() << "width = ";
	  if (c->widthtype == print_format::width_dynamic)
	    o->line() << "clamp_t(int, l->arg" << arg_ix++
		      << ", 0, end - str + 1);";
	  else if (c->widthtype == print_format::width_static)
	    o->line() << "clamp_t(int, " << c->width
		      << ", 0, end - str + 1);";
	  else
	    o->line() << "-1;";

	  o->newline() << "precision = ";
	  if (c->prectype == print_format::prec_dynamic)
	    o->line() << "clamp_t(int, l->arg" << arg_ix++
		      << ", 0, end - str + 1);";
	  else if (c->prectype == print_format::prec_static)
	    o->line() << "clamp_t(int, " << c->precision
		      << ", 0, end - str + 1);";
	  else
	    o->line() << "-1;";

	  string value = "l->arg" + lex_cast(arg_ix++);
	  switch (c->type)
	    {
	    case print_format::conv_pointer:
	      // NB: stap < 1.3 had odd %p behavior... see _stp_vsnprintf
	      if (strverscmp(session->compatible.c_str(), "1.3") < 0)
		{
		  o->newline() << "ptr_value = " << value << ";";
		  o->newline() << "if (width == -1)";
		  o->newline(1) << "width = 2 + 2 * sizeof(void*);";
		  o->newline(-1) << "precision = width - 2;";
		  if (!c->test_flag(print_format::fmt_flag_left))
		    o->newline() << "precision = min_t(int, precision, 2 * sizeof(void*));";
		  o->newline() << "str = number(str, end, ptr_value, "
		    << c->base << ", width, precision, " << c->flags << ");";
		  break;
		}
	      /* Fallthrough */
	      // else fall-through to conv_number
	    case print_format::conv_number:
	      o->newline() << "str = number(str, end, " << value << ", "
			   << c->base << ", width, precision, " << c->flags << ");";
	      break;

	    case print_format::conv_char:
	      o->newline() << "str = _stp_vsprint_char(str, end, "
			   << value << ", width, " << c->flags << ");";
	      break;

	    case print_format::conv_string:
	      o->newline() << "str = _stp_vsprint_memory(str, end, "
			   << value << ", width, precision, 's', "
			   << c->flags << ");";
	      break;

	    case print_format::conv_memory:
	    case print_format::conv_memory_hex:
	      o->newline() << "str = _stp_vsprint_memory(str, end, "
			   << "(const char*)(intptr_t)" << value
			   << ", width, precision, '"
			   << ((c->type == print_format::conv_memory) ? "m" : "M")
			   << "', " << c->flags << ");";
	      o->newline() << "if (unlikely(str == NULL)) {";
	      o->indent(1);
	      if (print_to_stream)
                {
		  o->newline() << "_stp_unreserve_bytes(num_bytes);";
	          o->newline() << "goto err_unlock;";
                }
              else
                {
	          o->newline() << "return;";
                }
	      o->newline(-1) << "}";
	      break;

	    case print_format::conv_binary:
	      o->newline() << "str = _stp_vsprint_binary(str, end, "
			   << value << ", width, precision, "
			   << c->flags << ");";
	      break;

	    default:
	      assert(false); // XXX
	      break;
	    }
	}

      if (!print_to_stream)
	{
	  o->newline() << "if (str <= end)";
	  o->newline(1) << "*str = '\\0';";
	  o->newline(-1) << "else";
	  o->newline(1) << "*end = '\\0';";
	  o->indent(-1);
	}

      o->newline(-1) << "}";

      if (print_to_stream)
        {
          o->newline(-1) << "err_unlock:";
          o->newline(1) << "_stp_print_unlock_irqrestore(&irqflags);";
        }
      o->newline(-1) << "}";
    }
  o->newline() << "#endif // STP_LEGACY_PRINT";
}


void
c_unparser::emit_global_param (vardecl *v)
{
  // Only true globals can be params, not private variables.
  if (!v->name.starts_with("__global_")) return;

  // Only non-synthetic globals can be params.
  if (v->synthetic) return;

  string global = c_globalname (v->name);
  interned_string param = v->name.substr(sizeof("__global_") - 1);

  // For dyninst, use the emit_global_init_* functionality instead.
  assert (!session->runtime_usermode_p());

  // NB: systemtap globals can collide with linux macros,
  // e.g. VM_FAULT_MAJOR.  We want the parameter name anyway.  This
  // #undef is spit out at the end of the C file, so that removing the
  // definition won't affect any other embedded-C or generated code.
  // XXX: better not have a global variable named module_param_named etc.!
  o->newline() << "#undef " << param; // avoid colliding with non-mangled name

  // Emit module_params for this global, if its type is convenient.
  if (v->arity == 0 && v->type == pe_long)
    {
      o->newline() << "module_param_named (" << param << ", "
                   << "global(" << global << "), int64_t, 0);";
    }
  else if (v->arity == 0 && v->type == pe_string)
    {
      // NB: no special copying is needed.
      o->newline() << "module_param_string (" << param << ", "
                   << "global(" << global << "), MAXSTRINGLEN, 0);";
    }
}


void
c_unparser::emit_global_init_setters ()
{
  // Hack for dyninst module params: setter function forms a little
  // linear lookup table ditty to find a global variable by name.
  o->newline() << "int stp_global_setter (const char *name, const char *value) {";
  o->newline(1);
  for (unsigned i=0; i<session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->arity > 0) continue;
      if (v->type != pe_string && v->type != pe_long) continue;

      // Only true globals can be params, not private variables.
      if (!v->name.starts_with("__global_")) continue;

      string global = c_globalname (v->name);
      interned_string param = v->name.substr(sizeof("__global_") - 1);

      // Do not mangle v->name for the comparison!
      o->line() << "if (0 == strcmp(name,\"" << param << "\"))" << " {";

      o->indent(1);
      if (v->type == pe_string)
        {
          c_assign("stp_global_init." + global, "value", pe_string, "BUG: global module param", v->tok);
          o->newline() << "return 0;";
        }
      else
        {
          o->newline() << "return set_int64_t(value, &stp_global_init." << global << ");";
        }

      o->newline(-1) << "} else ";
    }

  // Call the runtime function that handles session attributes, like
  // log_level, etc.
  o->line() << "return stp_session_attribute_setter(name, value);";
  o->newline(-1) << "}";
  o->newline();
}


void
c_unparser::emit_global (vardecl *v)
{
  string vn = c_globalname (v->name);

  string type;
  if (v->arity > 0)
    type = (v->type == pe_stats) ? "PMAP" : "MAP";
  else
    type = c_typename (v->type);

  if (session->runtime_usermode_p())
    {
      // In stapdyn mode, the stat/map/pmap pointers are stored as offptr_t in
      // shared memory.  However, we can keep a little type safety by emitting
      // FOO_typed and using typeof(FOO_typed) in the global() macros.
      bool offptr_p  = (v->type == pe_stats) || (v->arity > 0);
      string stored_type = offptr_p ? "offptr_t" : type;

      // NB: The casted_type is in the unused side of a __builtin_choose_expr
      // for non-offptr types, so it doesn't matter what we put for them, as
      // long as it passes syntax long enough for gcc to choose the other expr.
      string casted_type = offptr_p ? type : "void*";

      o->newline() << "union {";
      o->newline(1) << casted_type << " " << vn << "_typed;";
      o->newline() << stored_type << " " << vn << ";";
      o->newline(-1) << "};";
    }
  else
    o->newline() << type << " " << vn << ";";

  o->newline() << "stp_rwlock_t " << vn << "_lock;";
  o->newline() << "#ifdef STP_TIMING";
  o->newline() << "atomic_t " << vn << "_lock_skip_count;";
  o->newline() << "atomic_t " << vn << "_lock_contention_count;";
  o->newline() << "#endif\n";
}


void
c_unparser::emit_global_init (vardecl *v)
{
  // We can only statically initialize some scalars.
  if (v->arity == 0 && v->init)
    {
      o->newline() << "." << c_globalname (v->name) << " = ";
      v->init->visit(this);
      o->line() << ",";
    }
  else if (v->arity == 0 && session->runtime_usermode_p())
    {
      // For dyninst: always try to put a default value into the initial
      // static structure, so we don't have to guess if it was customized.
      if (v->type == pe_long)
        o->newline() << "." << c_globalname (v->name) << " = 0,";
      else if (v->type == pe_string)
        o->newline() << "." << c_globalname (v->name) << " = { '\\0' },"; // XXX: ""
    }
  // The lock and lock_skip_count are handled in emit_module_init.
}


void
c_unparser::emit_global_init_type (vardecl *v)
{
  // We can only statically initialize some scalars.
  if (v->arity == 0) // ... although we still allow !v->init here.
    {
      o->newline() << c_typename(v->type) << " " << c_globalname(v->name) << ";";
    }
}


void
c_unparser::emit_functionsig (functiondecl* v)
{
  bool funcname_shortened;
  string funcname = c_funcname (v->name, funcname_shortened);
  if (funcname_shortened)
    o->newline() << "/* " << v->name << " */";
  o->newline() << "static void " << funcname
	       << " (struct context * __restrict__ c);";
}


void
c_unparser::emit_kernel_module_init ()
{
  if (session->runtime_usermode_p())
    return;

  o->newline();
  o->newline() << "static int systemtap_kernel_module_init (void) {";
  o->newline(1) << "int rc = 0;";
  o->newline() << "int i=0, j=0;"; // for derived_probe_group use

  vector<derived_probe_group*> g = all_session_groups (*session);
  for (unsigned i=0; i<g.size(); i++)
    {
      g[i]->emit_kernel_module_init (*session);

      o->newline() << "if (rc) {";
      o->indent(1);
      if (i>0)
        {
	  for (int j=i-1; j>=0; j--)
	    g[j]->emit_kernel_module_exit (*session);
	}
      o->newline() << "goto out;";
      o->newline(-1) << "}";
    }
  o->newline(-1) << "out:";
  o->indent(1);
  o->newline() << "return rc;";
  o->newline(-1) << "}\n";
  o->assert_0_indent(); 
}


void
c_unparser::emit_kernel_module_exit ()
{
  if (session->runtime_usermode_p())
    return;

  o->newline();
  o->newline() << "static void systemtap_kernel_module_exit (void) {";
  o->newline(1) << "int i=0, j=0;"; // for derived_probe_group use

  // We're processing the derived_probe_group list in reverse order.
  // This ensures that probe groups get unregistered in reverse order
  // of the way they were registered.
  vector<derived_probe_group*> g = all_session_groups (*session);
  for (vector<derived_probe_group*>::reverse_iterator i = g.rbegin();
       i != g.rend(); i++)
    {
      (*i)->emit_kernel_module_exit (*session);
    }
  o->newline(-1) << "}\n";
  o->assert_0_indent(); 
}


void
c_unparser::emit_module_init ()
{
  vector<derived_probe_group*> g = all_session_groups (*session);
  for (unsigned i=0; i<g.size(); i++)
    {
      g[i]->emit_module_decls (*session);
      o->assert_0_indent(); 
    }

  o->newline() << "#ifdef STAP_NEED_TRACEPOINTS";
  o->newline() << "#include \"linux/stp_tracepoint.c\"";
  o->newline() << "#endif";

  o->newline();
  o->newline() << "static int systemtap_module_init (void) {";
  o->newline(1) << "int rc = 0;";
  o->newline() << "int cpu;";
  o->newline() << "int i=0, j=0;"; // for derived_probe_group use
  o->newline() << "const char *probe_point = \"\";";

  // NB: This block of initialization only makes sense in kernel
  if (! session->runtime_usermode_p())
  {
      if (!session->runtime_usermode_p())
        {
          o->newline() << "#if defined(STP_TIMING)";
          o->newline() << "#ifdef STP_TIMING_NSECS";
          o->newline() << "s64 cycles_atstart = ktime_get_ns();";
          o->newline() << "#else";
          o->newline() << "cycles_t cycles_atstart = get_cycles();";
          o->newline() << "#endif";
          o->newline() << "#endif";
        }

      // XXX Plus, most of this code is completely static, so it probably should
      // move into the runtime, where kernel/dyninst is more easily separated.

      // The systemtap_module_init() function must be run in
      // non-atomic context, since several functions might need to
      // sleep.
      o->newline() << "might_sleep();";

      // PR26074: kallsyms lookups that need to happen potentially
      // *after* getting relocations, in order to have
      // access to kallsyms_lookup_name():
      o->newline() << "rc = _stp_handle_kallsyms_lookups();";
      o->newline() << "if (rc) goto out;";

      // Compare actual and targeted kernel releases/machines.  Sometimes
      // one may install the incorrect debuginfo or -devel RPM, and try to
      // run a probe compiled for a different version.  Catch this early,
      // just in case modversions didn't.
      o->newline() << "{";
      o->newline() << "#ifndef STP_NO_VERREL_CHECK";
      o->newline(1) << "const char* release = UTS_RELEASE;";
      o->newline() << "#ifdef STAPCONF_GENERATED_COMPILE";
      o->newline() << "const char* version = UTS_VERSION;";
      o->newline() << "#endif";

      // NB: This UTS_RELEASE compile-time macro directly checks only that
      // the compile-time kbuild tree matches the compile-time debuginfo/etc.
      // It does not check the run time kernel value.  However, this is
      // probably OK since the kbuild modversions system aims to prevent
      // mismatches between kbuild and runtime versions at module-loading time.

      // o->newline() << "const char* machine = UTS_MACHINE;";
      // NB: We could compare UTS_MACHINE too, but on x86 it lies
      // (UTS_MACHINE=i386, but uname -m is i686).  Sheesh.

      // Now optional as the comparison of two compile-time values is vacuous:
      o->newline() << "#ifdef STP_FULL_VERREL_CHECK";
      o->newline() << "if (strcmp (release, "
		   << lex_cast_qstring (session->kernel_release) << ")) {";
      o->newline(1) << "_stp_error (\"module release mismatch (%s vs %s)\", "
		    << "release, "
		    << lex_cast_qstring (session->kernel_release)
		    << ");";
      o->newline() << "rc = -EINVAL;";
      o->newline(-1) << "}";
      o->newline() << "#endif";

      o->newline() << "#ifdef STAPCONF_GENERATED_COMPILE";
      o->newline() << "if (strcmp (utsname()->version, version)) {";
      o->newline(1) << "_stp_error (\"module version mismatch (%s vs %s), release %s\", "
		    << "version, "
		    << "utsname()->version, "
		    << "release"
		    << ");";
      o->newline() << "rc = -EINVAL;";
      o->newline(-1) << "}";
      o->newline() << "#endif";
      o->newline() << "#endif";

      // perform buildid-based checking if able
      o->newline() << "if (_stp_module_check()) rc = -EINVAL;";

      // Perform checking on the user's credentials vs those required to load/run this module.
      o->newline() << "if (_stp_privilege_credentials == 0) {";
      o->newline(1) << "if (STP_PRIVILEGE_CONTAINS(STP_PRIVILEGE, STP_PR_STAPDEV) ||";
      o->newline() << "    STP_PRIVILEGE_CONTAINS(STP_PRIVILEGE, STP_PR_STAPUSR)) {";
      o->newline(1) << "_stp_privilege_credentials = STP_PRIVILEGE;";
      o->newline() << "#ifdef DEBUG_PRIVILEGE";
      o->newline(1) << "_dbug(\"User's privilege credentials default to %s\\n\",";
      o->newline() << "      privilege_to_text(_stp_privilege_credentials));";
      o->newline(-1) << "#endif";
      o->newline(-1) << "}";
      o->newline() << "else {";
      o->newline(1) << "_stp_error (\"Unable to verify that you have the required privilege credentials to run this module (%s required). You must use staprun version 1.7 or higher.\",";
      o->newline() << "            privilege_to_text(STP_PRIVILEGE));";
      o->newline() << "rc = -EINVAL;";
      o->newline(-1) << "}";
      o->newline(-1) << "}";
      o->newline() << "else {";
      o->newline(1) << "#ifdef DEBUG_PRIVILEGE";
      o->newline(1) << "_dbug(\"User's privilege credentials provided as %s\\n\",";
      o->newline() << "      privilege_to_text(_stp_privilege_credentials));";
      o->newline(-1) << "#endif";
      o->newline() << "if (! STP_PRIVILEGE_CONTAINS(_stp_privilege_credentials, STP_PRIVILEGE)) {";
      o->newline(1) << "_stp_error (\"Your privilege credentials (%s) are insufficient to run this module (%s required).\",";
      o->newline () << "            privilege_to_text(_stp_privilege_credentials), privilege_to_text(STP_PRIVILEGE));";
      o->newline() << "rc = -EINVAL;";
      o->newline(-1) << "}";
      o->newline(-1) << "}";

      o->newline(-1) << "}";

      o->newline() << "if (rc) goto out;";
  }

  // Now that kernel version and permissions are correct,
  // initialize the global session states before anything else.
  o->newline() << "rc = stp_session_init();";
  o->newline() << "if (rc) {";
  o->newline(1) << "_stp_error (\"couldn't initialize the main session (rc %d)\", rc);";
  o->newline() << "goto out;";
  o->newline(-1) << "}";

  // This signals any other probes that may be invoked in the next little
  // while to abort right away.  Currently running probes are allowed to
  // terminate.  These may set STAP_SESSION_ERROR!
  //
  // Note that this *must* be done after stp_session_init() is called,
  // since that initializes the dyninst session atomics. Note that we
  // don't want to run systemtap_module_init() twice.
  o->newline() << "if (atomic_cmpxchg(session_state(), STAP_SESSION_UNINITIALIZED, STAP_SESSION_STARTING) != STAP_SESSION_UNINITIALIZED) {";
  o->newline(1) << "_stp_error (\"session has already been initialized\");";
  // Note that here we don't want to jump to "out", since we don't
  // want to deregister anything, we just want to return.
  o->newline() << "return -EALREADY;";
  o->newline(-1) << "}";

  // initialize gettimeofday (if needed)
  o->newline() << "#ifdef STAP_NEED_GETTIMEOFDAY";
  o->newline() << "rc = _stp_init_time();";  // Kick off the Big Bang.
  o->newline() << "if (rc) {";
  o->newline(1) << "_stp_error (\"couldn't initialize gettimeofday\");";
  o->newline() << "goto out;";
  o->newline(-1) << "}";
  o->newline() << "#endif";

  // initialize tracepoints (if needed)
  o->newline() << "#ifdef STAP_NEED_TRACEPOINTS";
  o->newline() << "rc = stp_tracepoint_init();";
  o->newline() << "if (rc) {";
  o->newline(1) << "_stp_error (\"couldn't initialize tracepoints\");";
  o->newline() << "goto out;";
  o->newline(-1) << "}";
  o->newline() << "#endif";

  // initialize stack setup (if needed)
  o->newline() << "#ifdef STP_NEED_UNWIND_DATA";
  o->newline() << "rc = _stp_init_stack();";
  o->newline() << "if (rc) {";
  o->newline(1) << "_stp_error (\"couldn't initialize stack support\");";
  o->newline() << "goto out;";
  o->newline(-1) << "}";
  o->newline() << "#endif";

  // NB: we don't need per-_stp_module task_finders, since a single common one
  // set up in runtime/sym.c's _stp_sym_init() will scan through all _stp_modules. XXX - check this!
  o->newline() << "(void) probe_point;";
  o->newline() << "(void) i;";
  o->newline() << "(void) j;";

  // Allocate context structures.
  o->newline() << "rc = _stp_runtime_contexts_alloc();";
  o->newline() << "if (rc != 0)";
  o->newline(1) << "goto out;";
  o->indent(-1);

  for (unsigned i=0; i<session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->index_types.size() > 0)
	o->newline() << getmap (v).init();
      else if (session->runtime_usermode_p() && v->arity == 0
               && (v->type == pe_long || v->type == pe_string))
	c_assign(getvar (v).value(), "stp_global_init." + c_globalname(v->name), v->type, "BUG: global initialization", v->tok);
      else
	o->newline() << getvar (v).init();
      // NB: in case of failure of allocation, "rc" will be set to non-zero.
      // Allocation can in general continue.

      o->newline() << "if (rc) {";
      o->newline(1) << "_stp_error (\"global variable '" << v->name << "' allocation failed\");";
      o->newline() << "goto out;";
      o->newline(-1) << "}";

      o->newline() << "global_lock_init(" << c_globalname (v->name) << ");";
      o->newline() << "#ifdef STP_TIMING";
      o->newline() << "atomic_set(global_skipped(" << c_globalname (v->name) << "), 0);";
      o->newline() << "atomic_set(global_contended(" << c_globalname (v->name) << "), 0);";
      o->newline() << "#endif";
    }

  // Print a message to the kernel log about this module.  This is
  // intended to help debug problems with systemtap modules.
  if (! session->runtime_usermode_p())
    o->newline() << "_stp_print_kernel_info("
                 << "\"" << escaped_literal_string(session->script_basename()) << "\""
                 << ", \"" << VERSION
                 << "/" << dwfl_version (NULL) << "\""
                 << ", (num_online_cpus() * sizeof(struct context))"
                 << ", " << session->probes.size()
                 << ");";
  // In dyninst mode, we need to know when all the globals have been
  // allocated and we're ready to run probe registration.
  else
    {
      o->newline() << "rc = stp_session_init_finished();";
      o->newline() << "if (rc) goto out;";
    }

  if (!session->runtime_usermode_p())
    {
      // Initialize workqueue needed for on-the-fly arming/disarming
      o->newline() << "INIT_WORK(&module_refresher_work, module_refresher);";
    }

  // Run all probe registrations.  This actually runs begin probes.

  for (unsigned i=0; i<g.size(); i++)
    {
      g[i]->emit_module_init (*session);
      // NB: this gives O(N**2) amount of code, but luckily there
      // are only seven or eight derived_probe_groups, so it's ok.
      o->newline() << "if (rc) {";
      // If a probe types's emit_module_init() wants to handle error
      // messages itself, it should set probe_point to NULL, 
      o->newline(1) << "if (probe_point)";
      o->newline(1) << "_stp_error (\"probe %s registration error [man warning::pass5] (rc %d)\", probe_point, rc);";
      o->indent(-1);
      // NB: we need to be in the error state so timers can shutdown cleanly,
      // and so end probes don't run.  OTOH, error probes can run.
      o->newline() << "atomic_set (session_state(), STAP_SESSION_ERROR);";
      if (i>0)
        for (int j=i-1; j>=0; j--)
          g[j]->emit_module_exit (*session);
      o->newline() << "goto out;";
      o->newline(-1) << "}";
    }

  // All registrations were successful.  Consider the system started.
  // NB: only other valid state value is ERROR, in which case we don't
  o->newline() << "atomic_cmpxchg(session_state(), STAP_SESSION_STARTING, STAP_SESSION_RUNNING);";

  // Run all post-session starting code.
  for (unsigned i=0; i<g.size(); i++)
    {
      g[i]->emit_module_post_init (*session);
    }

  if (!session->runtime_usermode_p())
    {
      o->newline() << "#ifdef STP_ON_THE_FLY_TIMER_ENABLE";

      // Initialize hrtimer needed for on-the-fly arming/disarming
      o->newline() << "hrtimer_init(&module_refresh_timer, CLOCK_MONOTONIC,";
      o->newline() << "             HRTIMER_MODE_REL);";
      o->newline() << "module_refresh_timer.function = &module_refresh_timer_cb;";

      // We check here if it's worth it to start the timer at all. We only need
      // the background timer if there is a probe which doesn't support directy
      // scheduling work (otf_safe_context() == false), but yet does affect the
      // condition of at least one probe which supports on-the-fly operations.
      {
        // for each derived probe...
        bool start_timer = false;
        for (unsigned i=0; i<session->probes.size() && !start_timer; i++)
          {
            // if it isn't safe in this probe type to directly schedule work,
            // and this probe could affect other probes...
            if (session->probes[i]->group
                && !session->probes[i]->group->otf_safe_context(*session)
                && !session->probes[i]->probes_with_affected_conditions.empty())
              {
                // and if any of those possible probes support on-the-fly operations,
                // then we'll need the timer
                for (set<derived_probe*>::const_iterator
                      it  = session->probes[i]->probes_with_affected_conditions.begin();
                      it != session->probes[i]->probes_with_affected_conditions.end()
                            && !start_timer; ++it)
                  {
                    if ((*it)->group && (*it)->group->otf_supported(*session))
                      start_timer = true;
                  }
              }
          }

        if (start_timer)
          {
            o->newline() << "hrtimer_start(&module_refresh_timer,";
            o->newline() << "              ktime_set(0, STP_ON_THE_FLY_INTERVAL),";
            o->newline() << "              HRTIMER_MODE_REL);";
          }
      }

      o->newline() << "#endif /* STP_ON_THE_FLY_TIMER_ENABLE */";
    }

  if (!session->runtime_usermode_p())
    {
      // see also common_probe_entryfn_epilogue()
      o->newline() << "#if defined(STP_TIMING)";
      o->newline() << "if (likely(g_module_init_timing)) {";
      o->newline() << "#ifdef STP_TIMING_NSECS";
      o->newline(1) << "s64 cycles_atend = ktime_get_ns ();";
      o->newline() << "s64 cycles_elapsed = ((s64)cycles_atend > (s64)cycles_atstart)";
      o->newline(1) << "? ((s64)cycles_atend - (s64)cycles_atstart)";
      o->newline() << ": (~(s64)0) - (s64)cycles_atstart + (s64)cycles_atend + 1;";
      o->newline(-2) << "#else";
      o->newline(1) << "cycles_t cycles_atend = get_cycles ();";
      o->newline() << "int32_t cycles_elapsed = ((int32_t)cycles_atend > (int32_t)cycles_atstart)";
      o->newline(1) << "? ((int32_t)cycles_atend - (int32_t)cycles_atstart)";
      o->newline() << ": (~(int32_t)0) - (int32_t)cycles_atstart + (int32_t)cycles_atend + 1;";
      o->newline(-2) << "#endif";
      // STP_TIMING requires min, max, avg (and thus count and sum) as well as variance.
      o->newline(1) << "preempt_disable();";
      o->newline() << "_stp_stat_add(g_module_init_timing, cycles_elapsed, 1, 1, 1, 1, 1);";
      o->newline() << "preempt_enable_no_resched();";
      o->newline(-1) << "}";
      o->newline() << "#endif";
    }

  o->newline() << "return 0;";

  // Error handling path; by now all partially registered probe groups
  // have been unregistered.
  o->newline(-1) << "deref_fault: __attribute__((unused));";
  o->newline(0) << "out:";
  o->indent(1);

  // If any registrations failed, we will need to deregister the globals,
  // as this is our only chance.
  for (unsigned i=0; i<session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->index_types.size() > 0)
	o->newline() << getmap (v).fini();
      else
	o->newline() << getvar (v).fini();
    }

  // For any partially registered/unregistered kernel facilities.
  o->newline() << "atomic_set (session_state(), STAP_SESSION_STOPPED);";
  o->newline() << "stp_synchronize_sched();";

  // In case tracepoints were started, they need to be cleaned up
  o->newline() << "#ifdef STAP_NEED_TRACEPOINTS";
  o->newline() << " stp_tracepoint_exit();";
  o->newline() << "#endif";

  // In case gettimeofday was started, it needs to be stopped
  o->newline() << "#ifdef STAP_NEED_GETTIMEOFDAY";
  o->newline() << " _stp_kill_time();";  // An error is no cause to hurry...
  o->newline() << "#endif";

  // Free up the context memory after an error too
  o->newline() << "_stp_runtime_contexts_free();";

  // Free up any timing Stats in case STP_TIMING was used
  if (!session->runtime_usermode_p())
    o->newline() << "stp_session_exit();";

  o->newline() << "return rc;";
  o->newline(-1) << "}\n";
}


void
c_unparser::emit_module_refresh ()
{
  o->newline() << "static void systemtap_module_refresh (const char *modname) {";
  o->newline(1) << "int state;";
  o->newline() << "int i=0, j=0;"; // for derived_probe_group use

  if (!session->runtime_usermode_p())
    {
      o->newline() << "#if defined(STP_TIMING)";
      o->newline() << "#ifdef STP_TIMING_NSECS";
      o->newline() << "s64 cycles_atstart = ktime_get_ns();";
      o->newline() << "#else";
      o->newline() << "cycles_t cycles_atstart = get_cycles();";
      o->newline() << "#endif";
      o->newline() << "#endif";
    }

  // Ensure we're only doing the refreshing one at a time. NB: it's important
  // that we get the lock prior to checking the session_state, in case whoever
  // is holding the lock (e.g. systemtap_module_exit()) changes it.
  if (!session->runtime_usermode_p())
    o->newline() << "mutex_lock(&module_refresh_mutex);";

  /* If we're not in STARTING/RUNNING state, don't try doing any work.
     PR16766.  We don't want to run refresh ops during e.g. STOPPING,
     so as to possibly activate uprobes near shutdown. */
  o->newline() << "state = atomic_read (session_state());";
  o->newline() << "if (state != STAP_SESSION_RUNNING && state != STAP_SESSION_STARTING) {";
  o->newline(1);
  if (!session->runtime_usermode_p())
    o->newline() << "mutex_unlock(&module_refresh_mutex);";
  o->newline() << "return;";
  o->newline(-1) << "}";

  o->newline() << "(void) i;";
  o->newline() << "(void) j;";

  vector<derived_probe_group*> g = all_session_groups (*session);
  for (unsigned i=0; i<g.size(); i++)
    {
      g[i]->emit_module_refresh (*session);
    }

  if (!session->runtime_usermode_p())
    {
      // see also common_probe_entryfn_epilogue()
      o->newline() << "#if defined(STP_TIMING)";
      o->newline() << "if (likely(g_refresh_timing)) {";
      o->newline() << "#ifdef STP_TIMING_NSECS";
      o->newline(1) << "s64 cycles_atend = ktime_get_ns ();";
      o->newline() << "s64 cycles_elapsed = ((s64)cycles_atend > (s64)cycles_atstart)";
      o->newline(1) << "? ((s64)cycles_atend - (s64)cycles_atstart)";
      o->newline() << ": (~(s64)0) - (s64)cycles_atstart + (s64)cycles_atend + 1;";
      o->newline(-2) << "#else";
      o->newline(1) << "cycles_t cycles_atend = get_cycles ();";
      o->newline() << "int32_t cycles_elapsed = ((int32_t)cycles_atend > (int32_t)cycles_atstart)";
      o->newline(1) << "? ((int32_t)cycles_atend - (int32_t)cycles_atstart)";
      o->newline() << ": (~(int32_t)0) - (int32_t)cycles_atstart + (int32_t)cycles_atend + 1;";
      o->newline(-2) << "#endif";
      // STP_TIMING requires min, max, avg (and thus count and sum) as well as variance.
      o->newline(1) << "preempt_disable();";
      o->newline() << "_stp_stat_add(g_refresh_timing, cycles_elapsed, 1, 1, 1, 1, 1);";
      o->newline() << "preempt_enable_no_resched();";      
      o->newline(-1) << "}";
      o->newline() << "#endif";
    }

  if (!session->runtime_usermode_p())
    o->newline() << "mutex_unlock(&module_refresh_mutex);";

  o->newline(-1) << "}\n";
}


void
c_unparser::emit_module_exit ()
{
  o->newline() << "static void systemtap_module_exit (void) {";
  // rc?
  o->newline(1) << "int i=0, j=0;"; // for derived_probe_group use
  o->newline() << "(void) i;";
  o->newline() << "(void) j;";
  // If we aborted startup, then everything has been cleaned up already, and
  // module_exit shouldn't even have been called.  But since it might be, let's
  // beat a hasty retreat to avoid double uninitialization.
  o->newline() << "if (atomic_read (session_state()) == STAP_SESSION_STARTING)";
  o->newline(1) << "return;";
  o->indent(-1);

  o->newline() << "if (atomic_read (session_state()) == STAP_SESSION_RUNNING)";
  // NB: only other valid state value is ERROR, in which case we don't
  o->newline(1) << "atomic_set (session_state(), STAP_SESSION_STOPPING);";
  o->indent(-1);
  // This signals any other probes that may be invoked in the next little
  // while to abort right away.  Currently running probes are allowed to
  // terminate.  These may set STAP_SESSION_ERROR!

  if (!session->runtime_usermode_p())
    {
      o->newline() << "#ifdef STP_ON_THE_FLY_TIMER_ENABLE";
      o->newline() << "hrtimer_cancel(&module_refresh_timer);";
      o->newline() << "#endif";
    }

  // cargo cult prologue ... hope to flush any pending workqueue items too
  o->newline() << "stp_synchronize_sched();";

  // Get the lock before exiting to ensure there's no one in module_refresh
  // NB: this should't be able to happen, because both the module_refresh_timer
  // and the workqueue ought to have been shut down by now.
  if (!session->runtime_usermode_p())
    o->newline() << "mutex_lock(&module_refresh_mutex);";

  // We're processing the derived_probe_group list in reverse
  // order.  This ensures that probes get unregistered in reverse
  // order of the way they were registered.
  vector<derived_probe_group*> g = all_session_groups (*session);
  for (vector<derived_probe_group*>::reverse_iterator i = g.rbegin();
       i != g.rend(); i++)
    (*i)->emit_module_exit (*session); // NB: runs "end" probes

  if (!session->runtime_usermode_p())
    o->newline() << "mutex_unlock(&module_refresh_mutex);";

  // But some other probes may have launched too during unregistration.
  // Let's wait a while to make sure they're all done, done, done.

  // cargo cult prologue
  o->newline() << "stp_synchronize_sched();";

  // NB: systemtap_module_exit is assumed to be called from ordinary
  // user context, say during module unload.  Among other things, this
  // means we can sleep a while.
  o->newline() << "_stp_runtime_context_wait();";

  // cargo cult epilogue
  o->newline() << "atomic_set (session_state(), STAP_SESSION_STOPPED);";
  o->newline() << "stp_synchronize_sched();";

  // XXX: might like to have an escape hatch, in case some probe is
  // genuinely stuck somehow

  for (unsigned i=0; i<session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->index_types.size() > 0)
	o->newline() << getmap (v).fini();
      else
	o->newline() << getvar (v).fini();
    }

  // We're finished with the contexts if we're not in dyninst
  // mode. The dyninst mode needs the contexts, since print buffers
  // are stored there.
  if (!session->runtime_usermode_p())
    {
      o->newline() << "_stp_runtime_contexts_free();";
    }
  else
    {
      o->newline() << "struct context* __restrict__ c;";
      o->newline() << "c = _stp_runtime_entryfn_get_context();";
    }

  // teardown tracepoints (if needed)
  o->newline() << "#ifdef STAP_NEED_TRACEPOINTS";
  o->newline() << " stp_tracepoint_exit();";
  o->newline() << "#endif";

  // teardown gettimeofday (if needed)
  o->newline() << "#ifdef STAP_NEED_GETTIMEOFDAY";
  o->newline() << " _stp_kill_time();";  // Go to a beach.  Drink a beer.
  o->newline() << "#endif";

  // NB: PR13386 points out that _stp_printf may be called from contexts
  // without already active preempt disabling, which breaks various uses
  // of smp_processor_id().  So we temporary block preemption around this
  // whole printing block.  XXX: get_cpu() / put_cpu() may work just as well.
  o->newline() << "preempt_disable();";

  // print per probe point timing/alibi statistics
  o->newline() << "#if defined(STP_TIMING) || defined(STP_ALIBI)";
  o->newline() << "#ifndef STP_STDOUT_NOT_ATTY";
  o->newline() << "_stp_printf(\"----- probe hit report: \\n\");";
  o->newline() << "#endif"; // !defined(STP_STDOUT_NOT_ATTY)
  o->newline() << "for (i = 0; i < ARRAY_SIZE(stap_probes); ++i) {";
  o->newline(1) << "const struct stap_probe *const p = &stap_probes[i];";
  o->newline() << "#ifndef STP_STDOUT_NOT_ATTY";
  o->newline() << "#ifdef STP_ALIBI";
  o->newline() << "int alibi = atomic_read(probe_alibi(i));";
  o->newline() << "if (alibi)";
  o->newline(1) << "_stp_printf (\"%s, (%s), hits: %d,%s, index: %d\\n\",";
  o->newline(2) << "p->pp, p->location, alibi, p->derivation, i);";
  o->newline(-3) << "#endif"; // STP_ALIBI
  o->newline() << "#endif"; // !defined(STP_STDOUT_NOT_ATTY)
  o->newline() << "#ifdef STP_TIMING";
  o->newline() << "if (likely (probe_timing(i))) {"; // NB: check for null stat object
  o->newline() << "#ifndef STP_STDOUT_NOT_ATTY";
  o->newline(1) << "struct stat_data *stats = _stp_stat_get (probe_timing(i), 0);";
  o->newline() << "if (stats->count) {";
  o->newline(1) << "int64_t avg = _stp_div64 (NULL, stats->sum, stats->count);";
  o->newline() << "_stp_printf (\"%s, (%s), hits: %lld, \"";
  o->newline() << "#ifdef STP_TIMING_NSECS";
  o->newline(2) << "\"nsecs\"";
  o->newline(-2) << "#else";
  o->newline(2) << (!session->runtime_usermode_p() ? "\"cycles\"" : "\"nsecs\"");
  o->newline(-2) << "#endif";
  o->newline(2) << "\": %lldmin/%lldavg/%lldmax, variance: %lld,%s, index: %d\\n\",";
  o->newline() << "p->pp, p->location, (long long) stats->count,";
  o->newline() << "(long long) stats->min, (long long) avg, (long long) stats->max,";
  o->newline() << "(long long) stats->variance, p->derivation, i);";
  o->newline(-3) << "}";
  o->newline() << "#endif"; // !defined(STP_STDOUT_NOT_ATTY)
  o->newline() << "preempt_enable_no_resched();";
  o->newline() << "_stp_stat_del (probe_timing(i));";
  o->newline() << "preempt_disable();";
  o->newline(-1) << "}";
  o->newline() << "#endif"; // STP_TIMING
  o->newline(-1) << "}";

  if (!session->runtime_usermode_p())
    {
      o->newline() << "#if !defined(STP_STDOUT_NOT_ATTY) && defined(STP_TIMING)";

      /* module refresh timing report */

      o->newline() << "_stp_printf(\"----- refresh report:\\n\");";
      o->newline() << "if (likely (g_refresh_timing)) {";
      o->newline(1) << "struct stat_data *stats = _stp_stat_get (g_refresh_timing, 0);";
      o->newline() << "if (stats->count) {";
      o->newline(1) << "int64_t avg = _stp_div64 (NULL, stats->sum, stats->count);";
      o->newline() << "_stp_printf (\"hits: %lld, \"";
      o->newline() << "#ifdef STP_TIMING_NSECS";
      o->newline(2) << "\"nsecs\"";
      o->newline(-2) << "#else";
      o->newline(2) << "\"cycles\"";
      o->newline(-2) << "#endif";
      o->newline(2) << "\": %lldmin/%lldavg/%lldmax, variance: %lld\\n\",";
      o->newline() << "(long long) stats->count, (long long) stats->min, ";
      o->newline() <<  "(long long) avg, (long long) stats->max, (long long) stats->variance);";
      o->newline(-3) << "}";
      o->newline() << "preempt_enable_no_resched();";
      o->newline() << "_stp_stat_del (g_refresh_timing);";
      o->newline() << "preempt_disable();";
      o->newline(-1) << "}";

      /* module init timing report */

      o->newline() << "_stp_printf(\"----- module init report:\\n\");";
      o->newline() << "if (likely (g_module_init_timing)) {";
      o->newline(1) << "struct stat_data *stats = _stp_stat_get (g_module_init_timing, 0);";
      o->newline() << "if (stats->count) {";
      o->newline(1) << "int64_t avg = _stp_div64 (NULL, stats->sum, stats->count);";
      o->newline() << "_stp_printf (\"hits: %lld, \"";
      o->newline() << "#ifdef STP_TIMING_NSECS";
      o->newline(2) << "\"nsecs\"";
      o->newline(-2) << "#else";
      o->newline(2) << "\"cycles\"";
      o->newline(-2) << "#endif";
      o->newline(2) << "\": %lldmin/%lldavg/%lldmax, variance: %lld\\n\",";
      o->newline() << "(long long) stats->count, (long long) stats->min, ";
      o->newline() <<  "(long long) avg, (long long) stats->max, (long long) stats->variance);";
      o->newline(-3) << "}";
      o->newline() << "preempt_enable_no_resched();";
      o->newline() << "_stp_stat_del (g_module_init_timing);";
      o->newline() << "preempt_disable();";
      o->newline(-1) << "}";

      o->newline() << "#elif defined(STP_TIMING)"; // STP_TIMING

      o->newline() << "if (likely (g_refresh_timing)) {";
      o->newline(1) << "preempt_enable_no_resched();";
      o->newline() << "_stp_stat_del (g_refresh_timing);";
      o->newline() << "preempt_disable();";
      o->newline(-1) << "}";

      o->newline() << "if (likely (g_module_init_timing)) {";
      o->newline(1) << "preempt_enable_no_resched();";
      o->newline() << "_stp_stat_del (g_module_init_timing);";
      o->newline() << "preempt_disable();";
      o->newline(-1) << "}";

      o->newline() << "#endif"; // STP_TIMING
    }

  o->newline() << "_stp_print_flush();";
  o->newline() << "#endif";

  //print lock contentions if non-zero
  o->newline() << "#ifdef STP_TIMING";
  o->newline() << "{";
  o->newline(1) << "int ctr;";
  for (unsigned i=0; i<session->globals.size(); i++)
    {
      string orig_vn = session->globals[i]->name;
      string vn = c_globalname (orig_vn);
      o->newline() << "ctr = atomic_read (global_contended(" << vn << "));";
      o->newline() << "if (ctr) _stp_printf(\"'%s' lock contention occurred %d times\\n\", "
	           << lex_cast_qstring(orig_vn) << ", ctr);";
    }
  o->newline(-1) << "}";
  o->newline() << "_stp_print_flush();";
  o->newline () << "#endif";

  // print final error/skipped counts if non-zero
  o->newline() << "if (atomic_read (skipped_count()) || "
               << "atomic_read (error_count()) || "
               << "atomic_read (skipped_count_reentrant())) {"; // PR9967
  o->newline(1) << "_stp_warn (\"Number of errors: %d, "
                << "skipped probes: %d\\n\", "
                << "(int) atomic_read (error_count()), "
                << "(int) atomic_read (skipped_count()));";
  o->newline() << "#ifdef STP_TIMING";
  o->newline() << "{";
  o->newline(1) << "int ctr;";
  for (unsigned i=0; i<session->globals.size(); i++)
    {
      string orig_vn = session->globals[i]->name;
      string vn = c_globalname (orig_vn);
      o->newline() << "ctr = atomic_read (global_skipped(" << vn << "));";
      o->newline() << "if (ctr) _stp_warn (\"Skipped due to global '%s' lock timeout: %d\\n\", "
                   << lex_cast_qstring(orig_vn) << ", ctr);";
    }
  o->newline() << "ctr = atomic_read (skipped_count_lowstack());";
  o->newline() << "if (ctr) _stp_warn (\"Skipped due to low stack: %d\\n\", ctr);";
  o->newline() << "ctr = atomic_read (skipped_count_reentrant());";
  o->newline() << "if (ctr) _stp_warn (\"Skipped due to reentrancy: %d\\n\", ctr);";
  o->newline() << "ctr = atomic_read (skipped_count_uprobe_reg());";
  o->newline() << "if (ctr) _stp_warn (\"Skipped due to uprobe register failure: %d\\n\", ctr);";
  o->newline() << "ctr = atomic_read (skipped_count_uprobe_unreg());";
  o->newline() << "if (ctr) _stp_warn (\"Skipped due to uprobe unregister failure: %d\\n\", ctr);";
  o->newline(-1) << "}";
  o->newline () << "#endif";
  o->newline() << "_stp_print_flush();";
  o->newline(-1) << "}";

  // NB: PR13386 needs to restore preemption-blocking counts
  o->newline() << "preempt_enable_no_resched();";

  // In dyninst mode, now we're done with the contexts, transport, everything!
  if (session->runtime_usermode_p())
    {
      o->newline() << "_stp_runtime_entryfn_put_context(c);";
      o->newline() << "_stp_dyninst_transport_shutdown();";
      o->newline() << "_stp_runtime_contexts_free();";
    }

  o->newline(-1) << "}\n";
}

struct max_action_info: public functioncall_traversing_visitor
{
  max_action_info(systemtap_session& s): sess(s), statement_count(0) {}

  systemtap_session& sess;
  unsigned statement_count;
  static const unsigned max_statement_count = ~0;

  void add_stmt_count (unsigned val)
    {
      statement_count = (statement_count > max_statement_count - val) ? max_statement_count : statement_count + val;
    }
  void add_max_stmt_count () { statement_count = max_statement_count; }
  bool statement_count_finite() { return statement_count < max_statement_count; }

  void visit_for_loop (for_loop*) { add_max_stmt_count(); }
  void visit_foreach_loop (foreach_loop*) { add_max_stmt_count(); }
  void visit_expr_statement (expr_statement *stmt)
    {
      add_stmt_count(1);
      traversing_visitor::visit_expr_statement(stmt); // which will trigger visit_functioncall, if applicable
    }
  void visit_if_statement (if_statement *stmt)
    {
      add_stmt_count(1);
      stmt->condition->visit(this);

      // Create new visitors for the two forks.  Copy the nested[] set
      // to prevent infinite recursion for a   function f () { if (a) f() }
      max_action_info tmp_visitor_then (*this);
      max_action_info tmp_visitor_else (*this);
      stmt->thenblock->visit(& tmp_visitor_then);
      if (stmt->elseblock)
        {
          stmt->elseblock->visit(& tmp_visitor_else);
        }

      // Simply overwrite our copy of statement_count, since these
      // visitor copies already included our starting count.
      statement_count = max(tmp_visitor_then.statement_count, tmp_visitor_else.statement_count);
    }

  void note_recursive_functioncall (functioncall *) { add_max_stmt_count(); }

  void visit_null_statement (null_statement *) { add_stmt_count(1); }
  void visit_return_statement (return_statement *) { add_stmt_count(1); }
  void visit_delete_statement (delete_statement *) { add_stmt_count(1); }
  void visit_next_statement (next_statement *) { add_stmt_count(1); }
  void visit_break_statement (break_statement *) { add_stmt_count(1); }
  void visit_continue_statement (continue_statement *) { add_stmt_count(1); }
};

void
c_tmpcounter::emit_function (functiondecl* fd)
{
  this->current_probe = 0;
  this->current_function = fd;
  this->tmpvar_counter = 0;
  this->action_counter = 0;
  this->already_checked_action_count = false;
  declared_vars.clear();

  translator_output *o = parent->o;

  // indent the dummy output as if we were already in a block
  this->o->indent (1);

  bool funcname_shortened;
  string funcname = c_funcname (fd->name, funcname_shortened);
  if (funcname_shortened)
    o->newline() << "/* " << fd->name << " */";
  o->newline() << "struct " << funcname << "_locals {";
  o->indent(1);

  for (unsigned j=0; j<fd->locals.size(); j++)
    {
      vardecl* v = fd->locals[j];
      try
	{
	  if (fd->mangle_oldstyle)
	    {
	      // PR14524: retain old way of referring to the locals
	      o->newline() << "union { "
			   << c_typename (v->type) << " "
			   << c_localname (v->name) << "; "
			   << c_typename (v->type) << " "
			   << c_localname (v->name, true) << "; };";
	    }
	  else
	    {
	      o->newline() << c_typename (v->type) << " "
			   << c_localname (v->name) << ";";
	    }
	} catch (const semantic_error& e) {
	  semantic_error e2 (e);
	  if (e2.tok1 == 0) e2.tok1 = v->tok;
	  throw e2;
	}
    }

  for (unsigned j=0; j<fd->formal_args.size(); j++)
    {
      vardecl* v = fd->formal_args[j];
      try
	{
	  v->char_ptr_arg = (is_unmodified_string_fnarg (session, fd, v));

	  if (v->char_ptr_arg && session->verbose > 2)
	    clog << _F("variable %s for function %s will be passed by reference (char *)",
		       v->name.to_string().c_str(),
		       fd->unmangled_name.to_string().c_str()) << endl;

	  if (fd->mangle_oldstyle)
	    {
	      // PR14524: retain old way of referring to the locals
	      o->newline() << "union { "
			   << (v->char_ptr_arg ? "const char *" : c_typename (v->type))
			   << " " << c_localname (v->name) << "; "
			   << (v->char_ptr_arg ? "const char *" : c_typename (v->type))
			   << " " << c_localname (v->name, true) << "; };";
	    }
	  else
	    {
	      o->newline() << (v->char_ptr_arg ? "const char *" : c_typename (v->type))
			   << " " << c_localname (v->name) << ";";
	    }
	} catch (const semantic_error& e) {
	  semantic_error e2 (e);
	  if (e2.tok1 == 0) e2.tok1 = v->tok;
	  throw e2;
	}
    }

  fd->body->visit (this);

  if (fd->type == pe_unknown)
    o->newline() << "/* no return value */";
  else
    {
      bool as_charp = !session->unoptimized && fd->type == pe_string;
      if (as_charp && session->verbose > 2)
	clog << _F("return value for function %s will be passed by reference (char *)",
		   fd->unmangled_name.to_string().c_str()) << endl;
      o->newline() << (as_charp ? "char *" : c_typename (fd->type))
		   << " __retvalue;";
    }
  o->newline(-1) << "} " << c_funcname (fd->name) << ";";

  // finish dummy indentation
  this->o->indent (-1);
  this->o->assert_0_indent ();

  declared_vars.clear();
  this->current_function = 0;
  this->already_checked_action_count = false;
}

void
c_unparser::emit_function (functiondecl* v)
{
  this->current_probe = 0;
  this->current_function = v;
  this->tmpvar_counter = 0;
  this->action_counter = 0;
  this->already_checked_action_count = false;

  bool funcname_shortened;
  string funcname = c_funcname (v->name, funcname_shortened);
  if (funcname_shortened)
    o->newline() << "/* " << v->name << " */";
  o->newline() << "static void " << funcname
            << " (struct context* __restrict__ c) {";
  o->indent(1);

  o->newline() << "__label__ deref_fault;";
  o->newline() << "__label__ out;";
  o->newline()
    << "struct " << c_funcname (v->name) << "_locals * "
    << " __restrict__ l = "
    << "& c->locals[c->nesting+1]." << c_funcname (v->name) // NB: nesting+1
    << ";";
  o->newline() << "(void) l;"; // make sure "l" is marked used
  o->newline() << "#define CONTEXT c";
  o->newline() << "#define THIS l";
  for (unsigned i = 0; i < v->formal_args.size(); i++) {
    o->newline() << c_arg_define(v->formal_args[i]->name); // #define STAP_ARG_foo ...
  }
  for (unsigned i = 0; i < v->locals.size(); i++) {
    o->newline() << c_arg_define(v->locals[i]->name); // #define STAP_ARG_foo ...
  }
  // define STAP_RETVALUE only if the function is non-void
  if (v->type != pe_unknown)
    o->newline() << "#define STAP_RETVALUE THIS->__retvalue";

  // set this, in case embedded-c code sets last_error but doesn't otherwise identify itself
  if (v->tok)
    o->newline() << "c->last_stmt = " << lex_cast_qstring(*v->tok) << ";";

  // check/increment nesting level
  // NB: incoming c->nesting level will be -1 (if we're called directly from a probe),
  // or 0...N (if we're called from another function).  Incoming parameters are already
  // stored in c->locals[c->nesting+1].  See also ::emit_common_header() for more.

  o->newline() << "if (unlikely (c->nesting+1 >= MAXNESTING)) {";
  o->newline(1) << "c->last_error = ";
  o->line() << STAP_T_02;
  o->newline() << "return;";
  o->newline(-1) << "} else {";
  o->newline(1) << "c->nesting ++;";
  o->newline(-1) << "}";

  // initialize runtime overloading flag
  o->newline() << "c->next = 0;";
  o->newline() << "#define STAP_NEXT do { c->next = 1; goto out; } while(0)";

  // initialize locals
  // XXX: optimization: use memset instead
  for (unsigned i=0; i<v->locals.size(); i++)
    {
      if (v->locals[i]->index_types.size() > 0) // array?
	throw SEMANTIC_ERROR (_("array locals not supported, missing global declaration?"),
                              v->locals[i]->tok);

      o->newline() << getvar (v->locals[i]).init();
    }

  // initialize return value, if any
  if (v->type != pe_unknown)
    {
      var retvalue = var(this, true, v->type, "__retvalue", false); // not mangled
      o->newline() << retvalue.init();
    }

  switch (v->type)
    {
    case pe_long:
      o->newline() << "#define STAP_RETURN(v) do { STAP_RETVALUE = (int64_t) (v); " 
        "goto out; } while(0)";
      break;

    case pe_string:
      o->newline() <<
        "#define STAP_RETURN(v) do { strlcpy(STAP_RETVALUE, (v), MAXSTRINGLEN); "
        "goto out; } while(0)";
      break;

    default:
      o->newline() << "#define STAP_RETURN() do { goto out; } while(0)";
      break;
    }

  o->newline() << "#define STAP_PRINTF(fmt, ...) do { _stp_printf(fmt, ##__VA_ARGS__); } while (0)";
  o->newline() << "#define STAP_ERROR(...) do { snprintf(CONTEXT->error_buffer, MAXSTRINGLEN, __VA_ARGS__); CONTEXT->last_error = CONTEXT->error_buffer; goto out; } while (0)";
  o->newline() << "#define return goto out"; // redirect embedded-C return

  max_action_info mai (*session);
  v->body->visit (&mai);

  if (mai.statement_count_finite() && !session->suppress_time_limits
      && !session->unoptimized) // this is a finite-statement-count function
    {
      o->newline() << "if (c->actionremaining < " << mai.statement_count
                   << ") { c->last_error = " << STAP_T_04 << "goto out; }";
      this->already_checked_action_count = true;
    }

  v->body->visit (this);
  o->newline() << "#undef return";
  o->newline() << "#undef STAP_PRINTF";
  o->newline() << "#undef STAP_ERROR";
  o->newline() << "#undef STAP_RETURN";

  this->current_function = 0;

  record_actions(0, v->body->tok, true);

  o->newline(-1) << "deref_fault: __attribute__((unused));";
  o->newline(0) << "out: __attribute__((unused));";

  // Function prologue: this is why we redirect the "return" above.
  // Decrement nesting level.
  o->newline(1) << "c->nesting --;";

  o->newline() << "#undef CONTEXT";
  o->newline() << "#undef THIS";
  o->newline() << "#undef STAP_NEXT";
  for (unsigned i = 0; i < v->formal_args.size(); i++) {
    o->newline() << c_arg_undef(v->formal_args[i]->name); // #undef STAP_ARG_foo
  }
  for (unsigned i = 0; i < v->locals.size(); i++) {
    o->newline() << c_arg_undef(v->locals[i]->name); // #undef STAP_ARG_foo
  }
  o->newline() << "#undef STAP_RETVALUE";
  o->newline(-1) << "}\n";

  this->current_function = 0;
  this->already_checked_action_count = false;
}

void
c_tmpcounter::emit_probe (derived_probe* dp)
{
  this->current_function = 0;
  this->current_probe = dp;
  this->tmpvar_counter = 0;
  this->action_counter = 0;
  this->already_checked_action_count = false;
  declared_vars.clear();
  pushdown_lock.clear();
  pushdown_unlock.clear();

  if (get_probe_dupe (dp) == NULL)
    {
      translator_output *o = parent->o;

      // indent the dummy output as if we were already in a block
      this->o->indent (1);

      o->newline() << "struct " << dp->name() << "_locals {";
      o->indent(1);
      for (unsigned j=0; j<dp->locals.size(); j++)
	{
	  vardecl* v = dp->locals[j];
	  try
	    {
	      o->newline() << c_typename (v->type) << " "
			   << c_localname (v->name) << ";";
	    } catch (const semantic_error& e) {
	    semantic_error e2 (e);
	    if (e2.tok1 == 0) e2.tok1 = v->tok;
	    throw e2;
	  }
	}

      dp->body->visit (this);

      // finish by visiting conditions of affected probes to match
      // c_unparser::emit_probe()
      if (!dp->probes_with_affected_conditions.empty())
	{
	  for (set<derived_probe*>::const_iterator
		it  = dp->probes_with_affected_conditions.begin();
		it != dp->probes_with_affected_conditions.end(); ++it)
	    (*it)->sole_location()->condition->visit(this);
	}

      o->newline(-1) << "} " << dp->name() << ";";

      // finish dummy indentation
      this->o->indent (-1);
      this->o->assert_0_indent ();
    }

  declared_vars.clear();
  pushdown_lock.clear();
  pushdown_unlock.clear();
  this->current_probe = 0;
  this->already_checked_action_count = false;
}

#define DUPMETHOD_CALL 0
#define DUPMETHOD_ALIAS 0
#define DUPMETHOD_RENAME 1


void
c_unparser::emit_probe (derived_probe* v)
{
  this->current_function = 0;
  this->current_probe = v;
  this->tmpvar_counter = 0;
  this->action_counter = 0;
  this->already_checked_action_count = false;

  // If we about to emit a probe that is exactly the same as another
  // probe previously emitted, make the second probe just call the
  // first one.
  probe *dupe = get_probe_dupe (v);
  if (dupe != NULL)
    {
      // NB: Elision of context variable structs is a separate
      // operation which has already taken place by now.
      if (session->verbose > 1)
        clog << _F("%s elided, duplicates %s\n",
		   v->name().c_str(), dupe->name().c_str());

#if DUPMETHOD_CALL
      // This one emits a direct call to the first copy.
      o->newline();
      o->newline() << "static void " << v->name() << " (struct context * __restrict__ c) ";
      o->newline() << "{ " << dupe->name() << " (c); }";
#elif DUPMETHOD_ALIAS
      // This one defines a function alias, arranging gcc to emit
      // several equivalent symbols for the same function body.
      // For some reason, on gcc 4.1, this is twice as slow as
      // the CALL option.
      o->newline();
      o->newline() << "static void " << v->name() << " (struct context * __restrict__ c) ";
      o->line() << "__attribute__ ((alias (\"" << dupe->name() << "\")));";
#elif DUPMETHOD_RENAME
      // This one is sneaky.  It emits nothing for duplicate probe
      // handlers.  It instead redirects subsequent references to the
      // probe handler function to the first copy, *by name*.
      v->id = dupe->id;
#else
#error "Unknown duplicate elimination method"
#endif
    }
  else // This probe is unique.  Remember it and output it.
    {
      o->newline();
      o->newline() << "static void " << v->name() << " (struct context * __restrict__ c) ";
      o->line () << "{";
      o->indent (1);

      o->newline() << "__label__ deref_fault;";
      o->newline() << "__label__ out;";

      // emit static read/write lock decls for global variables
      if (v->needs_global_locks ())
        {
          varuse_collecting_visitor vut(*session);
          v->body->visit (& vut);

          // PR26296
          // ... so we know the probe handler body will need to lock 
          pushdown_lock.insert(v->body);

          // also visit any probe conditions which this current probe might
          // evaluate so that read locks are emitted as necessary: e.g. suppose
          //    probe X if (a || b) {...} probe Y {a = ...} probe Z {b = ...}
          // then Y and Z will already write-lock a and b respectively, but they
          // also need a read-lock on b and a respectively, since they will read
          // them when evaluating the new cond_enabled field (see c_unparser::
          // emit_probe_condition_update()).
          for (set<derived_probe*>::const_iterator
                it  = v->probes_with_affected_conditions.begin();
                it != v->probes_with_affected_conditions.end(); ++it)
            {
              assert((*it)->sole_location()->condition != NULL);
              (*it)->sole_location()->condition->visit (& vut);
            }

          // If there are no probe conditions affected by this probe, then emit
          // the unlock somewhere in the normal handler.  Otherwise, we need the
          // unlock done in a fixed location, AFTER all the condition expressions.
          // PR26296
          if (v->probes_with_affected_conditions.size() == 0)
            pushdown_unlock.insert(v->body);
          
          emit_lock_decls (vut);
        }

      // initialize frame pointer
      o->newline() << "struct " << v->name() << "_locals * __restrict__ l = "
                   << "& c->probe_locals." << v->name() << ";";
      o->newline() << "(void) l;"; // make sure "l" is marked used

      // Emit runtime safety net for unprivileged mode.
      // NB: In usermode, the system restricts our privilege for us.
      if (!session->runtime_usermode_p())
        v->emit_privilege_assertion (o);

      // emit probe local initialization block

      v->emit_probe_local_init(*this->session, o);

      // PR26296: not so early!
#if 0
      // emit all read/write locks for global variables
      if (v->needs_global_locks ())
        emit_lock ();
#endif
      
      // initialize locals
      for (unsigned j=0; j<v->locals.size(); j++)
        {
	  if (v->locals[j]->synthetic)
            continue;
	  if (v->locals[j]->index_types.size() > 0) // array?
            throw SEMANTIC_ERROR (_("array locals not supported, missing global declaration?"),
                                  v->locals[j]->tok);
	  else if (v->locals[j]->type == pe_long)
	    o->newline() << "l->" << c_localname (v->locals[j]->name)
			 << " = 0;";
	  else if (v->locals[j]->type == pe_string)
	    o->newline() << "l->" << c_localname (v->locals[j]->name)
			 << "[0] = '\\0';";
	  else
	    throw SEMANTIC_ERROR (_("unsupported local variable type"),
				  v->locals[j]->tok);
        }

      v->initialize_probe_context_vars (o);

      max_action_info mai (*session);
      v->body->visit (&mai);
      if (session->verbose > 1)
        clog << _F("%d statements for probe %s", mai.statement_count,
		   v->name().c_str()) << endl;

      if (mai.statement_count_finite() && !session->suppress_time_limits
          && !session->unoptimized) // this is a finite-statement-count probe
        {
          o->newline() << "if (c->actionremaining < " << mai.statement_count 
                       << ") { c->last_error = " << STAP_T_04 << " goto out; }";
          this->already_checked_action_count = true;
        }

      v->body->visit (this);

      record_actions(0, v->body->tok, true);

      o->newline(-1) << "deref_fault: __attribute__((unused));";
      o->newline(0) << "out:";
      // NB: no need to uninitialize locals, except if arrays/stats can
      // someday be local

      o->indent(1);

      if (!v->probes_with_affected_conditions.empty())
        {
          // PR26296
          // emit all read/write locks for global variables ... if somehow still not done by now
          // emit a local out: label, for error catching in these condition exprs
          o->newline() << "{";
          o->newline(1) << "__label__ out, deref_fault;";
          if (v->needs_global_locks ())
            emit_lock ();

          for (set<derived_probe*>::const_iterator
                 it  = v->probes_with_affected_conditions.begin();
               it != v->probes_with_affected_conditions.end(); ++it)
            {
              emit_probe_condition_update(*it);
            }
          o->newline(-1) << "deref_fault: __attribute__((unused));";
          o->newline() << "out: __attribute__((unused));";
          o->newline() << "}";
        }

      // PR26296
      // Emit an unlock at the end, even if it was pushed down into some
      // probe handler statement.  (It'll be conditional on c->locked
      // anyway.)
      if (v->needs_global_locks ())
	emit_unlock ();

      // XXX: do this flush only if the body included a
      // print/printf/etc. routine!
      o->newline() << "_stp_print_flush();";
      o->newline(-1) << "}\n";
    }

  this->current_probe = 0;
  this->already_checked_action_count = false;
}

// Updates the cond_enabled field and sets need_module_refresh if it was
// changed.
void
c_unparser::emit_probe_condition_update(derived_probe* v)
{
  unsigned i = v->session_index;
  assert(i < session->probes.size());

  expression *cond = v->sole_location()->condition;
  assert(cond);

  // NB: the caller guarantees that global variables are already locked
  // (if necessary) by this point.  It's wrong to judge necessity by
  // v->needs_global_locks(), because that's the wrong v (the OTHER probe
  // that is conditional on some global, not THIS probe that modifies the
  // global, and thus recomputes the conditions).
  
  string cond_enabled = "stap_probes[" + lex_cast(i) + "].cond_enabled";

  // Concurrency note: we're safe modifying cond_enabled here since we emit
  // locks not only for globals we write to, but also for globals read in other
  // probes' whose conditions we visit below (see in c_unparser::emit_probe). So
  // we can be assured we're the only ones modifying cond_enabled.

  o->newline() << "if (" << cond_enabled << " != ";
  o->line() << "!!"; // NB: turn general integer into boolean 1 or 0
  v->sole_location()->condition->visit(this);
  o->line() << ") {";
  o->newline(1) << cond_enabled << " ^= 1;"; // toggle it 

  // don't bother refreshing if on-the-fly not supported
  if (!session->runtime_usermode_p()
      && v->group && v->group->otf_supported(*session))
    o->newline() << "atomic_set(&need_module_refresh, 1);";

  o->newline(-1) << "}";
}

void
c_unparser::emit_lock_decls(const varuse_collecting_visitor& vut)
{
  unsigned numvars = 0;

  if (session->verbose > 1)
    clog << "probe " << current_probe->session_index << " "
            "('" << *current_probe->sole_location() << "') locks";

  // We can only make this static in kernel mode.  In stapdyn mode,
  // the globals and their locks are in shared memory.
  o->newline();
  if (!session->runtime_usermode_p())
    o->line() << "static ";
  o->line() << "const struct stp_probe_lock locks[] = {";
  o->indent(1);

  for (unsigned i = 0; i < session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      bool read_p = vut.read.count(v) > 0;
      bool write_p = vut.written.count(v) > 0;
      if (!read_p && !write_p) continue;

      bool written_p;
      if (v->type == pe_stats) // read and write locks are flipped
        // Specifically, a "<<<" to a stats object is considered a
        // "shared-lock" operation, since it's implicitly done
        // per-cpu.  But a "@op(x)" extraction is an "exclusive-lock"
        // one, as is a (sorted or unsorted) foreach, so those cases
        // are excluded by the w & !r condition below.
        {
          if (write_p && !read_p) { read_p = true; write_p = false; }
          else if (read_p && !write_p) { read_p = false; write_p = true; }
          written_p = vcv_needs_global_locks.read.count(v) > 0;
        }
      else
        written_p = vcv_needs_global_locks.written.count(v) > 0;

      // We don't need to read lock "read-mostly" global variables.  A
      // "read-mostly" global variable is only written to within
      // probes that don't need global variable locking (such as
      // begin/end probes).  If vcv_needs_global_locks doesn't mark
      // the global as written to, then we don't have to lock it
      // here to read it safely.
      if (!written_p && read_p && !write_p)
        continue;

      o->newline() << "{";
      o->newline(1) << ".lock = global_lock(" + c_globalname(v->name) + "),";
      o->newline() << ".write_p = " << (write_p ? 1 : 0) << ",";
      o->newline() << "#ifdef STP_TIMING";
      o->newline() << ".skipped = global_skipped(" << c_globalname (v->name) << "),";
      o->newline() << ".contention = global_contended(" << c_globalname (v->name) << "),";
      o->newline() << "#endif";
      o->newline(-1) << "},";

      numvars ++;
      if (session->verbose > 1)
        clog << " " << v->name << "[" << (read_p ? "r" : "")
             << (write_p ? "w" : "")  << "]";
    }

  o->newline(-1) << "};";

  if (session->verbose > 1)
    {
      if (!numvars)
        clog << _(" nothing");
      clog << endl;
    }
}


// PR26296: emit locking ops just before statements that involve
// reads/writes to script globals.

void
c_unparser::emit_lock()
{
  if (this->session->verbose > 3)
    clog << "emit lock" << endl;
  
  // Emit code to lock, if we haven't already done it during this
  // probe handler run.
  o->newline() << "if (c->locked == 0) {";
  o->newline(1) << "if (!stp_lock_probe(locks, ARRAY_SIZE(locks)))";
  o->newline(1) << "goto out;"; // bypass try/catch etc.
  o->newline(-1) << "else";
  o->newline(1) << "c->locked = 1;";
  o->newline(-2) << "} else if (unlikely(c->locked == 2)) {";
  o->newline(1) << "_stp_error(\"invalid lock state\");";
  o->newline(-1) << "}";
}
    

// The given statement was found to have no lockworthy constituents.
// But if given statement was still listed for pushdown, then it was 
// by logic error, so kvetch and emit a token lock and/or unlock.
// Eventually this could become an assertion error.
void
c_unparser::locks_not_needed_argh (statement *p)
{
  if (!pushdown_lock_p(p) && !pushdown_unlock_p(p))
    return; // no problem then!
      
  if (this->session->verbose > 2)
    clog << "Oops, unexpected"
         << (pushdown_lock_p(p) ? " lock" : "")
         << (pushdown_unlock_p(p) ? " unlock" : "")
         << " pushdown for statement " << *p->tok << endl;

  if (pushdown_lock_p(p))
    emit_lock();
  if (pushdown_unlock_p(p))
    emit_unlock();
}


// Check whether this statement reads or writes any globals.
// Those that do not, can allow lock or unlock operations to
// slide forward or backward over them (respectively).
bool
c_unparser::locks_needed_p(visitable *s) // statement OR expression
{
  if (! current_probe) // called from function context?
    return false;

  if (! current_probe->needs_global_locks ())
    return false;

  // NB: In compatible mode, return TRUE all the time, so that
  // locks/unlocks are emitted early/late always.
  if (strverscmp(this->session->compatible.c_str(), "4.3") <= 0)
    return true;
  
  varuse_collecting_visitor vut(*session);
  s->visit (& vut);
  bool lock_me = false;
  for (unsigned i = 0; i < session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      bool read_p = vut.read.count(v) > 0;
      bool write_p = vut.written.count(v) > 0;
      lock_me = read_p || write_p;
      if (lock_me) break; // first hit is enough
    }

  return lock_me;
}


void
c_unparser::emit_unlock()
{
  if (this->session->verbose > 3)
    clog << "emit unlock" << endl;
  
  o->newline() << "if (c->locked == 1) {";
  o->newline(1) << "stp_unlock_probe(locks, ARRAY_SIZE(locks));";
  o->newline() << "c->locked = 2;"; // NB: 2 so it won't re-lock
  o->newline(-1) << "}";
}


void
c_unparser::collect_map_index_types(vector<vardecl *> const & vars,
				    set< pair<vector<exp_type>, exp_type> > & types)
{
  for (unsigned i = 0; i < vars.size(); ++i)
    {
      vardecl *v = vars[i];
      if (v->arity > 0)
	{
	  types.insert(make_pair(v->index_types, v->type));
	}
    }
}

string
mapvar::value_typename(exp_type e)
{
  switch (e)
    {
    case pe_long:
      return "INT64";
    case pe_string:
      return "STRING";
    case pe_stats:
      return "STAT";
    default:
      throw SEMANTIC_ERROR(_("array type is neither string nor long"));
    }
}

string
mapvar::key_typename(exp_type e)
{
  switch (e)
    {
    case pe_long:
      return "INT64";
    case pe_string:
      return "STRING";
    default:
      throw SEMANTIC_ERROR(_("array key is neither string nor long"));
    }
}

string
mapvar::shortname(exp_type e)
{
  switch (e)
    {
    case pe_long:
      return "i";
    case pe_string:
      return "s";
    default:
      throw SEMANTIC_ERROR(_("array type is neither string nor long"));
    }
}

string
c_unparser::map_keytypes(vardecl* v)
{
  string result;
  vector<exp_type> types = v->index_types;
  types.push_back (v->type);
  for (unsigned i = 0; i < types.size(); ++i)
    {
      switch (types[i])
        {
        case pe_long:
          result += 'i';
          break;
        case pe_string:
          result += 's';
          break;
        case pe_stats:
          result += 'x';
          break;
        default:
          throw SEMANTIC_ERROR(_("unknown type of map"));
          break;
        }
    }
  return result;
}

void
c_unparser::emit_map_type_instantiations ()
{
  set< pair<vector<exp_type>, exp_type> > types;

  collect_map_index_types(session->globals, types);

  for (unsigned i = 0; i < session->probes.size(); ++i)
    collect_map_index_types(session->probes[i]->locals, types);

  for (map<string,functiondecl*>::iterator it = session->functions.begin(); it != session->functions.end(); it++)
    collect_map_index_types(it->second->locals, types);

  if (!types.empty())
    o->newline() << "#include \"alloc.c\"";

  for (set< pair<vector<exp_type>, exp_type> >::const_iterator i = types.begin();
       i != types.end(); ++i)
    {
      o->newline() << "#define VALUE_TYPE " << mapvar::value_typename(i->second);
      for (unsigned j = 0; j < i->first.size(); ++j)
	{
	  string ktype = mapvar::key_typename(i->first.at(j));
	  o->newline() << "#define KEY" << (j+1) << "_TYPE " << ktype;
	}
      /* For statistics, flag map-gen to pull in nested pmap-gen too.  */
      if (i->second == pe_stats)
	o->newline() << "#define MAP_DO_PMAP 1";
      o->newline() << "#include \"map-gen.c\"";
      o->newline() << "#undef MAP_DO_PMAP";
      o->newline() << "#undef VALUE_TYPE";
      for (unsigned j = 0; j < i->first.size(); ++j)
	{
	  o->newline() << "#undef KEY" << (j+1) << "_TYPE";
	}
    }

  if (!types.empty())
    o->newline() << "#include \"map.c\"";

};


string
c_unparser::c_typename (exp_type e)
{
  switch (e)
    {
    case pe_long: return string("int64_t");
    case pe_string: return string("string_t");
    case pe_stats: return string("Stat");
    case pe_unknown:
    default:
      throw SEMANTIC_ERROR (_("cannot expand unknown type"));
    }
}


// XXX: the below is just for the sake of example; it's possibly
// better to expose the hash function in hash.cxx

// unsigned int
// do_hash (const char *e)
// {
//   unsigned int foo = 0;
//   while (*e) {
//     foo *= 101; foo += *e; e++;
//   }
//   return foo;
// }


string
c_unparser::c_localname (const string& e, bool mangle_oldstyle)
{
  if (strverscmp(session->compatible.c_str(), "1.8") < 0 || mangle_oldstyle)
    return e; // old mangling behaviour
  else
// XXX: we may wish to invent and/or test other mangling schemes, e.g.:
//  return "l_" + e + "_" + lex_cast(do_hash(e.c_str()));
    return "l_" + e;
}


string
c_unparser::c_globalname (const string& e)
{
  // XXX uncomment to test custom mangling:
  // return "s_" + e + "_" + lex_cast(do_hash(e.c_str()));
  return "s_" + e;
}


string
c_unparser::c_funcname (const string& e, bool& funcname_shortened)
{
  const string function_prefix = "function_";
  // This matches MAX_NAME_LEN in linux objtool/elf.c used by kbuild
  // The kernel objtool used by kbuild has a hardcoded function length limit
  const unsigned max_name_len = 128;
  // Add padding to allow for gcc function attribute suffixes like constprop or cold
  const unsigned func_attr_suffix_padding = 32;
  // XXX uncomment to test custom mangling:
  // return function_prefix + e + "_" + lex_cast(do_hash(e.c_str()));

  if (e.length() > max_name_len - function_prefix.length() - func_attr_suffix_padding)
    {
      long function_index = 0;
      for (map<string,functiondecl*>::iterator it = session->functions.begin();
          it != session->functions.end(); it++)
        {
          if (it->first == e)
            {
              funcname_shortened = true;
              return function_prefix + lex_cast (function_index);
            }
          function_index += 1;
        }
        throw SEMANTIC_ERROR (_("unresolved symbol: ") + e); // should not happen
    }
  else
    {
      funcname_shortened = false;
      return function_prefix + e;
    }
}


string
c_unparser::c_funcname (const string& e)
{
  bool funcname_shortened;
  return c_funcname (e, funcname_shortened);
}


string
c_unparser::c_arg_define (const string& e)
{
  return "#define STAP_ARG_" + e + " THIS->" + c_localname(e);
}


string
c_unparser::c_arg_undef (const string& e)
{
  return "#undef STAP_ARG_" + e;
}

void
c_unparser::c_global_write_def(vardecl* v)
{
  if (v->arity > 0)
    {
      o->newline() << "#define STAP_GLOBAL_SET_" << v->unmangled_name << "(...) "
                   << "({int rc = _stp_map_set_" << map_keytypes(v)
                   << "(global(" << c_globalname(v->name) << "), __VA_ARGS__); "
                   << "if (unlikely(rc)) { c->last_error = " << STAP_T_01
                   << lex_cast(v->maxsize > 0 ? "size limit (" + lex_cast(v->maxsize)
                      + ")" : "MAXMAPENTRIES") + "\"; goto out; } rc;})";
    }
  else
    {
      o->newline() << "#define STAP_GLOBAL_SET_" << v->unmangled_name << "(val) ";
      if (v->type == pe_string)
          o->line() << "strlcpy(global(" << c_globalname(v->name) << "), val, MAXSTRINGLEN)";
      else if (v->type == pe_long)
          o->line() << "global_set(" << c_globalname(v->name) << ", val)";
    }
}

void
c_unparser::c_global_read_def(vardecl* v)
{
  if (v->arity > 0)
    {
      o->newline() << "#define STAP_GLOBAL_GET_" << v->unmangled_name << "(...) "
                   << "_stp_map_get_" << map_keytypes(v)
                   << "(global(" << c_globalname(v->name) << "), __VA_ARGS__)";
    }
  else
    {
      o->newline() << "#define STAP_GLOBAL_GET_" << v->unmangled_name << "() "
                   << "global(" << c_globalname(v->name) << ")";
    }
}

void
c_unparser::c_global_write_undef(vardecl* v)
{
  o->newline() << "#undef STAP_GLOBAL_SET_" << v->unmangled_name;
}

void
c_unparser::c_global_read_undef(vardecl* v)
{
  o->newline() << "#undef STAP_GLOBAL_GET_" << v->unmangled_name;
}

void
c_unparser::c_assign (var& lvalue, const string& rvalue, const token *tok)
{
  switch (lvalue.type())
    {
    case pe_string:
      c_strcpy(lvalue.value(), rvalue);
      break;
    case pe_long:
      o->newline() << lvalue << " = " << rvalue << ";";
      break;
    default:
      throw SEMANTIC_ERROR (_("unknown lvalue type in assignment"), tok);
    }
}


void
c_unparser::c_assign(tmpvar& t, expression *e, const char* msg)
{
  // We don't really need a tmpvar if the expression is a literal.
  // (NB: determined by the expression itself, not tok->type!)

  if (dynamic_cast<literal*>(e))
    {
      // We need to use the visitors to get proper C values, like
      // "((int64_t)5LL)" for numbers and escaped characters in strings.

      // Create a fake output stream so we can grab the string output.
      ostringstream oss;
      translator_output tmp_o(oss);

      // Temporarily swap out the real translator_output stream with our
      // fake one.
      translator_output *saved_o = o;
      o = &tmp_o;

      // Visit the expression then restore the original output stream
      e->visit (this);
      o = saved_o;

      // All instances of this tmpvar will use the literal value.
      t.override (oss.str());
    }
  else
    c_assign (t.value(), e, msg);
}

struct expression_is_functioncall : public nop_visitor
{
  functioncall* fncall;
  expression_is_functioncall ()
    : fncall(NULL) {}

  void visit_functioncall (functioncall* e)
    {
      fncall = e;
    }
};

void
c_unparser::c_assign (const string& lvalue, expression* rvalue,
		      const char* msg)
{
  if (rvalue->type == pe_long)
    {
      o->newline() << lvalue << " = ";
      rvalue->visit (this);
      o->line() << ";";
    }
  else if (rvalue->type == pe_string)
    {
      expression_is_functioncall eif;
      rvalue->visit(& eif);
      if (!session->unoptimized && eif.fncall)
        {
	  const functioncall* saved_fncall = assigned_functioncall;
	  const string* saved_retval = assigned_functioncall_retval;

          // let the functioncall know that the return value is being saved/used
          // and keep track of the lvalue, so that the retval assignment can
          // happen in ::visit_functioncall, to avoid complications with nesting.
	  assigned_functioncall = eif.fncall;
	  assigned_functioncall_retval = &lvalue;
          eif.fncall->visit (this);
          o->line() << ";";

	  assigned_functioncall = saved_fncall;
	  assigned_functioncall_retval = saved_retval;
        }
      else
        {
          // will call rvalue->visit()
          c_strcpy (lvalue, rvalue);
        }
    }
  else
    {
      string fullmsg = string(msg) + _(" type unsupported");
      throw SEMANTIC_ERROR (fullmsg, rvalue->tok);
    }
}


void
c_unparser::c_assign (const string& lvalue, const string& rvalue,
		      exp_type type, const char* msg, const token* tok)
{
  if (type == pe_long)
    {
      o->newline() << lvalue << " = " << rvalue << ";";
    }
  else if (type == pe_string)
    {
      c_strcpy (lvalue, rvalue);
    }
  else
    {
      string fullmsg = string(msg) + _(" type unsupported");
      throw SEMANTIC_ERROR (fullmsg, tok);
    }
}


void
c_unparser_assignment::c_assignop(tmpvar & res,
				  var const & lval,
				  tmpvar const & rval,
				  token const * tok)
{
  // This is common code used by scalar and array-element assignments.
  // It assumes an operator-and-assignment (defined by the 'pre' and
  // 'op' fields of c_unparser_assignment) is taking place between the
  // following set of variables:
  //
  // res: the result of evaluating the expression, a temporary
  // lval: the lvalue of the expression, which may be damaged
  // rval: the rvalue of the expression, which is a temporary or constant

  // we'd like to work with a local tmpvar so we can overwrite it in
  // some optimized cases

  translator_output* o = parent->o;

  if (res.type() == pe_string)
    {
      if (post)
	throw SEMANTIC_ERROR (_("post assignment on strings not supported"),
			      tok);
      if (op == "=")
	{
	  parent->c_strcpy (lval.value(), rval.value());
	  // no need for second copy
	  res = rval;
        }
      else if (op == ".=")
	{
	  parent->c_strcat (lval.value(), rval.value());
	  res = lval;
	}
      else
        throw SEMANTIC_ERROR (_F("string assignment operator %s unsupported",
				 op.to_string().c_str()), tok);
    }
  else if (op == "<<<")
    {
      int stat_op_count = lval.sdecl().stat_ops & (STAT_OP_COUNT|STAT_OP_AVG|STAT_OP_VARIANCE);
      int stat_op_sum = lval.sdecl().stat_ops & (STAT_OP_SUM|STAT_OP_AVG|STAT_OP_VARIANCE);
      int stat_op_min = lval.sdecl().stat_ops & STAT_OP_MIN;
      int stat_op_max = lval.sdecl().stat_ops & STAT_OP_MAX;
      int stat_op_variance = lval.sdecl().stat_ops & STAT_OP_VARIANCE;

      assert(lval.type() == pe_stats);
      assert(rval.type() == pe_long);
      assert(res.type() == pe_long);

      o->newline() << "_stp_stat_add (" << lval << ", " << rval << ", " <<
                      stat_op_count << ", " <<  stat_op_sum << ", " <<
                      stat_op_min << ", " << stat_op_max << ", " <<
                      stat_op_variance << ");";
      res = rval;
    }
  else if (res.type() == pe_long)
    {
      // a lot of operators come through this "gate":
      // - vanilla assignment "="
      // - stats aggregation "<<<"
      // - modify-accumulate "+=" and many friends
      // - pre/post-crement "++"/"--"
      // - "/" and "%" operators, but these need special handling in kernel

      // compute the modify portion of a modify-accumulate
      string macop;
      unsigned oplen = op.size();
      if (op == "=")
	macop = "*error*"; // special shortcuts below
      else if (op == "++" || op == "+=")
	macop = "+=";
      else if (op == "--" || op == "-=")
	macop = "-=";
      else if (oplen > 1 && op[oplen-1] == '=') // for *=, <<=, etc...
	macop = op;
      else
	// internal error
	throw SEMANTIC_ERROR (_("unknown macop for assignment"), tok);

      if (post)
	{
          if (macop == "/" || macop == "%" || op == "=")
            throw SEMANTIC_ERROR (_("invalid post-mode operator"), tok);

	  o->newline() << res << " = " << lval << ";";

	  if (macop == "+=" || macop == "-=")
	    o->newline() << lval << " " << macop << " " << rval << ";";
	  else
	    o->newline() << lval << " = " << res << " " << macop << " " << rval << ";";
	}
      else
	{
          if (op == "=") // shortcut simple assignment
	    {
	      o->newline() << lval << " = " << rval << ";";
	      res = rval;
	    }
	  else
	    {
	      if (macop == "/=" || macop == "%=")
		{
		  o->newline() << "if (unlikely(!" << rval << ")) {";
		  o->newline(1) << "c->last_error = ";
                  o->line() << STAP_T_03;
		  o->newline() << "c->last_stmt = " << lex_cast_qstring(*rvalue->tok) << ";";
		  o->newline() << "goto out;";
		  o->newline(-1) << "}";
		  o->newline() << lval << " = "
			       << ((macop == "/=") ? "_stp_div64" : "_stp_mod64")
			       << " (NULL, " << lval << ", " << rval << ");";
		}
	      else
		o->newline() << lval << " " << macop << " " << rval << ";";
	      res = lval;
	    }
	}
    }
    else
      throw SEMANTIC_ERROR (_("assignment type not yet implemented"), tok);
}


void
c_unparser::c_declare(exp_type ty, const string &ident)
{
  o->newline() << c_typename (ty) << " " << ident << ";";
}


void
c_unparser::c_declare_static(exp_type ty, const string &ident)
{
  o->newline() << "static " << c_typename (ty) << " " << ident << ";";
}


void
c_unparser::c_strcpy (const string& lvalue, const string& rvalue)
{
  o->newline() << "strlcpy ("
		   << lvalue << ", "
		   << rvalue << ", MAXSTRINGLEN);";
}


void
c_unparser::c_strcpy (const string& lvalue, expression* rvalue)
{
  o->newline() << "strlcpy (" << lvalue << ", ";
  rvalue->visit (this);
  o->line() << ", MAXSTRINGLEN);";
}


void
c_unparser::c_strcat (const string& lvalue, const string& rvalue)
{
  o->newline() << "strlcat ("
	       << lvalue << ", "
	       << rvalue << ", MAXSTRINGLEN);";
}


void
c_unparser::c_strcat (const string& lvalue, expression* rvalue)
{
  o->newline() << "strlcat (" << lvalue << ", ";
  rvalue->visit (this);
  o->line() << ", MAXSTRINGLEN);";
}


bool
c_unparser::is_local(vardecl const *r, token const *tok)
{
  if (current_probe)
    {
      for (unsigned i=0; i<current_probe->locals.size(); i++)
	{
	  if (current_probe->locals[i] == r)
	    return true;
	}
    }
  else if (current_function)
    {
      for (unsigned i=0; i<current_function->locals.size(); i++)
	{
	  if (current_function->locals[i] == r)
	    return true;
	}

      for (unsigned i=0; i<current_function->formal_args.size(); i++)
	{
	  if (current_function->formal_args[i] == r)
	    return true;
	}
    }

  for (unsigned i=0; i<session->globals.size(); i++)
    {
      if (session->globals[i] == r)
	return false;
    }

  if (tok)
    throw SEMANTIC_ERROR (_("unresolved symbol"), tok);
  else
    throw SEMANTIC_ERROR (_("unresolved symbol: ") + (string)r->name);
}


tmpvar
c_unparser::gensym(exp_type ty)
{
  return tmpvar (this, ty, tmpvar_counter);
}

aggvar
c_unparser::gensym_aggregate()
{
  return aggvar (this, tmpvar_counter);
}


var
c_unparser::getvar(vardecl *v, token const *tok)
{
  bool loc = is_local (v, tok);
  if (loc)
    return var (this, loc, v->type, v->name);
  else
    {
      statistic_decl sd;
      std::map<interned_string, statistic_decl>::const_iterator i;
      i = session->stat_decls.find(v->name);
      if (i != session->stat_decls.end())
	sd = i->second;
      return var (this, loc, v->type, sd, v->name);
    }
}


mapvar
c_unparser::getmap(vardecl *v, token const *tok)
{
  if (v->arity < 1)
    throw SEMANTIC_ERROR(_("attempt to use scalar where map expected"), tok);
  statistic_decl sd;
  std::map<interned_string, statistic_decl>::const_iterator i;
  i = session->stat_decls.find(v->name);
  if (i != session->stat_decls.end())
    sd = i->second;
  return mapvar (this, is_local (v, tok), v->type, sd,
      v->name, v->index_types, v->maxsize, v->wrap);
}


itervar
c_unparser::getiter(symbol *s)
{
  return itervar (this, s, tmpvar_counter);
}


// Queue up some actions to remove from actionremaining.  Set update=true at
// the end of basic blocks to actually update actionremaining and check it
// against MAXACTION.
void
c_unparser::record_actions (unsigned actions, const token* tok, bool update)
{
  action_counter += actions;

  // Update if needed, or after queueing up a few actions, in case of very
  // large code sequences.
  if (((update && action_counter > 0) || action_counter >= 10/*<-arbitrary*/)
    && !session->suppress_time_limits && !already_checked_action_count)
    {

      o->newline() << "c->actionremaining -= " << action_counter << ";";
      o->newline() << "if (unlikely (c->actionremaining <= 0)) {";
      o->newline(1) << "c->last_error = ";
      o->line() << STAP_T_04;

      // XXX it really ought to be illegal for anything to be missing a token,
      // but until we're sure of that, we need to defend against NULL.
      if (tok)
        o->newline() << "c->last_stmt = " << lex_cast_qstring(*tok) << ";";

      o->newline() << "goto out;";
      o->newline(-1) << "}";
      action_counter = 0;
    }
}


void
c_unparser::visit_block (block *s)
{
  // Key insight: individual statements of a block can reuse
  // temporary variable slots, since temporaries don't survive
  // statement boundaries.  So we use gcc's anonymous union/struct
  // facility to explicitly overlay the temporaries.
  start_compound_statement ("block_statement", s);

  o->newline() << "{";
  o->indent (1);

  // PR26296 Designate the statements in the block for locking and unlocking
  // by whether they are the first (or last) to refer to globals.  Don't emit
  // locking operations here at all: force them to do so via the pushdown_* set,
  // except if there are no locks_needed_p statements at all in our body.
  if (pushdown_lock_p(s) ||
      pushdown_unlock_p(s))
    {
      bool pushed_lock_down = false;
 
      // if needed, find the lock insertion site; instruct it to lock
      if (pushdown_lock_p(s))
        {
          for (unsigned i=0; i<s->statements.size(); i++)
            {
              struct statement *stmt = s->statements[i];
              if (locks_needed_p (stmt))
                {
                  pushed_lock_down = true;
                  pushdown_lock.insert (stmt);

                  if (! stmt->might_pushdown_lock ())
                    {
                      // now we know the subsquement stmts must have locks
                      // held, so we don't bother going forward.
                      break;
                    }
                }
            }
        }

      // if needed, find the unlock insertion site; instruct it to unlock
      if (pushdown_unlock_p(s))
        for (ssize_t i=s->statements.size()-1; i>=0; i--) // NB: traverse backward!
          if (locks_needed_p (s->statements[i]))
            { pushdown_unlock.insert(s->statements[i]); pushed_lock_down = true; break; }

      if (! pushed_lock_down)
        {
          // NB: pushed_lock_down will remain false if no statement in this block requires global
          // locks at all.  Ideally, this shouldn't happen, since our parent staptree* shouldn't
          // have entered us into push_*lock_down[].  Us being in both push_lock_down[] AND
          // push_unlock_down[] in this case is especially goofy.  Nevertheless, let's play
          // along and emit a dummy lock and/or unlock at the top.
          locks_not_needed_argh (s);
        }
    }
    
  for (unsigned i=0; i<s->statements.size(); i++)
    {
      try
        {
          wrap_compound_visit (s->statements[i]); // incl. lock/unlock as appropriate
	  o->newline();
        }
      catch (const semantic_error& e)
        {
          session->print_error (e);
        }
    }
  o->newline(-1) << "}";

  close_compound_statement ("block_statement", s);
}


void c_unparser::visit_try_block (try_block *s)
{
  record_actions(0, s->tok, true); // flush prior actions

  start_compound_statement ("try_block", s);

  // PR26296: for try/catch, don't try to push lock/unlock down
  if (pushdown_lock_p(s))
    emit_lock();
  
  o->newline() << "{";
  o->newline(1) << "__label__ normal_fallthrough;";
  o->newline(1) << "{";
  o->newline() << "__label__ deref_fault;";
  o->newline() << "__label__ out;";

  assert (!session->unoptimized || s->try_block); // dead_stmtexpr_remover would zap it
  if (s->try_block)
    {
      wrap_compound_visit (s->try_block);
      record_actions(0, s->try_block->tok, true); // flush accumulated actions
    }
  o->newline() << "goto normal_fallthrough;";

  o->newline() << "deref_fault: __attribute__((unused));";
  o->newline() << "out: __attribute__((unused));";

  // Close the scope of the above nested 'out' label, to make sure
  // that the catch block, should it encounter errors, does not resolve
  // a 'goto out;' to the above label, causing infinite looping.
  o->newline(-1) << "}";

  o->newline() << "if (likely(c->last_error == NULL)) goto out;";

  // NB: MAXACTION errors are not catchable and we should never clear the error
  // message below otherwise the source location in the message would
  // become inaccurate (always being the top-level try/catch statement's).
  if (!session->suppress_time_limits)
    o->newline() << "if (unlikely (c->actionremaining <= 0)) goto out;";
  
  if (s->catch_error_var)
    {
      var cev(getvar(s->catch_error_var->referent, s->catch_error_var->tok));
      c_strcpy (cev.value(), "c->last_error");
    }
  o->newline() << "c->last_error = NULL;";

  // Prevent the catch{} handler from even starting if MAXACTIONS have
  // already been used up.  Add one for the act of catching too.
  record_actions(1, s->tok, true);

  if (s->catch_block)
    {
      wrap_compound_visit (s->catch_block);
      record_actions(0, s->catch_block->tok, true); // flush accumulated actions
    }

  o->newline() << "normal_fallthrough:";
  o->newline() << ";"; // to have _some_ statement
  o->newline(-1) << "}";

  if (pushdown_unlock_p(s))
    emit_unlock();
  
  close_compound_statement ("try_block", s);
}


void
c_unparser::visit_embeddedcode (embeddedcode *s)
{
  // Automatically add a call to assert_is_myproc to any code tagged with
  // /* myproc-unprivileged */
  if (s->tagged_p ("/* myproc-unprivileged */"))
    o->newline() << "assert_is_myproc();";
  o->newline() << "{";

  bool ln = locks_needed_p(s);
  if (!ln)
    locks_not_needed_argh(s);
  
  // PR26296
  if (ln && pushdown_lock_p(s))
    emit_lock();
  
  //  if (1 || s->tagged_p ("CATCH_DEREF_FAULT"))
  //    o->newline() << "__label__ deref_fault;";

  vector<vardecl*> read_defs;
  vector<vardecl*> write_defs;
  for (unsigned i = 0; i < session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->synthetic) continue; /* skip synthetic variables; embedded c can't access them. */
      string name = v->unmangled_name;
      assert (name != "");
      if (s->tagged_p("/* pragma:read:" + name + " */"))
        {
          c_global_read_def(v);
          read_defs.push_back(v);
        }
      if (s->tagged_p("/* pragma:write:" + name + " */"))
        {
          c_global_write_def(v);
          write_defs.push_back(v);
        }
    }

  o->newline(1) << s->code;
  o->indent(-1);

  for (vector<vardecl*>::const_iterator it = read_defs.begin(); it != read_defs.end(); ++it)
    c_global_read_undef(*it);
  for (vector<vardecl*>::const_iterator it = write_defs.begin(); it != write_defs.end(); ++it)
    c_global_write_undef(*it);

  //  if (1 || s->tagged_p ("CATCH_DEREF_FAULT"))
  //    o->newline() << ";";

  if (ln && pushdown_unlock_p(s))
    emit_unlock();
  
  o->newline() << "}";
}


void
c_unparser::visit_null_statement (null_statement *s)
{
  o->newline() << "/* null */;";
  locks_not_needed_argh(s);
}


void
c_unparser::visit_expr_statement (expr_statement *s)
{
  bool ln = locks_needed_p(s);
  
  if (!ln)
    locks_not_needed_argh(s);
  
  if (ln && pushdown_lock_p(s))
    emit_lock();
  
  o->newline() << "(void) ";
  s->value->visit (this);
  o->line() << ";";
  record_actions(1, s->tok);

  if (ln && pushdown_unlock_p(s))
    emit_unlock();
}


void
c_tmpcounter::wrap_compound_visit (statement *s)
{
  if (!s) return;

  std::ostream::pos_type before_struct_pos;
  std::ostream::pos_type after_struct_pos;

  start_struct_def(before_struct_pos, after_struct_pos, s->tok);
  c_unparser::wrap_compound_visit (s);
  close_struct_def(before_struct_pos, after_struct_pos);
}

void
c_tmpcounter::wrap_compound_visit (expression *e)
{
  if (!e) return;

  std::ostream::pos_type before_struct_pos;
  std::ostream::pos_type after_struct_pos;

  start_struct_def(before_struct_pos, after_struct_pos, e->tok);
  c_unparser::wrap_compound_visit (e);
  close_struct_def(before_struct_pos, after_struct_pos);
}

void
c_tmpcounter::start_struct_def (std::ostream::pos_type &before,
                                std::ostream::pos_type &after, const token* tok)
{
  // To avoid lots of empty structs, remember  where we are now.  Then,
  // output the struct start and remember that positon.  If when we get
  // done with the statement we haven't moved, then we don't really need
  // the struct.  To get rid of the struct start we output, we'll seek back
  // to where we were before we output the struct (done in ::close_struct_def).
  translator_output *o = parent->o;
  before = o->tellp();
  o->newline() << "struct { /* source: " << tok->location.file->name
               << ":" << lex_cast(tok->location.line) << " */";
  o->indent(1);
  after = o->tellp();
}

void
c_tmpcounter::close_struct_def (std::ostream::pos_type before,
                                std::ostream::pos_type after)
{
  // meant to be used with ::start_struct_def. remove the struct if empty.
  translator_output *o = parent->o;
  o->indent(-1);
  if (after == o->tellp())
    o->seekp(before);
  else
    o->newline() << "};";
}

void
c_tmpcounter::start_compound_statement (const char* tag, statement *s)
{
  const source_loc& loc = s->tok->location;
  translator_output *o = parent->o;
  o->newline() << "union { /* " << tag << ": "
               << loc.file->name << ":"
               << lex_cast(loc.line) << " */";
  o->indent(1);
}

void
c_tmpcounter::close_compound_statement (const char*, statement *)
{
  translator_output *o = parent->o;
  o->newline(-1) << "};";
}


void
c_unparser::visit_if_statement (if_statement *s)
{
  record_actions(1, s->tok, true);

  start_compound_statement ("if_statement", s);

  bool condition_nl = locks_needed_p (s->condition);
  bool thenblock_nl = locks_needed_p (s->thenblock);
  bool elseblock_nl = s->elseblock ? locks_needed_p (s->elseblock) : false;
  
  if (!condition_nl && !thenblock_nl && !elseblock_nl)
    locks_not_needed_argh(s);

  if (condition_nl && pushdown_lock_p(s))
    emit_lock(); // and then thenblock/elseblock don't need to lock or pushdown!
  
  o->newline() << "if (";
  o->indent (1);

  wrap_compound_visit (s->condition);
  o->indent (-1);
  o->line() << ")";
  
  o->line() << "{";
  o->indent (1);
  
  if (condition_nl && !thenblock_nl && pushdown_unlock_p(s))
    emit_unlock();
  
  if (!condition_nl && thenblock_nl && pushdown_lock_p(s))
    pushdown_lock.insert(s->thenblock);

  if (thenblock_nl && pushdown_unlock_p(s))
    pushdown_unlock.insert(s->thenblock);
  
  wrap_compound_visit (s->thenblock);
  record_actions(0, s->thenblock->tok, true);

  if (!condition_nl && !thenblock_nl && elseblock_nl && pushdown_lock_p(s))
    emit_lock(); // reluctantly

  o->newline(-1) << "}";
  
  if (s->elseblock)
    {
      o->newline() << "else {";
      o->indent (1);

      if (condition_nl && !elseblock_nl && pushdown_unlock_p(s))
        emit_unlock();

      if (!condition_nl && elseblock_nl && pushdown_lock_p(s))
        pushdown_lock.insert(s->elseblock);
      
      if (elseblock_nl && pushdown_unlock_p(s))
        pushdown_unlock.insert(s->elseblock);

      wrap_compound_visit (s->elseblock);
      record_actions(0, s->elseblock->tok, true);

      if (!condition_nl && thenblock_nl && !elseblock_nl && pushdown_lock_p(s))
        emit_lock(); // reluctantly
      
      o->newline(-1) << "}";
    }

  close_compound_statement ("if_statement", s);
}


void
c_unparser::visit_for_loop (for_loop *s)
{
  string ctr = lex_cast (label_counter++);
  string toplabel = "top_" + ctr;
  string contlabel = "continue_" + ctr;
  string breaklabel = "break_" + ctr;

  // PR26269 lockpushdown:
  // for loops, forget optimizing, just emit locks at top & bottom
  if (pushdown_lock_p(s))
    emit_lock();
  
  start_compound_statement ("for_loop", s);

  // initialization
  wrap_compound_visit (s->init);
  record_actions(1, s->tok, true);

  // condition
  o->newline(-1) << toplabel << ":";

  // Emit an explicit action here to cover the act of iteration.
  // Equivalently, it can stand for the evaluation of the condition
  // expression.
  o->indent(1);
  record_actions(1, s->tok);

  o->newline() << "if (! (";
  if (s->cond->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric type"), s->cond->tok);
  wrap_compound_visit (s->cond);
  o->line() << ")) goto " << breaklabel << ";";

  // body
  loop_break_labels.push_back (breaklabel);
  loop_continue_labels.push_back (contlabel);
  wrap_compound_visit (s->block);
  record_actions(0, s->block->tok, true);
  loop_break_labels.pop_back ();
  loop_continue_labels.pop_back ();

  // iteration
  o->newline(-1) << contlabel << ":";
  o->indent(1);
  wrap_compound_visit (s->incr);
  o->newline() << "goto " << toplabel << ";";

  // exit
  o->newline(-1) << breaklabel << ":";
  o->newline(1) << "; /* dummy statement */";

  if (pushdown_unlock_p(s))
    emit_unlock();
  
  close_compound_statement ("for_loop", s);
}


struct arrayindex_downcaster
  : public traversing_visitor
{
  arrayindex *& arr;

  arrayindex_downcaster (arrayindex *& arr)
    : arr(arr)
  {}

  void visit_arrayindex (arrayindex* e)
  {
    arr = e;
  }
};


static bool
expression_is_arrayindex (expression *e,
			  arrayindex *& hist)
{
  arrayindex *h = NULL;
  arrayindex_downcaster d(h);
  e->visit (&d);
  if (static_cast<void*>(h) == static_cast<void*>(e))
    {
      hist = h;
      return true;
    }
  return false;
}


// Look for opportunities to used a saved value at the beginning of the loop
void
c_unparser::visit_foreach_loop_value (foreach_loop* s, const string& value)
{
  bool stable_value = false;

  // There are three possible cases that we might easily retrieve the value:
  //   1. foreach ([keys] in any_array_type)
  //   2. foreach (idx in @hist_*(stat))
  //   3. foreach (idx in @hist_*(stat[keys]))
  //
  // For 1 and 2, we just need to check that the keys/idx are const throughout
  // the loop.  For 3, we'd have to check also that the arbitrary keys
  // expressions indexing the stat are const -- much harder, so I'm punting
  // that case for now.

  symbol *array;
  hist_op *hist;
  classify_indexable (s->base, array, hist);

  if (!(hist && get_symbol_within_expression(hist->stat)->referent->arity > 0))
    {
      set<vardecl*> indexes;
      for (unsigned i=0; i < s->indexes.size(); ++i)
        indexes.insert(s->indexes[i]->referent);

      varuse_collecting_visitor v(*session);
      s->block->visit (&v);
      v.embedded_seen = false; // reset because we only care about the indexes
      if (v.side_effect_free_wrt(indexes))
        stable_value = true;
    }

  if (stable_value)
    {
      // Rather than trying to compare arrayindexes to this foreach_loop
      // manually, we just create a fake arrayindex that would match the
      // foreach_loop, render it as a string, and later render encountered
      // arrayindexes as strings and compare.
      arrayindex ai;
      ai.base = s->base;
      for (unsigned i=0; i < s->indexes.size(); ++i)
        ai.indexes.push_back(s->indexes[i]);
      string loopai = lex_cast(ai);
      foreach_loop_values[loopai] = value;
      s->block->visit (this);
      foreach_loop_values.erase(loopai);
    }
  else
    s->block->visit (this);
}


bool
c_unparser::get_foreach_loop_value (arrayindex* ai, string& value)
{
  if (!ai)
    return false;
  map<string,string>::iterator it = foreach_loop_values.find(lex_cast(*ai));
  if (it == foreach_loop_values.end())
    return false;
  value = it->second;
  return true;
}


void
c_unparser::visit_foreach_loop (foreach_loop *s)
{
  symbol *array;
  hist_op *hist;
  classify_indexable (s->base, array, hist);

  string ctr = lex_cast (label_counter++);
  string toplabel = "top_" + ctr;
  string contlabel = "continue_" + ctr;
  string breaklabel = "break_" + ctr;

  // PR26269 lockpushdown:
  // for loops, forget optimizing, just emit locks at top & bottom
  if (pushdown_lock_p(s))
    emit_lock();

  if (array)
    {
      mapvar mv = getmap (array->referent, s->tok);
      vector<var> keys;

      // NB: structure parallels for_loop

      // initialization

      tmpvar *res_limit = NULL;
      if (s->limit)
        {
	  // Evaluate the limit expression once.
	  res_limit = new tmpvar(gensym(pe_long));
	  c_assign (*res_limit, s->limit, "foreach limit");
	}

      // aggregate array if required
      if (mv.is_parallel())
	{
	  o->newline() << "if (unlikely(NULL == " << mv.calculate_aggregate() << ")) {";
	  o->newline(1) << "c->last_error = ";
          o->line() << STAP_T_05 << mv << "\";";
	  o->newline() << "c->last_stmt = " << lex_cast_qstring(*s->tok) << ";";
	  o->newline() << "goto out;";
	  o->newline(-1) << "}";

	  // sort array if desired
	  if (s->sort_direction)
	    {
	      string sort_column;

	      // If the user wanted us to sort by value, we'll sort by
	      // @count or selected function instead for aggregates.  
	      // See runtime/map.c
	      if (s->sort_column == 0)
                switch (s->sort_aggr) {
                default: case sc_none: case sc_count: sort_column = "SORT_COUNT"; break;
                case sc_sum: sort_column = "SORT_SUM"; break;
                case sc_min: sort_column = "SORT_MIN"; break;
                case sc_max: sort_column = "SORT_MAX"; break;
                case sc_average: sort_column = "SORT_AVG"; break;
                }
	      else
		sort_column = lex_cast(s->sort_column);

	      o->newline() << "else"; // only sort if aggregation was ok
	      if (s->limit)
	        {
		  o->newline(1) << mv.function_keysym("sortn", true) <<" ("
				<< mv.fetch_existing_aggregate() << ", "
				<< *res_limit << ", " << sort_column << ", "
				<< - s->sort_direction << ");";
		}
	      else
	        {
		  o->newline(1) << mv.function_keysym("sort", true) <<" ("
				<< mv.fetch_existing_aggregate() << ", "
				<< sort_column << ", "
				<< - s->sort_direction << ");";
		}
	      o->indent(-1);
	    }
        }
      else
	{
	  // sort array if desired
	  if (s->sort_direction)
	    {
	      if (s->limit)
	        {
		  o->newline() << mv.function_keysym("sortn") <<" ("
			       << mv.value() << ", "
			       << *res_limit << ", " << s->sort_column << ", "
			       << - s->sort_direction << ");";
		}
	      else
	        {
		  o->newline() << mv.function_keysym("sort") <<" ("
			       << mv.value() << ", "
			       << s->sort_column << ", "
			       << - s->sort_direction << ");";
		}
	    }
	}

      // NB: sort direction sense is opposite in runtime, thus the negation

      tmpvar *limitv = NULL;
      if (s->limit)
      {
	  // Create the loop limit variable here and initialize it.
	  limitv = new tmpvar(gensym (pe_long));
	  o->newline() << *limitv << " = 0LL;";
      }

      if (mv.is_parallel())
	aggregations_active.insert(mv.value());

      itervar iv = getiter (array);
      o->newline() << iv << " = " << iv.start (mv) << ";";

      vector<tmpvar *> array_slice_vars;
      // store the the variables corresponding to the index of the array slice
      // as temporary variables
      if (!s->array_slice.empty())
          for (unsigned i = 0; i < s->array_slice.size(); ++i)
            {
              if (s->array_slice[i])
                {
                  tmpvar *asvar = new tmpvar(gensym(s->array_slice[i]->type));
                  c_assign(*asvar, s->array_slice[i], "array slice index");
                  array_slice_vars.push_back(asvar);
                }
              else
                array_slice_vars.push_back(NULL);
            }

      record_actions(1, s->tok, true);

      // condition
      o->newline(-1) << toplabel << ":";

      // Emit an explicit action here to cover the act of iteration.
      // Equivalently, it can stand for the evaluation of the
      // condition expression.
      o->indent(1);
      record_actions(1, s->tok);

      o->newline() << "if (! (" << iv << ")) goto " << breaklabel << ";";

      // body
      loop_break_labels.push_back (breaklabel);
      loop_continue_labels.push_back (contlabel);
      o->newline() << "{";
      o->indent (1);

      if (s->limit)
      {
	  // If we've been through LIMIT loop iterations, quit.
	  o->newline() << "if (" << *limitv << "++ >= " << *res_limit
		       << ") goto " << breaklabel << ";";

	  // We're done with limitv and res_limit.
	  delete limitv;
	  delete res_limit;
      }

      for (unsigned i = 0; i < s->indexes.size(); ++i)
	{
	  // copy the iter values into the specified locals
	  var v = getvar (s->indexes[i]->referent);
	  c_assign (v, iv.get_key (mv, v.type(), i), s->tok);
	}

      // in the case that the user specified something like
      // foreach ([a,b] in foo[*, 123]), need to check that it iterates over
      // the specified values, ie b is alwasy going to be 123
      if (!s->array_slice.empty())
        {
          //add in the beginning portion of the if statement
          o->newline() << "if (0"; // in case all are wildcards
          for (unsigned i = 0; i < s->array_slice.size(); ++i)

            // only output a comparsion if the expression is not "*".
            if (s->array_slice[i])
            {
              o->line() << " || ";
              if (s->indexes[i]->type == pe_string)
                {
                  if (s->array_slice[i]->type != pe_string)
                    throw SEMANTIC_ERROR (_("expected string types"), s->tok);
                  o->line() << "strncmp(" << getvar (s->indexes[i]->referent)
                            << ", " << *array_slice_vars[i];
                  o->line() << ", MAXSTRINGLEN) !=0";
                }
              else if (s->indexes[i]->type == pe_long)
                {
                  if (s->array_slice[i]->type != pe_long)
                    throw SEMANTIC_ERROR (_("expected numeric types"), s->tok);
                  o->line() << getvar (s->indexes[i]->referent) << " != "
                            << *array_slice_vars[i];
                }
              else
              {
                  throw SEMANTIC_ERROR (_("unexpected type"), s->tok);
              }
            }
          o->line() << ") goto " << contlabel << ";"; // end of the if statment
        }

      if (s->value)
        {
	  var v = getvar (s->value->referent);
	  c_assign (v, iv.get_value (mv, v.type()), s->tok);
        }

      visit_foreach_loop_value(s, iv.get_value(mv, array->type));
      record_actions(0, s->block->tok, true);
      o->newline(-1) << "}";
      loop_break_labels.pop_back ();
      loop_continue_labels.pop_back ();

      // iteration
      o->newline(-1) << contlabel << ":";
      o->newline(1) << iv << " = " << iv.next (mv) << ";";
      o->newline() << "goto " << toplabel << ";";

      // exit
      o->newline(-1) << breaklabel << ":";
      o->newline(1) << "; /* dummy statement */";

      if (mv.is_parallel())
	aggregations_active.erase(mv.value());
    }
  else
    {
      // Iterating over buckets in a histogram.

      // First make sure we have exactly one pe_long variable to use as
      // our bucket index.
      if (s->indexes.size() != 1 || s->indexes[0]->referent->type != pe_long)
	throw SEMANTIC_ERROR(_("Invalid indexing of histogram"), s->tok);

      tmpvar *res_limit = NULL;
      tmpvar *limitv = NULL;
      if (s->limit)
        {
	  // Evaluate the limit expression once.
	  res_limit = new tmpvar(gensym(pe_long));
	  c_assign (*res_limit, s->limit, "foreach limit");

	  // Create the loop limit variable here and initialize it.
	  limitv = new tmpvar(gensym (pe_long));
	  o->newline() << *limitv << " = 0LL;";
	}

      var bucketvar = getvar (s->indexes[0]->referent);

      aggvar agg = gensym_aggregate ();

      var *v = load_aggregate(hist->stat, agg);
      v->assert_hist_compatible(*hist);

      record_actions(1, s->tok, true);
      o->newline() << "for (" << bucketvar << " = 0; "
		   << bucketvar << " < " << v->buckets() << "; "
		   << bucketvar << "++) { ";
      o->newline(1);
      loop_break_labels.push_back (breaklabel);
      loop_continue_labels.push_back (contlabel);

      if (s->limit)
      {
	  // If we've been through LIMIT loop iterations, quit.
	  o->newline() << "if (" << *limitv << "++ >= " << *res_limit
		       << ") break;";

	  // We're done with limitv and res_limit.
	  delete limitv;
	  delete res_limit;
      }

      if (s->value)
        {
          var v = getvar (s->value->referent);
          c_assign (v, agg.get_hist (bucketvar), s->tok);
        }

      visit_foreach_loop_value(s, agg.get_hist(bucketvar));
      record_actions(1, s->block->tok, true);

      o->newline(-1) << contlabel << ":";
      o->newline(1) << "continue;";
      o->newline(-1) << breaklabel << ":";
      o->newline(1) << "break;";
      o->newline(-1) << "}";
      loop_break_labels.pop_back ();
      loop_continue_labels.pop_back ();

      delete v;
    }

  if (pushdown_unlock_p(s))
    emit_unlock();
}


void
c_unparser::visit_return_statement (return_statement* s)
{
  if (current_function == 0)
    throw SEMANTIC_ERROR (_("cannot 'return' from probe"), s->tok);

  // PR26296: We should not encounter a RETURN statement in a
  // lock-relevant section of code (a probe handler body) at all.
  if (pushdown_lock_p(s) || pushdown_unlock_p(s))
    throw SEMANTIC_ERROR (_("unexpected lock pushdown in 'return'"), s->tok);    
  
  if (s->value)
    {
      if (s->value->type != current_function->type)
        throw SEMANTIC_ERROR (_("return type mismatch"), current_function->tok,
                              s->tok);

      c_assign ("l->__retvalue", s->value, "return value");
    }
  else if (current_function->type != pe_unknown)
    throw SEMANTIC_ERROR (_("return type mismatch"), current_function->tok,
                          s->tok);


  
  record_actions(1, s->tok, true);
  o->newline() << "goto out;";
}


void
c_unparser::visit_next_statement (next_statement* s)
{
  /* Set next flag to indicate to caller to call next alternative function */
  if (current_function != 0)
    {
      o->newline() << "c->next = 1;";
      // PR26296: We should not encounter a NEXT statement in a
      // lock-irrelevant section of code (of a function body) at all.
      if (pushdown_lock_p(s) || pushdown_unlock_p(s))
        throw SEMANTIC_ERROR (_("unexpected lock pushdown in 'next'"), s->tok);
    }
  else if (current_probe != 0)
    locks_not_needed_argh(s);

  record_actions(1, s->tok, true);
  o->newline() << "goto out;";
}


struct delete_statement_operand_visitor:
  public throwing_visitor
{
  c_unparser *parent;
  delete_statement_operand_visitor (c_unparser *p):
    throwing_visitor (_("invalid operand of delete expression")),
    parent (p)
  {}
  void visit_symbol (symbol* e);
  void visit_arrayindex (arrayindex* e);
};

void
delete_statement_operand_visitor::visit_symbol (symbol* e)
{
  translator_output* o = parent->o;
  assert (e->referent != 0);
  if (e->referent->arity > 0)
    {
      mapvar mvar = parent->getmap(e->referent, e->tok);
      /* NB: Memory deallocation/allocation operations
       are not generally safe.
      o->newline() << mvar.fini ();
      o->newline() << mvar.init ();
      */
      if (mvar.is_parallel())
	o->newline() << "_stp_pmap_clear (" << mvar.value() << ");";
      else
	o->newline() << "_stp_map_clear (" << mvar.value() << ");";
    }
  else
    {
      var v = parent->getvar(e->referent, e->tok);
      switch (e->type)
	{
	case pe_stats:
	  o->newline() << "_stp_stat_clear (" << v.value() << ");";
	  break;
	case pe_long:
	  o->newline() << v.value() << " = 0;";
	  break;
	case pe_string:
	  o->newline() << v.value() << "[0] = '\\0';";
	  break;
	case pe_unknown:
	default:
	  throw SEMANTIC_ERROR(_("Cannot delete unknown expression type"), e->tok);
	}
    }
}

void
delete_statement_operand_visitor::visit_arrayindex (arrayindex* e)
{
  symbol *array;
  hist_op *hist;
  classify_indexable (e->base, array, hist);
  translator_output* o = parent->o;

  if (array)
    {
      bool array_slice = false;
      for (unsigned i = 0; i < e->indexes.size(); i ++)
        if (e->indexes[i] == NULL)
          {
            array_slice = true;
            break;
          }

      if (!array_slice) // delete a single element
        {
          vector<tmpvar> idx;
          parent->load_map_indices (e, idx);
          mapvar mvar = parent->getmap (array->referent, e->tok);
          o->newline() << mvar.del (idx) << ";";
        }
      else // delete elements if they match the array slice.
        {
          vardecl* r = array->referent;
          mapvar mvar = parent->getmap (r, e->tok);
          itervar iv = parent->getiter(array);

          // create tmpvars for the array indexes, storing NULL where there is
          // no specific value that the index should be
          vector<tmpvar *> array_slice_vars;
          vector<tmpvar> idx; // for the indexes if the variable is a pmap
          for (unsigned i=0; i<e->indexes.size(); i++)
            {
              if (e->indexes[i])
                {
                  tmpvar *asvar = new tmpvar(parent->gensym(e->indexes[i]->type));
                  parent->c_assign (*asvar, e->indexes[i], "tmp var");
                  array_slice_vars.push_back(asvar);
                  if (mvar.is_parallel())
                    idx.push_back(*asvar);
                }
              else
                {
                  array_slice_vars.push_back(NULL);
                  if (mvar.is_parallel())
                    {
                      tmpvar *asvar = new tmpvar(parent->gensym(r->index_types[i]));
                      idx.push_back(*asvar);
                    }
                }
            }

          if (mvar.is_parallel())
            {
              o->newline() << "if (unlikely(NULL == "
                           << mvar.calculate_aggregate() << ")) {";
              o->newline(1) << "c->last_error = ";
              o->line() << STAP_T_05 << mvar << "\";";
              o->newline() << "c->last_stmt = "
                           << lex_cast_qstring(*e->tok) << ";";
              o->newline() << "goto out;";
              o->newline(-1) << "}";
            }

          // iterate through the map, deleting elements that match the array slice
          string ctr = lex_cast (parent->label_counter++);
          string toplabel = "top_" + ctr;
          string breaklabel = "break_" + ctr;

          o->newline() << iv << " = " << iv.start(mvar) << ";";
          o->newline() << toplabel << ":";

          o->newline(1) << "if (!(" << iv << ")){";
          o->newline(1) << "goto " << breaklabel << ";}";

          // insert the comparison for keys that aren't wildcards
          o->newline(-1) << "if (1"; // in case all are wildcards
          for (unsigned i=0; i<array_slice_vars.size(); i++)
            if (array_slice_vars[i] != NULL)
              {
              if (array_slice_vars[i]->type() == pe_long)
                o->line() << " && " << *array_slice_vars[i] << " == "
                          << iv.get_key(mvar, array_slice_vars[i]->type(), i);
              else if (array_slice_vars[i]->type() == pe_string)
                o->line() << " && strncmp(" << *array_slice_vars[i] << ", "
                          << iv.get_key(mvar, array_slice_vars[i]->type(), i)
                          << ", MAXSTRINGLEN) == 0";
              else
                throw SEMANTIC_ERROR (_("unexpected type"), e->tok);
              }

          o->line() <<  ") {";

          // conditional is true, so delete item and go to the next item
          if (mvar.is_parallel())
            {
              o->indent(1);
              // fills in the wildcards with the current iteration's (map) indexes
              for (unsigned i = 0; i<array_slice_vars.size(); i++)
                if (array_slice_vars[i] == NULL)
                  parent->c_assign (idx[i].value(),
                                    iv.get_key(mvar, r->index_types[i], i),
                                    r->index_types[i], "tmpvar", e->tok);
              o->newline() << iv << " = " << iv.next(mvar) << ";";
              o->newline() << mvar.del(idx) << ";";
            }
          else
            o->newline(1) << iv << " = " << iv.del_next(mvar) << ";";

          o->newline(-1) << "} else";
          o->newline(1) << iv << " = " << iv.next(mvar) << ";";

          o->newline(-1) << "goto " << toplabel << ";";

          o->newline(-1) << breaklabel<< ":";
          o->newline(1) << "; /* dummy statement */";
          o->indent(-1);
        }
    }
  else
    {
      throw SEMANTIC_ERROR(_("cannot delete histogram bucket entries\n"), e->tok);
    }
}


void
c_unparser::visit_delete_statement (delete_statement* s)
{
  bool ln = locks_needed_p (s);

  if (!ln) // unlikely, as delete usually operates on globals
    locks_not_needed_argh(s);

  if (ln && pushdown_lock_p(s))
    emit_lock();
  
  delete_statement_operand_visitor dv (this);
  s->value->visit (&dv);

  if (ln && pushdown_unlock_p(s))
    emit_unlock();

  record_actions(1, s->tok);
}


void
c_unparser::visit_break_statement (break_statement* s)
{
  locks_not_needed_argh(s);

  if (loop_break_labels.empty())
    throw SEMANTIC_ERROR (_("cannot 'break' outside loop"), s->tok);

  record_actions(1, s->tok, true);
  o->newline() << "goto " << loop_break_labels.back() << ";";
}


void
c_unparser::visit_continue_statement (continue_statement* s)
{
  locks_not_needed_argh(s);

  if (loop_continue_labels.empty())
    throw SEMANTIC_ERROR (_("cannot 'continue' outside loop"), s->tok);

  record_actions(1, s->tok, true);
  o->newline() << "goto " << loop_continue_labels.back() << ";";
}



void
c_unparser::visit_literal_string (literal_string* e)
{
  interned_string v = e->value;
  o->line() << '"';
  for (unsigned i=0; i<v.size(); i++)
    // NB: The backslash character is specifically passed through as is.
    // This is because our parser treats "\" as an ordinary character, not
    // an escape sequence, leaving it to the C compiler (and this function)
    // to treat it as such.  If we were to escape it, there would be no way
    // of generating C-level escapes from script code.
    // See also print_format::components_to_string and lex_cast_qstring
    if (v[i] == '"') // or other escapeworthy characters?
      o->line() << '\\' << '"';
    else
      o->line() << v[i];
  o->line() << '"';
}


void
c_unparser::visit_literal_number (literal_number* e)
{
  // This looks ugly, but tries to be warning-free on 32- and 64-bit
  // hosts.
  // NB: this needs to be signed!
  if (e->value == -9223372036854775807LL-1) // PR 5023
    o->line() << "((int64_t)" << (unsigned long long) e->value << "ULL)";
  else
    o->line() << "((int64_t)" << e->value << "LL)";
}


void
c_unparser::visit_embedded_expr (embedded_expr* e)
{
  bool has_defines = false;
  vector<vardecl*> read_defs;
  vector<vardecl*> write_defs;
  for (unsigned i = 0; i < session->globals.size(); i++)
    {
      vardecl* v = session->globals[i];
      if (v->synthetic) continue; /* skip synthetic variables; embedded c can't access them. */
      string name = v->unmangled_name;
      assert (name != "");
      if (e->tagged_p ("/* pragma:read:" + name + " */"))
        {
          has_defines = true;
          c_global_read_def(v);
          read_defs.push_back(v);
        }
      if (e->tagged_p ("/* pragma:write:" + name + " */"))
        {
          has_defines = true;
          c_global_write_def(v);
          write_defs.push_back(v);
        }
    }

  if (has_defines)
    o->newline();

  o->line() << "(";

  // Automatically add a call to assert_is_myproc to any code tagged with
  // /* myproc-unprivileged */
  if (e->tagged_p ("/* myproc-unprivileged */"))
    o->line() << "({ assert_is_myproc(); }), ";

  if (e->type == pe_long)
    o->line() << "((int64_t) (" << e->code << "))";
  else if (e->type == pe_string)
    o->line() << "((const char *) (" << e->code << "))";
  else
    throw SEMANTIC_ERROR (_("expected numeric or string type"), e->tok);

  o->line() << ")";

  for (vector<vardecl*>::const_iterator it = read_defs.begin(); it != read_defs.end(); ++it)
    c_global_read_undef(*it);
  for (vector<vardecl*>::const_iterator it = write_defs.begin(); it != write_defs.end(); ++it)
    c_global_write_undef(*it);

  if (has_defines)
    o->newline();
}


void
c_unparser::visit_binary_expression (binary_expression* e)
{
  if (e->type != pe_long ||
      e->left->type != pe_long ||
      e->right->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric types"), e->tok);

  if (e->op == "+" ||
      e->op == "-" ||
      e->op == "*" ||
      e->op == "&" ||
      e->op == "|" ||
      e->op == "^")
    {
      o->line() << "((";
      e->left->visit (this);
      o->line() << ") " << e->op << " (";
      e->right->visit (this);
      o->line() << "))";
    }
  else if (e->op == ">>" ||
           e->op == "<<")
    {
      o->line() << "((int64_t)(";
      e->left->visit (this);
      o->line() << ") " << e->op << " ((";
      e->right->visit (this);
      o->line() << ") & 63))";
    }
  else if (e->op == ">>>")
    {
      o->line() << "(int64_t)((uint64_t)(";
      e->left->visit (this);
      o->line() << ") >> ((";
      e->right->visit (this);
      o->line() << ") & 63))";
    }
  else if (e->op == "/" ||
           e->op == "%")
    {
      // % and / need a division-by-zero check; and thus two temporaries
      // for proper evaluation order
      tmpvar left = gensym (pe_long);
      tmpvar right = gensym (pe_long);

      o->line() << "({";
      o->indent(1);

      c_assign (left, e->left, "division");
      c_assign (right, e->right, "division");

      o->newline() << "if (unlikely(!" << right << ")) {";
      o->newline(1) << "c->last_error = ";
      o->line() << STAP_T_03;
      o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
      o->newline() << "goto out;";
      o->newline(-1) << "}";
      o->newline() << ((e->op == "/") ? "_stp_div64" : "_stp_mod64")
		   << " (NULL, " << left << ", " << right << ");";

      o->newline(-1) << "})";
    }
  else
    throw SEMANTIC_ERROR (_F("operator %s not yet implemented", string(e->op).c_str()), e->tok);
}


void
c_unparser::visit_unary_expression (unary_expression* e)
{
  if (e->type != pe_long ||
      e->operand->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric types"), e->tok);

  if (e->op == "-")
    {
      // NB: Subtraction is special, since negative literals in the
      // script language show up as unary negations over positive
      // literals here.  This makes it "exciting" for emitting pure
      // C since: - 0x8000_0000_0000_0000 ==> - (- 9223372036854775808)
      // This would constitute a signed overflow, which gcc warns on
      // unless -ftrapv/-J are in CFLAGS - which they're not.

      o->line() << "(int64_t)(0 " << e->op << " (uint64_t)(";
      e->operand->visit (this);
      o->line() << "))";
    }
  else
    {
      o->line() << "(" << e->op << " (";
      e->operand->visit (this);
      o->line() << "))";
    }
}

void
c_unparser::visit_logical_or_expr (logical_or_expr* e)
{
  if (e->type != pe_long ||
      e->left->type != pe_long ||
      e->right->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric types"), e->tok);

  o->line() << "((";
  e->left->visit (this);
  o->line() << ") " << e->op << " (";
  e->right->visit (this);
  o->line() << "))";
}


void
c_unparser::visit_logical_and_expr (logical_and_expr* e)
{
  if (e->type != pe_long ||
      e->left->type != pe_long ||
      e->right->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric types"), e->tok);

  o->line() << "((";
  e->left->visit (this);
  o->line() << ") " << e->op << " (";
  e->right->visit (this);
  o->line() << "))";
}


void
c_unparser::visit_array_in (array_in* e)
{
  symbol *array;
  hist_op *hist;
  classify_indexable (e->operand->base, array, hist);

  if (array)
    {
      stmt_expr block(*this);

      tmpvar res = gensym (pe_long);
      vector<tmpvar> idx;

      // determine if the array index contains an asterisk
      bool array_slice = false;
      for (unsigned i = 0; i < e->operand->indexes.size(); i ++)
        if (e->operand->indexes[i] == NULL)
          {
            array_slice = true;
            break;
          }

      if (!array_slice) // checking for membership of a specific element
        {
          load_map_indices (e->operand, idx);
          // o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";

          mapvar mvar = getmap (array->referent, e->tok);
          c_assign (res, mvar.exists(idx), e->tok);

          o->newline() << res << ";";
        }
      else
        {
          // create tmpvars for the array indexes, storing NULL where there is
          // no specific value that the index should be
          vector<tmpvar *> array_slice_vars;
          for (unsigned i=0; i<e->operand->indexes.size(); i++)
            {
              if (e->operand->indexes[i])
                {
                  tmpvar *asvar = new tmpvar(gensym(e->operand->indexes[i]->type));
                  c_assign (*asvar, e->operand->indexes[i], "tmp var");
                  array_slice_vars.push_back(asvar);
                }
              else
                array_slice_vars.push_back(NULL);
            }

          mapvar mvar = getmap (array->referent, e->operand->tok);
          itervar iv = getiter(array);
          vector<tmpvar> idx;

          // we may not need to aggregate if we're already in a foreach
          bool pre_agg = (aggregations_active.count(mvar.value()) > 0);
          if (mvar.is_parallel() && !pre_agg)
            {
              o->newline() << "if (unlikely(NULL == "
                           << mvar.calculate_aggregate() << ")) {";
              o->newline(1) << "c->last_error = ";
              o->line() << STAP_T_05 << mvar << "\";";
              o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
              o->newline() << "goto out;";
              o->newline(-1) << "}";
            }

          string ctr = lex_cast (label_counter++);
          string toplabel = "top_" + ctr;
          string contlabel = "continue_" + ctr;
          string breaklabel = "break_" + ctr;

          o->newline() << iv << " = " << iv.start(mvar) << ";";
          c_assign (res, "0", e->tok); // set the default to 0

          o->newline() << toplabel << ":";

          o->newline(1) << "if (!(" << iv << "))";
          o->newline(1) << "goto " << breaklabel << ";";

          // generate code for comparing the keys to the index slice
          o->newline(-1) << "if (1"; // in case all are wildcards
          for (unsigned i=0; i<array_slice_vars.size(); i++)
            {
              if (array_slice_vars[i] != NULL)
                {
                if (array_slice_vars[i]->type() == pe_long)
                  o->line() << " && " << *array_slice_vars[i] << " == "
                            << iv.get_key(mvar, array_slice_vars[i]->type(), i);
                else if (array_slice_vars[i]->type() == pe_string)
                  o->line() << " && strncmp(" << *array_slice_vars[i] << ", "
                            << iv.get_key(mvar, array_slice_vars[i]->type(), i)
                            << ", MAXSTRINGLEN) == 0";
                else
                  throw SEMANTIC_ERROR (_("unexpected type"), e->tok);
                }
            }
          o->line() <<  "){";
          o->indent(1);
          // conditional is true, so set res and go to break
          c_assign (res, "1", e->tok);
          o->newline() << "goto " << breaklabel << ";";
          o->newline(-1) << "}";

          // else, keep iterating
          o->newline() << iv << " = " << iv.next(mvar) << ";";
          o->newline() << "goto " << toplabel << ";";

          o->newline(-1) << breaklabel<< ":";
          o->newline(1) << "; /* dummy statement */";
          o->newline(-1) << res << ";";
        }

    }
  else
    {
      // By definition:
      //
      // 'foo in @hist_op(...)'  is true iff
      // '@hist_op(...)[foo]'    is nonzero
      //
      // so we just delegate to the latter call, since int64_t is also
      // our boolean type.
      e->operand->visit(this);
    }
}

void
c_unparser::visit_regex_query (regex_query* e)
{
  o->line() << "(";
  o->indent(1);
  o->newline();
  if (e->op == "!~") o->line() << "!";
  stapdfa *dfa = session->dfas[e->right->value];
  dfa->emit_matchop_start (o);
  e->left->visit(this);
  dfa->emit_matchop_end (o);
  o->newline(-1) << ")";
}

void
c_unparser::visit_compound_expression(compound_expression* e)
{
  o->line() << "(";
  e->left->visit (this);
  o->line() << ", ";
  e->right->visit (this);
  o->line() << ")";
}

void
c_unparser::visit_comparison (comparison* e)
{
  o->line() << "(";

  if (e->left->type == pe_string)
    {
      if (e->right->type != pe_string)
        throw SEMANTIC_ERROR (_("expected string types"), e->tok);

      // PR13283 indicated that we may need a temporary variable to
      // store the operand strings, if e.g. they are both references
      // into function call __retvalue's, which overlap in memory.
      // ... but we now handle that inside the function call machinery,
      // which always returns an allocated temporary variable.

      o->line() << "(strncmp ((";
      e->left->visit (this);
      o->line() << "), (";
      e->right->visit (this);
      o->line() << "), MAXSTRINGLEN) " << e->op << " 0)";
    }
  else if (e->left->type == pe_long)
    {
      if (e->right->type != pe_long)
        throw SEMANTIC_ERROR (_("expected numeric types"), e->tok);

      o->line() << "((";
      e->left->visit (this);
      o->line() << ") " << e->op << " (";
      e->right->visit (this);
      o->line() << "))";
    }
  else
    throw SEMANTIC_ERROR (_("unexpected type"), e->left->tok);

  o->line() << ")";
}


void
c_unparser::visit_concatenation (concatenation* e)
{
  if (e->op != ".")
    throw SEMANTIC_ERROR (_("unexpected concatenation operator"), e->tok);

  if (e->type != pe_string ||
      e->left->type != pe_string ||
      e->right->type != pe_string)
    throw SEMANTIC_ERROR (_("expected string types"), e->tok);

  tmpvar t = gensym (e->type);

  o->line() << "({ ";
  o->indent(1);
  // o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
  c_assign (t.value(), e->left, "assignment");
  c_strcat (t.value(), e->right);
  o->newline() << t << ";";
  o->newline(-1) << "})";
}


void
c_unparser::visit_ternary_expression (ternary_expression* e)
{
  if (e->cond->type != pe_long)
    throw SEMANTIC_ERROR (_("expected numeric condition"), e->cond->tok);

  if (e->truevalue->type != e->falsevalue->type ||
      e->type != e->truevalue->type ||
      (e->truevalue->type != pe_long && e->truevalue->type != pe_string))
    throw SEMANTIC_ERROR (_("expected matching types"), e->tok);

  o->line() << "((";
  e->cond->visit (this);
  o->line() << ") ? (";
  e->truevalue->visit (this);
  o->line() << ") : (";
  e->falsevalue->visit (this);
  o->line() << "))";
}


void
c_unparser::visit_assignment (assignment* e)
{
  if (e->op == "<<<")
    {
      if (e->type != pe_long)
	throw SEMANTIC_ERROR (_("non-number <<< expression"), e->tok);

      if (e->left->type != pe_stats)
	throw SEMANTIC_ERROR (_("non-stats left operand to <<< expression"), e->left->tok);

      if (e->right->type != pe_long)
	throw SEMANTIC_ERROR (_("non-number right operand to <<< expression"), e->right->tok);

    }
  else
    {
      if (e->type != e->left->type)
	throw SEMANTIC_ERROR (_("type mismatch"), e->tok, e->left->tok);
      if (e->right->type != e->left->type)
	throw SEMANTIC_ERROR (_("type mismatch"), e->right->tok, e->left->tok);
    }

  c_unparser_assignment tav (this, e->op, e->right);
  e->left->visit (& tav);
}


void
c_unparser::visit_pre_crement (pre_crement* e)
{
  if (e->type != pe_long ||
      e->type != e->operand->type)
    throw SEMANTIC_ERROR (_("expected numeric type"), e->tok);

  c_unparser_assignment tav (this, e->op, false);
  e->operand->visit (& tav);
}


void
c_unparser::visit_post_crement (post_crement* e)
{
  if (e->type != pe_long ||
      e->type != e->operand->type)
    throw SEMANTIC_ERROR (_("expected numeric type"), e->tok);

  c_unparser_assignment tav (this, e->op, true);
  e->operand->visit (& tav);
}


void
c_unparser::visit_symbol (symbol* e)
{
  assert (e->referent != 0);
  vardecl* r = e->referent;

  if (r->index_types.size() != 0)
    throw SEMANTIC_ERROR (_("invalid reference to array"), e->tok);

  var v = getvar(r, e->tok);
  o->line() << v;
}

void
c_unparser::visit_target_register (target_register* e)
{
  o->line() << (e->userspace_p ? "u_fetch_register(" : "k_fetch_register(")
	    << e->regno
	    << ")";
}

void
c_unparser::visit_target_deref (target_deref* e)
{
  if (e->signed_p)
    {
      switch (e->size)
	{
	case 1:
	  o->line() << "(int64_t)(int8_t)";
	  break;
	case 2:
	  o->line() << "(int64_t)(int16_t)";
	  break;
	case 4:
	  o->line() << "(int64_t)(int32_t)";
	  break;
	case 8:
	  break;
	default:
	  abort();
	}
    }
  o->line() << (e->userspace_p ? "uderef(" : "kderef(")
	    << e->size << ", (";
  e->addr->visit (this);
  o->line() << "))";
}

void
c_unparser::visit_target_bitfield (target_bitfield*)
{
  // These are all expanded much earlier.
  abort();
}

// Assignment expansion is tricky.
//
// Because assignments are nestable expressions, we have
// to emit C constructs that are nestable expressions too.
// We have to evaluate the given expressions the proper number of times,
// including array indices.
// We have to lock the lvalue (if global) against concurrent modification,
// especially with modify-assignment operations (+=, ++).
// We have to check the rvalue (for division-by-zero checks).

// In the normal "pre=false" case, for (A op B) emit:
// ({ tmp = B; check(B); lock(A); res = A op tmp; A = res; unlock(A); res; })
// In the "pre=true" case, emit instead:
// ({ tmp = B; check(B); lock(A); res = A; A = res op tmp; unlock(A); res; })
//
// (op is the plain operator portion of a combined calculate/assignment:
// "+" for "+=", and so on.  It is in the "macop" variable below.)
//
// For array assignments, additional temporaries are used for each
// index, which are expanded before the "tmp=B" expression, in order
// to consistently order evaluation of lhs before rhs.
//

void
c_unparser_assignment::prepare_rvalue (interned_string op,
				       tmpvar & rval,
				       token const * tok)
{
  if (rvalue)
    parent->c_assign (rval, rvalue, "assignment");
  else
    {
      if (op == "++" || op == "--")
	// Here is part of the conversion proccess of turning "x++" to
	// "x += 1".
        rval.override("1");
      else
        throw SEMANTIC_ERROR (_("need rvalue for assignment"), tok);
    }
}

void
c_unparser_assignment::visit_symbol (symbol *e)
{
  stmt_expr block(*parent);
  translator_output* o = parent->o;

  assert (e->referent != 0);
  if (e->referent->index_types.size() != 0)
    throw SEMANTIC_ERROR (_("unexpected reference to array"), e->tok);

  // o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
  exp_type ty = rvalue ? rvalue->type : e->type;
  tmpvar rval = parent->gensym (ty);
  tmpvar res = parent->gensym (ty);

  prepare_rvalue (op, rval, e->tok);

  var lvar = parent->getvar (e->referent, e->tok);
  c_assignop (res, lvar, rval, e->tok);

  o->newline() << res << ";";
}

void
c_unparser_assignment::visit_target_register (target_register* e)
{
  exp_type ty = rvalue ? rvalue->type : e->type;
  assert(ty == pe_long);

  tmpvar rval = parent->gensym (pe_long);
  prepare_rvalue (op, rval, e->tok);

  // Given how target_registers are created in loc2stap.cxx,
  // we should never see anything other than simple assignment.
  assert(op == "=");

  translator_output* o = parent->o;
  o->newline() << (e->userspace_p ? "u_store_register(" : "k_store_register(")
	       << e->regno << ", " << rval << ")";
}

void
c_unparser_assignment::visit_target_deref (target_deref* e)
{
  exp_type ty = rvalue ? rvalue->type : e->type;
  assert(ty == pe_long);

  tmpvar rval = parent->gensym (pe_long);

  prepare_rvalue (op, rval, e->tok);

  // Given how target_deref are created in loc2stap.cxx,
  // we should never see anything other than simple assignment.
  assert(op == "=");

  translator_output* o = parent->o;
  o->newline() << (e->userspace_p ? "store_uderef(" : "store_kderef(")
	       << e->size << ", (";
  e->addr->visit (parent);
  o->line() << "), " << rval << ")";
}

void
c_unparser::visit_target_symbol (target_symbol* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general target-symbol expression"), e->tok);
}


void
c_unparser::visit_atvar_op (atvar_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @var expression"), e->tok);
}


void
c_unparser::visit_cast_op (cast_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @cast expression"), e->tok);
}


void
c_unparser::visit_autocast_op (autocast_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general dereference expression"), e->tok);
}


void
c_unparser::visit_defined_op (defined_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @defined expression"), e->tok);
}


void
c_unparser::visit_probewrite_op (probewrite_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @probewrite expression"), e->tok);
}


void
c_unparser::visit_entry_op (entry_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @entry expression"), e->tok);
}


void
c_unparser::visit_perf_op (perf_op* e)
{
  throw SEMANTIC_ERROR(_("cannot translate general @perf expression"), e->tok);
}


void
c_unparser::load_map_indices(arrayindex *e,
			     vector<tmpvar> & idx)
{
  symbol *array;
  hist_op *hist;
  classify_indexable (e->base, array, hist);

  if (array)
    {
      idx.clear();

      assert (array->referent != 0);
      vardecl* r = array->referent;

      if (r->index_types.size() == 0 ||
	  r->index_types.size() != e->indexes.size())
	throw SEMANTIC_ERROR (_("invalid array reference"), e->tok);

      for (unsigned i=0; i<r->index_types.size(); i++)
	{
	  if (r->index_types[i] != e->indexes[i]->type)
	    throw SEMANTIC_ERROR (_("array index type mismatch"), e->indexes[i]->tok);

	  tmpvar ix = gensym (r->index_types[i]);
	  c_assign (ix, e->indexes[i], "array index copy");
	  idx.push_back (ix);
	}
    }
  else
    {
      assert (e->indexes.size() == 1);
      assert (e->indexes[0]->type == pe_long);
      tmpvar ix = gensym (pe_long);
      c_assign (ix, e->indexes[0], "array index copy");
      idx.push_back(ix);
    }
}


var*
c_unparser::load_aggregate (expression *e, aggvar & agg)
{
  symbol *sym = get_symbol_within_expression (e);

  if (sym->referent->type != pe_stats)
    throw SEMANTIC_ERROR (_("unexpected aggregate of non-statistic"), sym->tok);

  var *v;
  if (sym->referent->arity == 0)
    {
      v = new var(getvar(sym->referent, sym->tok));
      // o->newline() << "c->last_stmt = " << lex_cast_qstring(*sym->tok) << ";";
      o->newline() << agg << " = _stp_stat_get (" << *v << ", 0);";
    }
  else
    {
      mapvar *mv = new mapvar(getmap(sym->referent, sym->tok));
      v = mv;

      arrayindex *arr = NULL;
      if (!expression_is_arrayindex (e, arr))
	throw SEMANTIC_ERROR(_("unexpected aggregate of non-arrayindex"), e->tok);

      // If we have a foreach_loop value, we don't need to index the map
      string agg_value;
      if (get_foreach_loop_value(arr, agg_value))
        o->newline() << agg << " = " << agg_value << ";";
      else
        {
          vector<tmpvar> idx;
          load_map_indices (arr, idx);
          // o->newline() << "c->last_stmt = " << lex_cast_qstring(*sym->tok) << ";";
	  bool pre_agg = (aggregations_active.count(mv->value()) > 0);
          o->newline() << agg << " = " << mv->get(idx, pre_agg) << ";";
        }
    }

  return v;
}


string
c_unparser::histogram_index_check(var & base, tmpvar & idx) const
{
  return "((" + idx.value() + " >= 0)"
    + " && (" + idx.value() + " < " + base.buckets() + "))";
}


void
c_unparser::visit_arrayindex (arrayindex* e)
{
  // If we have a foreach_loop value, use it and call it a day!
  string ai_value;
  if (get_foreach_loop_value(e, ai_value))
    {
      o->line() << ai_value;
      return;
    }

  symbol *array;
  hist_op *hist;
  classify_indexable (e->base, array, hist);

  if (array)
    {
      // Visiting an statistic-valued array in a non-lvalue context is prohibited.
      if (array->referent->type == pe_stats)
	throw SEMANTIC_ERROR (_("statistic-valued array in rvalue context"), e->tok);

      stmt_expr block(*this);

      vector<tmpvar> idx;
      load_map_indices (e, idx);
      tmpvar res = gensym (e->type);

      mapvar mvar = getmap (array->referent, e->tok);
      // o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
      c_assign (res, mvar.get(idx), e->tok);

      o->newline() << res << ";";
    }
  else
    {
      // Note: this is a slightly tricker-than-it-looks allocation of
      // temporaries. The reason is that we're in the branch handling
      // histogram-indexing, and the histogram might be build over an
      // indexable entity itself. For example if we have:
      //
      //  global foo
      //  ...
      //  foo[getpid(), geteuid()] <<< 1
      //  ...
      //  print @log_hist(foo[pid, euid])[bucket]
      //
      // We are looking at the @log_hist(...)[bucket] expression, so
      // allocating one tmpvar for calculating bucket (the "index" of
      // this arrayindex expression), and one tmpvar for storing the
      // result in, just as normal.
      //
      // But we are *also* going to call load_aggregate on foo, which
      // will itself require tmpvars for each of its indices. Since
      // this is not handled by delving into the subexpression (it
      // would be if hist were first-class in the type system, but
      // it's not) we we allocate all the tmpvars used in such a
      // subexpression up here: first our own aggvar, then our index
      // (bucket) tmpvar, then all the index tmpvars of our
      // pe_stat-valued subexpression, then our result.

      assert(hist);
      stmt_expr block(*this);

      aggvar agg = gensym_aggregate ();

      vector<tmpvar> idx;
      load_map_indices (e, idx);
      tmpvar res = gensym (e->type);

      // These should have faulted during elaboration if not true.
      if (idx.size() != 1 || idx[0].type() != pe_long)
	throw SEMANTIC_ERROR(_("Invalid indexing of histogram"), e->tok);

      var *v = load_aggregate(hist->stat, agg);
      v->assert_hist_compatible(*hist);

      o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";

      // PR 2142+2610: empty aggregates
      o->newline() << "if (unlikely (" << agg.value() << " == NULL)"
                   << " || " <<  agg.value() << "->count == 0) {";
      o->newline(1) << "c->last_error = ";
      o->line() << STAP_T_06;
      o->newline() << "goto out;";
      o->newline(-1) << "} else {";
      o->newline(1) << "if (" << histogram_index_check(*v, idx[0]) << ")";
      o->newline(1)  << res << " = " << agg << "->histogram[" << idx[0] << "];";
      o->newline(-1) << "else {";
      o->newline(1)  << "c->last_error = ";
      o->line() << STAP_T_07;
      o->newline() << "goto out;";
      o->newline(-1) << "}";

      o->newline(-1) << "}";
      o->newline() << res << ";";

      delete v;
    }
}


void
c_unparser_assignment::visit_arrayindex (arrayindex *e)
{
  symbol *array;
  hist_op *hist;
  classify_indexable (e->base, array, hist);

  if (array)
    {

      stmt_expr block(*parent);

      translator_output *o = parent->o;

      if (array->referent->index_types.size() == 0)
	throw SEMANTIC_ERROR (_("unexpected reference to scalar"), e->tok);

      vector<tmpvar> idx;
      parent->load_map_indices (e, idx);
      exp_type ty = rvalue ? rvalue->type : e->type;
      tmpvar rvar = parent->gensym (ty);
      tmpvar lvar = parent->gensym (ty);
      tmpvar res = parent->gensym (ty);

      // NB: because these expressions are nestable, emit this construct
      // thusly:
      // ({ tmp0=(idx0); ... tmpN=(idxN); rvar=(rhs); lvar; res;
      //    lock (array);
      //    lvar = get (array,idx0...N); // if necessary
      //    assignop (res, lvar, rvar);
      //    set (array, idx0...N, lvar);
      //    unlock (array);
      //    res; })
      //
      // we store all indices in temporary variables to avoid nasty
      // reentrancy issues that pop up with nested expressions:
      // e.g. ++a[a[c]=5] could deadlock
      //
      //
      // There is an exception to the above form: if we're doign a <<< assigment to
      // a statistic-valued map, there's a special form we follow:
      //
      // ({ tmp0=(idx0); ... tmpN=(idxN); rvar=(rhs);
      //    *no need to* lock (array);
      //    _stp_map_add_stat (array, idx0...N, rvar);
      //    *no need to* unlock (array);
      //    rvar; })
      //
      // To simplify variable-allocation rules, we assign rvar to lvar and
      // res in this block as well, even though they are technically
      // superfluous.

      prepare_rvalue (op, rvar, e->tok);

      if (op == "<<<")
	{
	  assert (e->type == pe_stats);
	  assert (rvalue->type == pe_long);

	  mapvar mvar = parent->getmap (array->referent, e->tok);
	  o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
	  o->newline() << mvar.add (idx, rvar) << ";";
          res = rvar;
	  // no need for these dummy assignments
	  // o->newline() << lvar << " = " << rvar << ";";
	  // o->newline() << res << " = " << rvar << ";";
	}
      else
	{
	  mapvar mvar = parent->getmap (array->referent, e->tok);
	  o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
	  if (op != "=") // don't bother fetch slot if we will just overwrite it
	    parent->c_assign (lvar, mvar.get(idx), e->tok);
	  c_assignop (res, lvar, rvar, e->tok);
	  o->newline() << mvar.set (idx, lvar) << ";";
	}

      o->newline() << res << ";";
    }
  else
    {
      throw SEMANTIC_ERROR(_("cannot assign to histogram buckets"), e->tok);
    }
}


void
c_unparser::visit_functioncall (functioncall* e)
{
  assert (!e->referents.empty());

  stmt_expr block(*this);

  vector<bool> cp_arg(e->args.size(), true);
  for (unsigned fd = 0; fd < e->referents.size(); fd++)
    {
      functiondecl* r = e->referents[fd];

      if (r->formal_args.size() != e->args.size())
        throw SEMANTIC_ERROR (_("invalid length argument list"), e->tok);

      for (unsigned i = 0; i < e->args.size(); i++)
        {
          if (r->formal_args[i]->type != e->args[i]->type)
            throw SEMANTIC_ERROR (_("function argument type mismatch"),
                                  e->args[i]->tok, r->formal_args[i]->tok);
        }

      // all alternative functions must be compatible if passing by
      // char pointer
      for (unsigned i = 0; i < r->formal_args.size(); i++)
        {
          if (!r->formal_args[i]->char_ptr_arg)
            cp_arg[i] = false;
        }
    }

  // NB: we store all actual arguments in temporary variables,
  // to avoid colliding sharing of context variables with
  // nested function calls: f(f(f(1)))

  // compute actual arguments
  vector<tmpvar> tmp;
  for (unsigned i=0; i<e->args.size(); i++)
    {
      tmpvar t = gensym(e->args[i]->type);

      symbol *sym_out;
      if (cp_arg[i] && e->args[i]->is_symbol(sym_out)
          && is_local(sym_out->referent, sym_out->tok))
        t.override(getvar(sym_out->referent, sym_out->tok).value());
      else
        c_assign (t, e->args[i],
                  _("function actual argument evaluation"));
      tmp.push_back(t);
    }

  // overloading execution logic for functioncall:
  //
  // - copy in computed function arguments for overload_0
  // - make the functioncall for overload_0 and overwrite return variable
  // - if context next flag is not set, goto fc_end
  //                    *
  //                    *
  //                    *
  // - copy in computed function arguments for overload_n
  // - make the functioncall for overload_n and overwrite return variable
  // fc_end:
  // - yield return value

  // store the return value after the function arguments have been worked out
  // to avoid problems that may occure with nesting.
  tmpvar tmp_ret = gensym (e->type);

  // NB: as per PR13283, it's important we always allocate a distinct
  // temporary value to receive the return value.  (We can pass its
  // address by reference to the function if we like.)
  
  bool yield = false; // set if statement expression is non void

  for (unsigned fd = 0; fd < e->referents.size(); fd++)
    {
      functiondecl* r = e->referents[fd];

      // copy in actual arguments
      for (unsigned i=0; i<e->args.size(); i++)
        {
          if (r->formal_args[i]->char_ptr_arg)
            o->newline() << "c->locals[c->nesting+1]." + c_funcname (r->name) + "."
                            + c_localname (r->formal_args[i]->name) << " = "
                         << tmp[i].value() << ";";
          else
            c_assign ("c->locals[c->nesting+1]." +
                      c_funcname (r->name) + "." +
                      c_localname (r->formal_args[i]->name),
                      tmp[i].value(),
                      e->args[i]->type,
                      "function actual argument copy",
                      e->args[i]->tok);
        }
      // optimized string returns need a local storage pointer.
      bool pointer_ret = (e->type == pe_string && !session->unoptimized);
      if (pointer_ret)
        {
          if (e == assigned_functioncall)
            tmp_ret.override (*assigned_functioncall_retval);
          o->newline() << "c->locals[c->nesting+1]." << c_funcname(r->name)
                       << ".__retvalue = &" << tmp_ret.value() << "[0];";
        }

      // call function
      o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
      o->newline() << c_funcname (r->name) << " (c);";
      o->newline() << "if (unlikely(c->last_error || c->aborted)) goto out;";

      if (!already_checked_action_count && !session->suppress_time_limits
          && !session->unoptimized)
        {
          max_action_info mai (*session);
          r->body->visit(&mai);
          // if an unoptimized function/probe called an optimized function, then
          // increase the counter, since the subtraction isn't done within an
          // optimized function
          if(mai.statement_count_finite())
            record_actions (mai.statement_count, e->tok, true);
        }

      if (r->type == pe_unknown || tmp_ret.is_overridden())
        // If we passed typechecking with pe_unknown, or if we directly assigned
        // the functioncall retval, then nothing will use this return value
        yield = false;
      else
        {
          if (!pointer_ret)
            {
              // overwrite the previous return value
              string value = "c->locals[c->nesting+1]." + c_funcname(r->name) + ".__retvalue";
              c_assign (tmp_ret.value(), value, e->type,
                        _("function return result evaluation"), e->tok);
            }
          yield = true;
        }

      if (e->referents.size() > 1 && r != e->referents.back())
        // branch to end of the enclosing statement-expression if one of the
        // function alternatives is selected
        o->newline() << "if (!c->next) goto fc_end_" << fc_counter << ";";
    }

  if (e->referents.size() > 1)
    {
      // end label and increment counter
      o->newline() << "fc_end_" << fc_counter++ << ":";
    }

  if (e->referents.back()->has_next)
    // check for aborted return from function; this could happen from non-overloaded ones too
    o->newline()
      << "if (unlikely(c->next)) { "
      << "c->last_stmt = " << lex_cast_qstring(*e->tok) << "; "
      << "c->last_error = \"all functions exhausted\"; goto out; }";

  // return result from retvalue slot NB: this must be last, for the
  // enclosing statement-expression ({ ... }) to carry this value.
  if (yield)
    o->newline() << tmp_ret.value() << ";";
  else
    o->newline() << "(void) 0;";
}


// returns true if it should print directly to a stream
static bool
preprocess_print_format(print_format* e, vector<tmpvar>& tmp,
                        vector<print_format::format_component>& components,
                        string& format_string)
{
  if (e->print_with_format)
    {
      format_string = e->raw_components;
      components = e->components;
    }
  else
    {
      string delim;
      if (e->print_with_delim)
	{
	  stringstream escaped_delim;
	  interned_string dstr = e->delimiter;
	  for (interned_string::const_iterator i = dstr.begin();
	       i != dstr.end(); ++i)
	    {
	      if (*i == '%')
		escaped_delim << '%';
	      escaped_delim << *i;
	    }
	  delim = escaped_delim.str();
	}

      // Synthesize a print-format string if the user didn't
      // provide one; the synthetic string simply contains one
      // directive for each argument.
      stringstream format;
      for (unsigned i = 0; i < e->args.size(); ++i)
	{
	  if (i > 0 && e->print_with_delim)
	    format << delim;
	  switch (e->args[i]->type)
	    {
	    default:
	    case pe_unknown:
	      throw SEMANTIC_ERROR(_("cannot print unknown expression type"), e->args[i]->tok);
	    case pe_stats:
	      throw SEMANTIC_ERROR(_("cannot print a raw stats object"), e->args[i]->tok);
	    case pe_long:
	      format << "%d";
	      break;
	    case pe_string:
	      format << "%s";
	      break;
	    }
	}
      if (e->print_with_newline)
	format << "\\n";

      format_string = format.str();
      components = print_format::string_to_components(format_string);
    }


  // optimize simple string prints
  if (e->print_to_stream && tmp.size() <= 1
      && format_string.find("%%") == string::npos)
    {
      // just a plain format string itself, or
      // simply formatting a string verbatim.
      if (tmp.empty() || format_string == "%s")
	return true;

      // just a string without formatting plus newline, and it's been
      // overridden with a literal, then we can token-paste the newline.
      // TODO could allow any prefix and suffix around "%s", C-escaped.
      if (tmp[0].is_overridden() && format_string == "%s\\n")
	{
	  tmp[0].override(tmp[0].value() + "\"\\n\"");
	  return true;
	}
    }

  return false;
}


void
c_unparser::visit_print_format (print_format* e)
{
  // Print formats can contain a general argument list *or* a special
  // type of argument which gets its own processing: a single,
  // non-format-string'ed, histogram-type stat_op expression.

  if (e->hist)
    {
      stmt_expr block(*this);
      aggvar agg = gensym_aggregate ();

      var *v = load_aggregate(e->hist->stat, agg);
      v->assert_hist_compatible(*e->hist);

      {
        // PR 2142+2610: empty aggregates
        o->newline() << "if (unlikely (" << agg.value() << " == NULL)"
                     << " || " <<  agg.value() << "->count == 0) {";
        o->newline(1) << "c->last_error = ";
        o->line() << STAP_T_06;
	o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
	o->newline() << "goto out;";
        o->newline(-1) << "} else";
        if (e->print_to_stream)
          {
            o->newline(1) << "_stp_stat_print_histogram (" << v->hist() << ", " << agg.value() << ");";
            o->indent(-1);
          }
        else
          {
            exp_type ty = pe_string;
            tmpvar res = gensym (ty);
            o->newline(1) << "_stp_stat_print_histogram_buf (" << res.value() << ", MAXSTRINGLEN, " << v->hist() << ", " << agg.value() << ");";
            o->newline(-1) << res.value() << ";";
          }
      }

      delete v;
    }
  else
    {
      stmt_expr block(*this);

      // PR10750: Enforce a reasonable limit on # of varargs
      // 32 varargs leads to max 256 bytes on the stack
      if (e->args.size() > 32)
        throw SEMANTIC_ERROR(_NF("additional argument to print", "too many arguments to print (%zu)",
                                e->args.size(), e->args.size()), e->tok);

      // Compute actual arguments
      vector<tmpvar> tmp;

      for (unsigned i=0; i<e->args.size(); i++)
	{
	  tmpvar t = gensym(e->args[i]->type);
	  c_assign (t, e->args[i],
		    "print format actual argument evaluation");
	  tmp.push_back(t);
	}

      // Allocate the result
      exp_type ty = e->print_to_stream ? pe_long : pe_string;
      tmpvar res = gensym (ty);

      // Munge so we can find our compiled printf
      vector<print_format::format_component> components;
      string format_string, format_string_out;
      bool use_print = preprocess_print_format(e, tmp, components, format_string);
      format_string_out = print_format::components_to_string(components);

      // Make the [s]printf call...

      // Generate code to check that any pointer arguments are actually accessible.
      size_t arg_ix = 0;
      for (unsigned i = 0; i < components.size(); ++i) {
	if (components[i].type == print_format::conv_literal)
	  continue;

	/* Take note of the width and precision arguments, if any.  */
	int width_ix = -1, prec_ix= -1;
	if (components[i].widthtype == print_format::width_dynamic)
	  width_ix = arg_ix++;
	if (components[i].prectype == print_format::prec_dynamic)
	  prec_ix = arg_ix++;

        (void) width_ix; /* XXX: notused */

        /* %m and %M need special care for digging into memory. */
	if (components[i].type == print_format::conv_memory
	    || components[i].type == print_format::conv_memory_hex)
	  {
	    string mem_size;
	    const token* prec_tok = e->tok;
	    if (prec_ix != -1)
	      {
		mem_size = tmp[prec_ix].value();
		prec_tok = e->args[prec_ix]->tok;
	      }
	    else if (components[i].prectype == print_format::prec_static &&
		     components[i].precision > 0)
	      mem_size = lex_cast(components[i].precision) + "LL";
	    else
	      mem_size = "1LL";

	    /* Limit how much can be printed at a time. (see also PR10490) */
	    o->newline() << "c->last_stmt = " << lex_cast_qstring(*prec_tok) << ";";
	    o->newline() << "if (" << mem_size << " > PAGE_SIZE) {";
	    o->newline(1) << "snprintf(c->error_buffer, sizeof(c->error_buffer), "
			  << "\"%lld is too many bytes for a memory dump\", (long long)"
			  << mem_size << ");";
	    o->newline() << "c->last_error = c->error_buffer;";
	    o->newline() << "goto out;";
	    o->newline(-1) << "}";
	  }

	++arg_ix;
      }

      // Shortcuts for cases that aren't formatted at all
      if (e->print_to_stream)
        {
	  if (e->print_char)
	    {
	      o->newline() << "_stp_print_char (";
	      if (tmp.size())
		o->line() << tmp[0].value() << ");";
	      else
		o->line() << '"' << format_string_out << "\");";
	      return;
	    }
	  if (use_print)
	    {
	      o->newline() << "_stp_print (";
	      if (tmp.size())
		o->line() << tmp[0].value() << ");";
	      else
		o->line() << '"' << format_string_out << "\");";
	      return;
	    }
	}

      // The default it to use the new compiled-printf, but one can fall back
      // to the old code with -DSTP_LEGACY_PRINT if desired.
      o->newline() << "#ifndef STP_LEGACY_PRINT";
      o->indent(1);

      // Copy all arguments to the compiled-printf's space, then call it
      const string& compiled_printf =
	get_compiled_printf (e->print_to_stream, format_string);
      for (unsigned i = 0; i < tmp.size(); ++i)
	o->newline() << "c->printf_locals." << compiled_printf
		     << ".arg" << i << " = " << tmp[i].value() << ";";
      if (e->print_to_stream)
	// We'll just hardcode the result of 0 instead of using the
	// temporary.
	res.override("((int64_t)0LL)");
      else
	o->newline() << "c->printf_locals." << compiled_printf
		     << ".__retvalue = " << res.value() << ";";
      o->newline() << compiled_printf << " (c);";

      o->newline(-1) << "#else // STP_LEGACY_PRINT";
      o->indent(1);

      // Generate the legacy call that goes through _stp_vsnprintf.
      if (e->print_to_stream)
	o->newline() << "_stp_printf (";
      else
	o->newline() << "_stp_snprintf (" << res.value() << ", MAXSTRINGLEN, ";
      o->line() << '"' << format_string_out << '"';

      // Make sure arguments match the expected type of the format specifier.
      arg_ix = 0;
      for (unsigned i = 0; i < components.size(); ++i)
	{
	  if (components[i].type == print_format::conv_literal)
	    continue;

	  /* Cast the width and precision arguments, if any, to 'int'.  */
	  if (components[i].widthtype == print_format::width_dynamic)
	    o->line() << ", (int)" << tmp[arg_ix++].value();
	  if (components[i].prectype == print_format::prec_dynamic)
	    o->line() << ", (int)" << tmp[arg_ix++].value();

	  /* The type of the %m argument is 'char*'.  */
	  if (components[i].type == print_format::conv_memory
	      || components[i].type == print_format::conv_memory_hex)
	    o->line() << ", (char*)(uintptr_t)" << tmp[arg_ix++].value();
	  /* The type of the %c argument is 'int'.  */
	  else if (components[i].type == print_format::conv_char)
	    o->line() << ", (int)" << tmp[arg_ix++].value();
	  else if (arg_ix < tmp.size())
	    o->line() << ", " << tmp[arg_ix++].value();
	}
      o->line() << ");";
      o->newline(-1) << "#endif // STP_LEGACY_PRINT";
      o->newline() << "if (unlikely(c->last_error || c->aborted)) goto out;";
      o->newline() << res.value() << ";";
    }
}

void
c_unparser::visit_stat_op (stat_op* e)
{
  // Stat ops can be *applied* to two types of expression:
  //
  //  1. An arrayindex expression on a pe_stats-valued array.
  //
  //  2. A symbol of type pe_stats.

  // FIXME: classify the expression the stat_op is being applied to,
  // call appropriate stp_get_stat() / stp_pmap_get_stat() helper,
  // then reach into resultant struct stat_data.

  // FIXME: also note that summarizing anything is expensive, and we
  // really ought to pass a timeout handler into the summary routine,
  // check its response, possibly exit if it ran out of cycles.

  {
    stmt_expr block(*this);
    aggvar agg = gensym_aggregate ();
    tmpvar res = gensym (pe_long);
    var *v = load_aggregate(e->stat, agg);
    {
      // PR 2142+2610: empty aggregates
      if ((e->ctype == sc_count) ||
          (e->ctype == sc_sum &&
           strverscmp(session->compatible.c_str(), "1.5") >= 0))
        {
          o->newline() << "if (unlikely (" << agg.value() << " == NULL))";
          o->indent(1);
          c_assign(res, "0", e->tok);
          o->indent(-1);
        }
      else
        {
          o->newline() << "if (unlikely (" << agg.value() << " == NULL)"
                       << " || " <<  agg.value() << "->count == 0) {";
          o->newline(1) << "c->last_error = ";
          o->line() << STAP_T_06;
          o->newline() << "c->last_stmt = " << lex_cast_qstring(*e->tok) << ";";
          o->newline() << "goto out;";
          o->newline(-1) << "}";
        }
      o->newline() << "else";
      o->indent(1);
      switch (e->ctype)
        {
        case sc_average:
          c_assign(res, ("_stp_div64(NULL, " + agg.value() + "->sum, "
                         + agg.value() + "->count)"),
                   e->tok);
          break;
        case sc_count:
          c_assign(res, agg.value() + "->count", e->tok);
          break;
        case sc_sum:
          c_assign(res, agg.value() + "->sum", e->tok);
          break;
        case sc_min:
          c_assign(res, agg.value() + "->min", e->tok);
          break;
        case sc_max:
          c_assign(res, agg.value() + "->max", e->tok);
          break;
        case sc_variance:
          c_assign(res, agg.value() + "->variance", e->tok);
          break;
        case sc_none:
          assert (0); // should not happen, as sc_none is only used in foreach sorts
        }
      o->indent(-1);
    }
    o->newline() << res << ";";
    delete v;
  }
}


void
c_unparser::visit_hist_op (hist_op*)
{
  // Hist ops can only occur in a limited set of circumstances:
  //
  //  1. Inside an arrayindex expression, as the base referent. See
  //     c_unparser::visit_arrayindex for handling of this case.
  //
  //  2. Inside a foreach statement, as the base referent. See
  //     c_unparser::visit_foreach_loop for handling this case.
  //
  //  3. Inside a print_format expression, as the sole argument. See
  //     c_unparser::visit_print_format for handling this case.
  //
  // Note that none of these cases involves the c_unparser ever
  // visiting this node. We should not get here.

  assert(false);
}


typedef map<Dwarf_Addr,const char*> addrmap_t; // NB: plain map, sorted by address

struct unwindsym_dump_context
{
  systemtap_session& session;
  ostream& output;
  unsigned stp_module_index;

  int build_id_len;
  unsigned char *build_id_bits;
  GElf_Addr build_id_vaddr;

  unsigned long stp_kretprobe_trampoline_addr;
  Dwarf_Addr stext_offset;

  vector<pair<string,unsigned> > seclist; // encountered relocation bases
                                          // (section names and sizes)
  map<unsigned, addrmap_t> addrmap; // per-relocation-base sorted addrmap

  void *debug_frame;
  size_t debug_len;
  void *debug_frame_hdr;
  size_t debug_frame_hdr_len;
  Dwarf_Addr debug_frame_off;
  void *eh_frame;
  void *eh_frame_hdr;
  size_t eh_len;
  size_t eh_frame_hdr_len;
  Dwarf_Addr eh_addr;
  Dwarf_Addr eh_frame_hdr_addr;
  void *debug_line;
  size_t debug_line_len;
  void *debug_line_str;
  size_t debug_line_str_len;

  set<string> undone_unwindsym_modules;
};

static bool need_byte_swap_for_target (const unsigned char e_ident[])
{
#if __BYTE_ORDER == __LITTLE_ENDIAN
  return (e_ident[EI_DATA] == ELFDATA2MSB);
#elif __BYTE_ORDER == __BIG_ENDIAN
  return (e_ident[EI_DATA] == ELFDATA2LSB);
#else
  #error Bad host __BYTE_ORDER
#endif
}

static void create_debug_frame_hdr (const unsigned char e_ident[],
				    Elf_Data *debug_frame,
				    void **debug_frame_hdr,
				    size_t *debug_frame_hdr_len,
				    Dwarf_Addr *debug_frame_off,
				    systemtap_session& session,
				    Dwfl_Module *mod)
{
  *debug_frame_hdr = NULL;
  *debug_frame_hdr_len = 0;

  int cies = 0;
  set< pair<Dwarf_Addr, Dwarf_Off> > fdes;
  set< pair<Dwarf_Addr, Dwarf_Off> >::iterator it;

  // In the .debug_frame the FDE encoding is always DW_EH_PE_absptr.
  // So there is no need to read the CIEs.  And the size is either 4
  // or 8, depending on the elf class from e_ident.
  int size = (e_ident[EI_CLASS] == ELFCLASS32) ? 4 : 8;
  bool need_byte_swap = need_byte_swap_for_target (e_ident);
#define host_to_target_64(x) (need_byte_swap ? bswap_64((x)) : (x))
#define host_to_target_32(x) (need_byte_swap ? bswap_32((x)) : (x))
#define target_to_host_64(x) (need_byte_swap ? bswap_64((x)) : (x))
#define target_to_host_32(x) (need_byte_swap ? bswap_32((x)) : (x))

  int res = 0;
  Dwarf_Off off = 0;
  Dwarf_CFI_Entry entry;

  while (res != 1)
    {
      Dwarf_Off next_off;
      res = dwarf_next_cfi (e_ident, debug_frame, false, off, &next_off,
			    &entry);
      if (res == 0)
	{
	  if (entry.CIE_id == DW_CIE_ID_64)
	    cies++; // We can just ignore the CIEs.
	  else
	    {
	      Dwarf_Addr addr;
	      if (size == 4)
		addr = target_to_host_32((*((uint32_t *) entry.fde.start)));
	      else
		addr = target_to_host_64((*((uint64_t *) entry.fde.start)));
	      fdes.insert(pair<Dwarf_Addr, Dwarf_Off>(addr, off));
	    }
	}
      else if (res > 0)
	; // Great, all done.
      else
	{
	  // Warn, but continue, backtracing will be slow...
          if (session.verbose > 2 && ! session.suppress_warnings)
	    {
	      const char *modname = dwfl_module_info (mod, NULL,
						      NULL, NULL, NULL,
						      NULL, NULL, NULL);
	      session.print_warning("Problem creating debug frame hdr for "
				    + lex_cast_qstring(modname)
				    + ", " + dwarf_errmsg (-1));
	    }
	  return;
	}
      off = next_off;
    }

  if (fdes.size() > 0)
    {
      it = fdes.begin();
      Dwarf_Addr first_addr = (*it).first;
      int res = dwfl_module_relocate_address (mod, &first_addr);
      DWFL_ASSERT ("create_debug_frame_hdr, dwfl_module_relocate_address",
		   res >= 0);
      *debug_frame_off = (*it).first - first_addr;
    }

  size_t total_size = 4 + (2 * size) + (2 * size * fdes.size());
  uint8_t *hdr = (uint8_t *) malloc(total_size);
  *debug_frame_hdr = hdr;
  *debug_frame_hdr_len = total_size;

  hdr[0] = 1; // version
  hdr[1] = DW_EH_PE_absptr; // ptr encoding
  hdr[2] = (size == 4) ? DW_EH_PE_udata4 : DW_EH_PE_udata8; // count encoding
  hdr[3] = DW_EH_PE_absptr; // table encoding
  if (size == 4)
    {
      uint32_t *table = (uint32_t *)(hdr + 4);
      *table++ = host_to_target_32 ((uint32_t) 0); // eh_frame_ptr, unused
      *table++ = host_to_target_32 ((uint32_t) fdes.size());
      for (it = fdes.begin(); it != fdes.end(); it++)
	{
	  *table++ = host_to_target_32 ((*it).first);
	  *table++ = host_to_target_32 ((*it).second);
	}
    }
  else
    {
      uint64_t *table = (uint64_t *)(hdr + 4);
      *table++ = host_to_target_64 ((uint64_t) 0); // eh_frame_ptr, unused
      *table++ = host_to_target_64 ((uint64_t) fdes.size());
      for (it = fdes.begin(); it != fdes.end(); it++)
	{
	  *table++ = host_to_target_64 ((*it).first);
	  *table++ = host_to_target_64 ((*it).second);
	}
    }
}

static set<string> vdso_paths;

// Get the .debug_frame end .eh_frame sections for the given module.
// Also returns the lenght of both sections when found, plus the section
// address (offset) of the eh_frame data. If a debug_frame is found, a
// synthesized debug_frame_hdr is also returned.
static void get_unwind_data (Dwfl_Module *m,
			     void **debug_frame, void **eh_frame,
			     size_t *debug_len, size_t *eh_len,
			     Dwarf_Addr *eh_addr,
			     void **eh_frame_hdr, size_t *eh_frame_hdr_len,
			     void **debug_frame_hdr,
			     size_t *debug_frame_hdr_len,
			     Dwarf_Addr *debug_frame_off,
			     Dwarf_Addr *eh_frame_hdr_addr,
			     systemtap_session& session)
{
  Dwarf_Addr start, bias = 0;
  GElf_Ehdr *ehdr, ehdr_mem;
  GElf_Shdr *shdr, shdr_mem;
  Elf_Scn *scn;
  Elf_Data *data = NULL;
  Elf *elf;

  // fetch .eh_frame info preferably from main elf file.
  dwfl_module_info (m, NULL, &start, NULL, NULL, NULL, NULL, NULL);
  elf = dwfl_module_getelf(m, &bias);
  ehdr = gelf_getehdr(elf, &ehdr_mem);

  scn = NULL;
  bool eh_frame_seen = false;
  bool eh_frame_hdr_seen = false;
  while ((scn = elf_nextscn(elf, scn)))
    {
      shdr = gelf_getshdr(scn, &shdr_mem);
      const char* scn_name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
      if (!eh_frame_seen
	  && strcmp(scn_name, ".eh_frame") == 0
	  && shdr->sh_type == SHT_PROGBITS)
	{
	  data = elf_rawdata(scn, NULL);
	  *eh_frame = data->d_buf;
	  *eh_len = data->d_size;
	  // For ".dynamic" sections we want the offset, not absolute addr.
	  // Note we don't trust dwfl_module_relocations() for ET_EXEC.
	  if (ehdr->e_type != ET_EXEC && dwfl_module_relocations (m) > 0)
	    *eh_addr = shdr->sh_addr - start + bias;
	  else
	    *eh_addr = shdr->sh_addr;
	  eh_frame_seen = true;
	}
      else if (!eh_frame_hdr_seen
	       && strcmp(scn_name, ".eh_frame_hdr") == 0
	       && shdr->sh_type == SHT_PROGBITS)
        {
          data = elf_rawdata(scn, NULL);
          *eh_frame_hdr = data->d_buf;
          *eh_frame_hdr_len = data->d_size;
          if (ehdr->e_type != ET_EXEC && dwfl_module_relocations (m) > 0)
	    *eh_frame_hdr_addr = shdr->sh_addr - start + bias;
	  else
	    *eh_frame_hdr_addr = shdr->sh_addr;
          eh_frame_hdr_seen = true;
        }
      if (eh_frame_seen && eh_frame_hdr_seen)
        break;
    }

  // fetch .debug_frame info preferably from dwarf debuginfo file.
  elf = (dwarf_getelf (dwfl_module_getdwarf (m, &bias))
	 ?: dwfl_module_getelf (m, &bias));
  ehdr = gelf_getehdr(elf, &ehdr_mem);
  scn = NULL;
  while ((scn = elf_nextscn(elf, scn)))
    {
      const char *sh_name;
      shdr = gelf_getshdr(scn, &shdr_mem);
      sh_name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
      // decompression is done via dwarf_begin_elf / global_read / check_section
      // / elf_compress_gnu / __libelf_decompress in libelf/elf_compress_gnu.c
      if (strcmp(sh_name, ".debug_frame") == 0
          || strcmp(sh_name, ".zdebug_frame") == 0)
	{
	  data = elf_rawdata(scn, NULL);
	  *debug_frame = data->d_buf;
	  *debug_len = data->d_size;
	  break;
	}
    }

  if (*debug_frame != NULL && *debug_len > 0)
    create_debug_frame_hdr (ehdr->e_ident, data,
			    debug_frame_hdr, debug_frame_hdr_len,
			    debug_frame_off, session, m);
}

static int
dump_build_id (Dwfl_Module *m,
	       unwindsym_dump_context *c,
	       const char *name, Dwarf_Addr)
{
  string modname = name;

  //extract build-id from debuginfo file
  int build_id_len = 0;
  unsigned char *build_id_bits;
  GElf_Addr build_id_vaddr;

  if ((build_id_len=dwfl_module_build_id(m,
                                        (const unsigned char **)&build_id_bits,
                                         &build_id_vaddr)) > 0)
  {
     if (modname != "kernel")
      {
        Dwarf_Addr reloc_vaddr = build_id_vaddr;
        const char *secname;
        int i;

        i = dwfl_module_relocate_address (m, &reloc_vaddr);
        DWFL_ASSERT ("dwfl_module_relocate_address reloc_vaddr", i >= 0);

        secname = dwfl_module_relocation_info (m, i, NULL);

        // assert same section name as in runtime/transport/symbols.c
        // NB: this is applicable only to module("...") probes.
        // process("...") ones may have relocation bases like '.dynamic',
        // and so we'll have to store not just a generic offset but
        // the relocation section/symbol name too: just like we do
        // for probe PC addresses themselves.  We want to set build_id_vaddr for
        // user modules even though they will not have a secname.

	if (modname[0] != '/')
	  if (!secname || strcmp(secname, ".note.gnu.build-id"))
	    throw SEMANTIC_ERROR (_("unexpected build-id reloc section ") +
				  string(secname ?: "null"));

        build_id_vaddr = reloc_vaddr;
      }

    if (c->session.verbose > 1)
      {
        clog << _F("Found build-id in %s, length %d, start at %#" PRIx64,
                   name, build_id_len, build_id_vaddr) << endl;
      }

    c->build_id_len = build_id_len;
    c->build_id_vaddr = build_id_vaddr;
    c->build_id_bits = build_id_bits;
  }

  return DWARF_CB_OK;
}

static int
dump_section_list (Dwfl_Module *m,
                   unwindsym_dump_context *c,
                   const char *name, Dwarf_Addr)
{
  // Depending on ELF section names normally means you are doing it WRONG.
  // Sadly it seems we do need it for the kernel modules. Which are ET_REL
  // files, which are "dynamically loaded" by the kernel. We keep a section
  // list for them to know which symbol corresponds to which section.
  //
  // Luckily for the kernel, normal executables (ET_EXEC) or shared
  // libraries (ET_DYN) we don't need it. We just have one "section",
  // which we will just give the arbitrary names "_stext", ".absolute"
  // or ".dynamic"

  string modname = name;

  // Use start and end as to calculate size for _stext, .dynamic and
  // .absolute sections.
  Dwarf_Addr start, end;
  dwfl_module_info (m, NULL, &start, &end, NULL, NULL, NULL, NULL);

  // Look up the relocation basis for symbols
  int n = dwfl_module_relocations (m);
  DWFL_ASSERT ("dwfl_module_relocations", n >= 0);

 if (n == 0)
    {
      // ET_EXEC, no relocations.
      string secname = ".absolute";
      unsigned size = end - start;
      c->seclist.push_back (make_pair (secname, size));
      return DWARF_CB_OK;
    }
  else if (n == 1)
    {
      // kernel or shared library (ET_DYN).
      string secname;
      secname = (modname == "kernel") ? "_stext" : ".dynamic";
      unsigned size = end - start;
      c->seclist.push_back (make_pair (secname, size));
      return DWARF_CB_OK;
    }
  else if (n > 1)
    {
      // ET_REL, kernel module.
      string secname;
      unsigned size;
      Dwarf_Addr bias;
      GElf_Ehdr *ehdr, ehdr_mem;
      GElf_Shdr *shdr, shdr_mem;
      Elf *elf = dwfl_module_getelf(m, &bias);
      ehdr = gelf_getehdr(elf, &ehdr_mem);
      Elf_Scn *scn = NULL;
      while ((scn = elf_nextscn(elf, scn)))
	{
	  // Just the "normal" sections with program bits please.
	  shdr = gelf_getshdr(scn, &shdr_mem);
	  if ((shdr->sh_type == SHT_PROGBITS || shdr->sh_type == SHT_NOBITS)
	      && (shdr->sh_flags & SHF_ALLOC))
	    {
	      size = shdr->sh_size;
	      const char* scn_name = elf_strptr(elf, ehdr->e_shstrndx,
						shdr->sh_name);
	      secname = scn_name;
	      c->seclist.push_back (make_pair (secname, size));
	    }
	}

      return DWARF_CB_OK;
    }

  // Impossible... dflw_assert above will have triggered.
  return DWARF_CB_ABORT;
}

static void find_debug_frame_offset (Dwfl_Module *m,
                                     unwindsym_dump_context *c)
{
  Dwarf_Addr start, bias = 0;
  GElf_Ehdr *ehdr, ehdr_mem;
  GElf_Shdr *shdr, shdr_mem;
  Elf_Scn *scn = NULL;
  Elf_Data *data = NULL;
  Elf *elf;

  dwfl_module_info (m, NULL, &start, NULL, NULL, NULL, NULL, NULL);

  // fetch .debug_frame info preferably from dwarf debuginfo file.
  elf = (dwarf_getelf (dwfl_module_getdwarf (m, &bias))
	 ?: dwfl_module_getelf (m, &bias));
  ehdr = gelf_getehdr(elf, &ehdr_mem);

  while ((scn = elf_nextscn(elf, scn)))
    {
      const char *sh_name;
      shdr = gelf_getshdr(scn, &shdr_mem);
      sh_name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
      // decompression is done via dwarf_begin_elf / global_read / check_section
      // / elf_compress_gnu / __libelf_decompress in libelf/elf_compress_gnu.c
      if (strcmp(sh_name, ".debug_frame") == 0
          || strcmp(sh_name, ".zdebug_frame") == 0)
	{
	  data = elf_rawdata(scn, NULL);
	  break;
	}
    }

  if (!data) // need this check since dwarf_next_cfi() doesn't do it
    return;

  // In the .debug_frame the FDE encoding is always DW_EH_PE_absptr.
  // So there is no need to read the CIEs.  And the size is either 4
  // or 8, depending on the elf class from e_ident.
  int size = (ehdr->e_ident[EI_CLASS] == ELFCLASS32) ? 4 : 8;
  int res = 0;
  Dwarf_Off off = 0;
  Dwarf_CFI_Entry entry;

  while (res != 1)
    {
      Dwarf_Off next_off;
      res = dwarf_next_cfi (ehdr->e_ident, data, false, off, &next_off, &entry);
      if (res == 0)
	{
	  if (entry.CIE_id != DW_CIE_ID_64) // ignore CIEs
	    {
	      Dwarf_Addr addr;
	      if (size == 4)
		addr = (*((uint32_t *) entry.fde.start));
	      else
		addr = (*((uint64_t *) entry.fde.start));
              Dwarf_Addr first_addr = addr;
              int res = dwfl_module_relocate_address (m, &first_addr);
              DWFL_ASSERT ("find_debug_frame_offset, dwfl_module_relocate_address",
                           res >= 0);
              c->debug_frame_off = addr - first_addr;
	    }
	}
      else if (res < 1)
        return;
      off = next_off;
    }
}

static int
dump_line_tables_check (void *data, size_t data_len)
{
  uint64_t unit_length = 0,  header_length = 0;
  uint16_t version = 0;
  uint8_t *ptr = (uint8_t *)data, *endunitptr, opcode_base = 0;
  unsigned length = 4;

  while (ptr < ((uint8_t *)data + data_len))
   {
      if (ptr + 4 > (uint8_t *)data + data_len)
        return DWARF_CB_ABORT;

      unit_length = *((uint32_t *) ptr);
      ptr += 4;
      if (unit_length == 0xffffffff)
        {
          if (ptr + 8 > (uint8_t *)data + data_len)
            return DWARF_CB_ABORT;
          length = 8;
          unit_length = *((uint64_t *) ptr);
          ptr += 8;
        }

      if ((ptr + unit_length > (uint8_t *)data + data_len) || unit_length <= 2)
        return DWARF_CB_ABORT;

      endunitptr = ptr + unit_length;

      version  = *((uint16_t *)ptr);
      ptr += 2;

      // Need to skip over DWARF 5's address_size and segment_selector_size right
      // to hdr_length (analogy to what happens in pass2's dump_line_tables_check()
      // PR29984
      if (version >= 5)
      {
        if (ptr + 2 > (uint8_t *)data + data_len)
            return DWARF_CB_ABORT;
        ptr += 2;
      }

      if (unit_length <= (2 + length))
        return DWARF_CB_ABORT;

      if (length == 4)
        {
          header_length = *((uint32_t *) ptr);
          ptr += 4;
        }
      else
        {
          header_length = *((uint64_t *) ptr);
          ptr += 8;
        }

      // safety check for the next few jumps
      if (header_length <= ((version >= 4 ? 5 : 4) + 2)
          || (unit_length - (2 + length) < header_length))
        return DWARF_CB_ABORT;

      // skip past min instr length, max ops per instr, and line base
      if (version >= 4)
        ptr += 3;
      else
        ptr += 2;

      // check that the line range is not 0
      if (*ptr == 0)
        return DWARF_CB_ABORT;
      ptr++;

      // check that the header accomodates the std opcode lens section
      opcode_base = *((uint8_t *) ptr);
      if (header_length <= (uint64_t) (opcode_base + (version >= 4 ? 7 : 6)))
        return DWARF_CB_ABORT;

      // the initial checks stop here, before the directory table
      ptr = endunitptr;
    }
  return DWARF_CB_OK;
}

static void
dump_line_tables (Dwfl_Module *m, unwindsym_dump_context *c,
                  const char *, Dwarf_Addr)
{
  Elf* elf;
  Elf_Scn* scn = NULL;
  Elf_Data* data;
  GElf_Ehdr *ehdr, ehdr_mem;
  GElf_Shdr* shdr, shdr_mem;
  Dwarf_Addr bias, start;

  dwfl_module_info (m, NULL, &start, NULL, NULL, NULL, NULL, NULL);

  elf = dwfl_module_getelf (m, &bias);
  if (elf == NULL)
    return;

  // we do not have the index for debug_line, so we can't use elf_getscn()
  // instead, we need to seach through the sections for the correct one as in
  // get_unwind_data()
  ehdr = gelf_getehdr(elf, &ehdr_mem);
  while ((scn = elf_nextscn(elf, scn)))
    {
      const char *sh_name;
      shdr = gelf_getshdr(scn, &shdr_mem);
      sh_name = elf_strptr(elf, ehdr->e_shstrndx, shdr->sh_name);
      // decompression is done via dwarf_begin_elf / global_read / check_section
      // / elf_compress_gnu / __libelf_decompress in libelf/elf_compress_gnu.c
      if (strcmp(sh_name, ".debug_line") == 0
          || strcmp(sh_name, ".zdebug_line") == 0)
        {
          data = elf_rawdata(scn, NULL);
          if (dump_line_tables_check(data->d_buf, data->d_size) == DWARF_CB_ABORT)
            return;
          c->debug_line = data->d_buf;
          c->debug_line_len = data->d_size;
          continue;
        }
       if (strcmp(sh_name, ".debug_line_str") == 0
           || strcmp(sh_name, ".zdebug_line_str") == 0)
        {
          data = elf_rawdata(scn, NULL);
          c->debug_line_str = data->d_buf;
          c->debug_line_str_len = data->d_size;
          continue;
        }
    }

  // still need to get some kind of information about the sec_load_offset for
  // kernel addresses if there is no unwind data
  if (c->debug_line_len > 0 && !c->session.need_unwind)
    find_debug_frame_offset (m, c);
}

/* Some architectures create special local symbols that are not
   interesting. */
static int
skippable_arch_symbol (GElf_Half e_machine, const char *name, GElf_Sym *sym)
{
  /* Filter out ARM mapping symbols */
  if ((e_machine == EM_ARM || e_machine == EM_AARCH64)
      && GELF_ST_TYPE (sym->st_info) == STT_NOTYPE
      && (! strcmp(name, "$a") || ! strcmp(name, "$t") || ! strcmp(name, "$x")
	  || ! strcmp(name, "$t.x") || ! strcmp(name, "$d")
	  || ! strcmp(name, "$v") || ! strcmp(name, "$d.realdata")))
    return 1;

  return 0;
}

static int
dump_symbol_tables (Dwfl_Module *m,
		    unwindsym_dump_context *c,
		    const char *modname, Dwarf_Addr base)
{
  // Use end as sanity check when resolving symbol addresses.
  Dwarf_Addr end;
  dwfl_module_info (m, NULL, NULL, &end, NULL, NULL, NULL, NULL);

  int syments = dwfl_module_getsymtab(m);
  if (syments < 0) // RHBZ1795196: elfutils 0.178+ can open vmlinuz as elf.main but fail here
    {
      c->session.print_warning(_F("libdwfl failure getting symbol table for %s: %s",
                                  modname, dwfl_errmsg(-1)));
      return DWARF_CB_ABORT;

      // signal to dump_unwindsyms() to not let things proceed all the way to
      // dump_unwindsym_cxt(), which then believes it has all the info for a
      // complete record about this module.  In the kernel's case, this allows
      // PR17921 fallback to /proc/kallsyms via dump_kallsyms().
    }

  // Look up the relocation basis for symbols
  int n = dwfl_module_relocations (m);
  DWFL_ASSERT ("dwfl_module_relocations", n >= 0);

  /* Needed on ppc64, for function descriptors. */
  Dwarf_Addr elf_bias;
  GElf_Ehdr *ehdr, ehdr_mem;
  Elf *elf;
  elf = dwfl_module_getelf(m, &elf_bias);
  ehdr = gelf_getehdr(elf, &ehdr_mem);

  // XXX: unfortunate duplication with tapsets.cxx:emit_address()

  // extra_offset is for the special kernel case.
  Dwarf_Addr extra_offset = 0;
  Dwarf_Addr kretprobe_trampoline_addr = (unsigned long) -1;
  int is_kernel = !strcmp(modname, "kernel");

  /* Set to bail early if we are just examining the kernel
     and don't need anything more. */
  int done = 0;
  for (int i = 0; i < syments && !done; ++i)
    {
      if (pending_interrupts)
        return DWARF_CB_ABORT;

      GElf_Sym sym;
      GElf_Word shndxp;

      const char *name = dwfl_module_getsym(m, i, &sym, &shndxp);
      if (name)
        {
          Dwarf_Addr sym_addr = sym.st_value;

	  // We always need two special values from the kernel.
	  // _stext for extra_offset and kretprobe_trampoline_holder
	  // for the unwinder.
          if (is_kernel)
	    {
	      // NB: Yey, we found the kernel's _stext value.
	      // Sess.sym_stext may be unset (0) at this point, since
	      // there may have been no kernel probes set.  We could
	      // use tapsets.cxx:lookup_symbol_address(), but then
	      // we're already iterating over the same data here...
	      if (! strcmp(name, KERNEL_RELOC_SYMBOL))
		{
		  int ki;
		  extra_offset = sym_addr;
		  ki = dwfl_module_relocate_address (m, &extra_offset);
		  DWFL_ASSERT ("dwfl_module_relocate_address extra_offset",
			       ki >= 0);

		  if (c->session.verbose > 2)
		    clog << _F("Found kernel _stext extra offset %#" PRIx64,
			       extra_offset) << endl;

		  if (! c->session.need_symbols
		      && (kretprobe_trampoline_addr != (unsigned long) -1
			  || ! c->session.need_unwind))
		    done = 1;
		}
	      else if (kretprobe_trampoline_addr == (unsigned long) -1
		       && c->session.need_unwind
		       && ! strcmp(name, "kretprobe_trampoline_holder"))
		{
		  int ki;
                  kretprobe_trampoline_addr = sym_addr;
                  ki = dwfl_module_relocate_address(m,
						    &kretprobe_trampoline_addr);
                  DWFL_ASSERT ("dwfl_module_relocate_address, kretprobe_trampoline_addr", ki >= 0);

		  if (! c->session.need_symbols
		      && extra_offset != 0)
		    done = 1;
		}
            }

	  // We are only interested in "real" symbols.
	  // We omit symbols that have suspicious addresses
	  // (before base, or after end).
          if (!done && c->session.need_symbols
	      && ! skippable_arch_symbol(ehdr->e_machine, name, &sym)
	      && (GELF_ST_TYPE (sym.st_info) == STT_FUNC
		  || (GELF_ST_TYPE (sym.st_info) == STT_NOTYPE
		      && (ehdr->e_type == ET_REL // PR10206 ppc fn-desc in .opd
			  || is_kernel)) // kernel entry functions are NOTYPE
		  || GELF_ST_TYPE (sym.st_info) == STT_OBJECT) // PR10000: .data
               && !(sym.st_shndx == SHN_UNDEF	// Value undefined,
		    || shndxp == (GElf_Word) -1	// in a non-allocated section,
		    || sym_addr >= end	// beyond current module,
		    || sym_addr < base))	// before first section.
            {
              const char *secname = NULL;
              unsigned secidx = 0; /* Most things have just one section. */
	      Dwarf_Addr func_desc_addr = 0; /* Function descriptor */

	      /* PPC64 uses function descriptors.
		 Note: for kernel ET_REL modules we rely on finding the
		 .function symbols instead of going through the opd function
		 descriptors. */
	      if (ehdr->e_machine == EM_PPC64
		  && GELF_ST_TYPE (sym.st_info) == STT_FUNC
		  && ehdr->e_type != ET_REL)
		{
		  Elf64_Addr opd_addr;
		  Dwarf_Addr opd_bias;
		  Elf_Scn *opd;

		  func_desc_addr = sym_addr;

		  opd = dwfl_module_address_section (m, &sym_addr, &opd_bias);
		  DWFL_ASSERT ("dwfl_module_address_section opd", opd != NULL);

		  Elf_Data *opd_data = elf_rawdata (opd, NULL);
		  assert(opd_data != NULL);

		  Elf_Data opd_in, opd_out;
		  opd_out.d_buf = &opd_addr;
		  opd_in.d_buf = (char *) opd_data->d_buf + sym_addr;
		  opd_out.d_size = opd_in.d_size = sizeof (Elf64_Addr);
		  opd_out.d_type = opd_in.d_type = ELF_T_ADDR;
		  if (elf64_xlatetom (&opd_out, &opd_in,
				      ehdr->e_ident[EI_DATA]) == NULL)
		    throw runtime_error ("elf64_xlatetom failed");

		  // So the real address of the function is...
		  sym_addr = opd_addr + opd_bias;
		}

              if (n > 0) // only try to relocate if there exist relocation bases
                {
                  int ki = dwfl_module_relocate_address (m, &sym_addr);
                  DWFL_ASSERT ("dwfl_module_relocate_address sym_addr", ki >= 0);
                  secname = dwfl_module_relocation_info (m, ki, NULL);

		  if (func_desc_addr != 0)
		    dwfl_module_relocate_address (m, &func_desc_addr);
		}

              if (n == 1 && is_kernel)
                {
                  // This is a symbol within a (possibly relocatable)
                  // kernel image.

		  // We only need the function symbols to identify kernel-mode
		  // PC's, so we omit undefined or "fake" absolute addresses.
		  // These fake absolute addresses occur in some older i386
		  // kernels to indicate they are vDSO symbols, not real
		  // functions in the kernel. We also omit symbols that have
                  if (GELF_ST_TYPE (sym.st_info) == STT_FUNC
		      && sym.st_shndx == SHN_ABS)
		    continue;

                  secname = "_stext";
                  // NB: don't subtract session.sym_stext, which could be
                  // inconveniently NULL. Instead, sym_addr will get
                  // compensated later via extra_offset.
                }
              else if (n > 0)
                {
                  assert (secname != NULL);
                  // secname adequately set

                  // NB: it may be an empty string for ET_DYN objects
                  // like shared libraries, as their relocation base
                  // is implicit.
                  if (secname[0] == '\0')
		    secname = ".dynamic";
		  else
		    {
		      // Compute our section number
		      for (secidx = 0; secidx < c->seclist.size(); secidx++)
			if (c->seclist[secidx].first==secname)
			  break;

		      if (secidx == c->seclist.size()) // PR23747 not an error
			{
                          continue; // way back to the next symbol
			}
		    }
                }
              else
                {
                  assert (n == 0);
                  // sym_addr is absolute, as it must be since there are
                  // no relocation bases
                  secname = ".absolute"; // sentinel
                }

              (c->addrmap[secidx])[sym_addr] = name;
	      /* If we have a function descriptor, register that address
	         under the same name */
	      if (func_desc_addr != 0)
		(c->addrmap[secidx])[func_desc_addr] = name;
            }
        }
    }

  if (is_kernel)
    {
      c->stext_offset = extra_offset;
      // Must be relative to actual kernel load address.
      if (kretprobe_trampoline_addr != (unsigned long) -1)
	c->stp_kretprobe_trampoline_addr = (kretprobe_trampoline_addr
					    - extra_offset);
    }

  return DWARF_CB_OK;
}

static int
dump_unwind_tables (Dwfl_Module *m,
		    unwindsym_dump_context *c,
		    const char *, Dwarf_Addr)
{
  // Add unwind data to be included if it exists for this module.
  get_unwind_data (m, &c->debug_frame, &c->eh_frame,
		   &c->debug_len, &c->eh_len,
		   &c->eh_addr, &c->eh_frame_hdr, &c->eh_frame_hdr_len,
		   &c->debug_frame_hdr, &c->debug_frame_hdr_len,
		   &c->debug_frame_off, &c->eh_frame_hdr_addr,
                   c->session);
  return DWARF_CB_OK;
}

static void
dump_unwindsym_cxt_table(systemtap_session& session, ostream& output,
			 const string& modname, unsigned modindex,
			 const string& secname, unsigned secindex,
			 const string& table, void*& data, size_t& len)
{
  if (len > MAX_UNWIND_TABLE_SIZE)
    {
      if (secname.empty())
	session.print_warning (_F("skipping module %s %s table (too big: %zi > %zi)",
				  modname.c_str(), table.c_str(),
				  len, (size_t)MAX_UNWIND_TABLE_SIZE));
      else
	session.print_warning (_F("skipping module %s, section %s %s table (too big: %zi > %zi)",
				  modname.c_str(), secname.c_str(), table.c_str(),
				  len, (size_t)MAX_UNWIND_TABLE_SIZE));
      data = NULL;
      len = 0;
      return;
    }

  // if it is the debug_line data, do not need the unwind flags to be defined
  if((table == "debug_line") || (table == "debug_line_str"))
    output << "#if defined(STP_NEED_LINE_DATA)\n";
  else
    output << "#if defined(STP_USE_DWARF_UNWINDER) && defined(STP_NEED_UNWIND_DATA)\n";
  output << "static uint8_t _stp_module_" << modindex << "_" << table;
  if (!secname.empty())
    output << "_" << secindex;
  output << "[] = \n";
  output << "  {";
  for (size_t i = 0; i < len; i++)
    {
      int h = ((uint8_t *)data)[i];
      output << h << ","; // decimal is less wordy than hex
      if ((i + 1) % 16 == 0)
	output << "\n" << "   ";
    }
  output << "};\n";
  if ((table == "debug_line") || (table == "debug_line_str"))
    output << "#endif /* STP_NEED_LINE_DATA */\n";
  else
    output << "#endif /* STP_USE_DWARF_UNWINDER && STP_NEED_UNWIND_DATA */\n";
}

static int
dump_unwindsym_cxt (Dwfl_Module *m,
		    unwindsym_dump_context *c,
		    const char *name, Dwarf_Addr base)
{
  string modname = name;
  unsigned stpmod_idx = c->stp_module_index;
  void *debug_frame = c->debug_frame;
  size_t debug_len = c->debug_len;
  void *debug_frame_hdr = c->debug_frame_hdr;
  size_t debug_frame_hdr_len = c->debug_frame_hdr_len;
  Dwarf_Addr debug_frame_off = c->debug_frame_off;
  void *eh_frame = c->eh_frame;
  void *eh_frame_hdr = c->eh_frame_hdr;
  size_t eh_len = c->eh_len;
  size_t eh_frame_hdr_len = c->eh_frame_hdr_len;
  Dwarf_Addr eh_addr = c->eh_addr;
  Dwarf_Addr eh_frame_hdr_addr = c->eh_frame_hdr_addr;
  void *debug_line = c->debug_line;
  size_t debug_line_len = c->debug_line_len;
  void *debug_line_str = c->debug_line_str;
  size_t debug_line_str_len = c->debug_line_str_len;

  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, "", 0,
			   "debug_frame", debug_frame, debug_len);

  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, "", 0,
			   "eh_frame", eh_frame, eh_len);

  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, "", 0,
			   "eh_frame_hdr", eh_frame_hdr, eh_frame_hdr_len);

  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, "", 0,
			   "debug_line", debug_line, debug_line_len);

  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, "", 0,
			   "debug_line_str", debug_line_str, debug_line_str_len);

  if (c->session.need_unwind && debug_frame == NULL && eh_frame == NULL)
    {
      // There would be only a small benefit to warning.  A user
      // likely can't do anything about this; backtraces for the
      // affected module would just get all icky heuristicy.
      // So only report in verbose mode.
      if (c->session.verbose > 2)
	c->session.print_warning ("No unwind data for " + modname
				  + ", " + dwfl_errmsg (-1));
    }

  if (c->session.need_lines && debug_line == NULL)
    {
      if (c->session.verbose > 2)
        c->session.print_warning ("No debug line data for " + modname + ", " +
                                  dwfl_errmsg (-1));
    }

  if (c->session.need_lines && debug_line_str == NULL)
    {
      if (c->session.verbose > 2)
        c->session.print_warning ("No debug line str data for " + modname + ", " +
                                  dwfl_errmsg (-1));
    }

  for (unsigned secidx = 0; secidx < c->seclist.size(); secidx++)
    {
      c->output << "static struct _stp_symbol "
                << "_stp_module_" << stpmod_idx<< "_symbols_" << secidx << "[] = {\n";

      string secname = c->seclist[secidx].first;
      Dwarf_Addr extra_offset;
      extra_offset = (secname == "_stext") ? c->stext_offset : 0;

      // Only include symbols if they will be used
      if (c->session.need_symbols)
	{
	  // We write out a *sorted* symbol table, so the runtime doesn't
	  // have to sort them later.
	  for (addrmap_t::iterator it = c->addrmap[secidx].begin();
	       it != c->addrmap[secidx].end(); it++)
	    {
	      // skip symbols that occur before our chosen base address
	      if (it->first < extra_offset)
		continue;

	      c->output << "  { 0x" << hex << it->first-extra_offset << dec
			<< ", " << lex_cast_qstring (it->second) << " },\n";
              // XXX: these literal strings all suffer ELF relocation bloat too.
              // See if the tapsets.cxx:dwarf_derived_probe_group::emit_module_decls
              // CALCIT hack could work here.
	    }
	}

      c->output << "};\n";

      /* For now output debug_frame index only in "magic" sections. */
      if (secname == ".dynamic" || secname == ".absolute"
	  || secname == ".text" || secname == "_stext")
	{
	  dump_unwindsym_cxt_table(c->session, c->output, modname, stpmod_idx, secname, secidx,
				   "debug_frame_hdr", debug_frame_hdr, debug_frame_hdr_len);
	}
    }

  c->output << "static struct _stp_section _stp_module_" << stpmod_idx<< "_sections[] = {\n";
  // For the kernel, executables (ET_EXEC) or shared libraries (ET_DYN)
  // there is just one section that covers the whole address space of
  // the module. For kernel modules (ET_REL) there can be multiple
  // sections that get relocated separately.
  for (unsigned secidx = 0; secidx < c->seclist.size(); secidx++)
    {
      c->output << "{\n"
                << ".name = " << lex_cast_qstring(c->seclist[secidx].first) << ",\n"
                << ".size = 0x" << hex << c->seclist[secidx].second << dec << ",\n"
                << ".symbols = _stp_module_" << stpmod_idx << "_symbols_" << secidx << ",\n"
                << ".num_symbols = " << c->addrmap[secidx].size() << ",\n";

      /* For now output debug_frame index only in "magic" sections. */
      string secname = c->seclist[secidx].first;
      if (debug_frame_hdr && (secname == ".dynamic" || secname == ".absolute"
			      || secname == ".text" || secname == "_stext"))
	{
	  c->output << "#if defined(STP_USE_DWARF_UNWINDER)"
		    << " && defined(STP_NEED_UNWIND_DATA)\n";

          c->output << ".debug_hdr = "
		    << "_stp_module_" << stpmod_idx
		    << "_debug_frame_hdr_" << secidx << ",\n";
          c->output << ".debug_hdr_len = " << debug_frame_hdr_len << ", \n";

	  Dwarf_Addr dwbias = 0;
	  dwfl_module_getdwarf (m, &dwbias);
	  c->output << ".sec_load_offset = 0x"
		    << hex << debug_frame_off - dwbias << dec << "\n";

	  c->output << "#else\n";
	  c->output << ".debug_hdr = NULL,\n";
	  c->output << ".debug_hdr_len = 0,\n";
	  c->output << ".sec_load_offset = 0\n";
	  c->output << "#endif /* STP_USE_DWARF_UNWINDER"
		    << " && STP_NEED_UNWIND_DATA */\n";

	}
      else
	{
	  c->output << ".debug_hdr = NULL,\n";
	  c->output << ".debug_hdr_len = 0,\n";
          if (c->session.need_lines && secname == ".text")
            {
              c->output << "#if defined(STP_NEED_LINE_DATA)\n";
              Dwarf_Addr dwbias = 0;
              dwfl_module_getdwarf (m, &dwbias);
              c->output << ".sec_load_offset = 0x"
                        << hex << debug_frame_off - dwbias << dec << "\n";
              c->output << "#else\n";
            }
	  c->output << ".sec_load_offset = 0\n";
          if (c->session.need_lines && secname == ".text")
            c->output << "#endif /* STP_NEED_LINE_DATA */\n";
	}

	c->output << "},\n";
    }
  c->output << "};\n";

  // Get the canonical path of the main file for comparison at runtime.
  // When given directly by the user through -d or in case of the kernel
  // name and path might differ. path should be used for matching.
  const char *mainfile;
  dwfl_module_info (m, NULL, NULL, NULL, NULL, NULL, &mainfile, NULL);

  // For user space modules store canonical path.
  // For kernel modules just the name itself.
  string mainpath = resolve_path(mainfile);
  string mainname;
  if (is_user_module(modname)) // userspace
    mainname = lex_cast_qstring (path_remove_sysroot(c->session,mainpath));
  else
    { // kernel module

      // If the module name is the full path to the ko, then we have to retrieve
      // the actual name by which the module will be known inside the kernel.
      // Otherwise, section relocations would be mismatched.
      if (is_fully_resolved(modname, c->session.sysroot, c->session.sysenv))
        mainname = lex_cast_qstring (modname_from_path(modname));
      else
        mainname = lex_cast_qstring (modname);
    }

  c->output << "static struct _stp_module _stp_module_" << stpmod_idx << " = {\n";
  c->output << ".name = " << mainname.c_str() << ",\n";
  c->output << ".path = " << lex_cast_qstring (path_remove_sysroot(c->session,mainpath)) << ",\n";
  c->output << ".eh_frame_addr = 0x" << hex << eh_addr << dec << ", \n";
  c->output << ".unwind_hdr_addr = 0x" << hex << eh_frame_hdr_addr
	    << dec << ", \n";

  if (debug_frame != NULL)
    {
      c->output << "#if defined(STP_USE_DWARF_UNWINDER) && defined(STP_NEED_UNWIND_DATA)\n";
      c->output << ".debug_frame = "
		<< "_stp_module_" << stpmod_idx << "_debug_frame, \n";
      c->output << ".debug_frame_len = " << debug_len << ", \n";
      c->output << "#else\n";
    }

  c->output << ".debug_frame = NULL,\n";
  c->output << ".debug_frame_len = 0,\n";

  if (debug_frame != NULL)
    c->output << "#endif /* STP_USE_DWARF_UNWINDER && STP_NEED_UNWIND_DATA*/\n";

  if (eh_frame != NULL)
    {
      c->output << "#if defined(STP_USE_DWARF_UNWINDER) && defined(STP_NEED_UNWIND_DATA)\n";
      c->output << ".eh_frame = "
		<< "_stp_module_" << stpmod_idx << "_eh_frame, \n";
      c->output << ".eh_frame_len = " << eh_len << ", \n";
      if (eh_frame_hdr)
        {
          c->output << ".unwind_hdr = "
                    << "_stp_module_" << stpmod_idx << "_eh_frame_hdr, \n";
          c->output << ".unwind_hdr_len = " << eh_frame_hdr_len << ", \n";
        }
      else
        {
          c->output << ".unwind_hdr = NULL,\n";
          c->output << ".unwind_hdr_len = 0,\n";
        }
      c->output << "#else\n";
    }

  c->output << ".eh_frame = NULL,\n";
  c->output << ".eh_frame_len = 0,\n";
  c->output << ".unwind_hdr = NULL,\n";
  c->output << ".unwind_hdr_len = 0,\n";
  if (eh_frame != NULL)
    c->output << "#endif /* STP_USE_DWARF_UNWINDER && STP_NEED_UNWIND_DATA*/\n";

  if (debug_line != NULL)
    {
      c->output << "#if defined(STP_NEED_LINE_DATA)\n";
      c->output << ".debug_line = "
		<< "_stp_module_" << stpmod_idx << "_debug_line, \n";
      c->output << ".debug_line_len = " << debug_line_len << ", \n";
      if (debug_line_str != NULL)
        {
          c->output << ".debug_line_str = "
                    << "_stp_module_" << stpmod_idx << "_debug_line_str, \n";
          c->output << ".debug_line_str_len = " << debug_line_str_len << ", \n";
        }
      c->output << "#else\n";
    }

  c->output << ".debug_line = NULL,\n";
  c->output << ".debug_line_len = 0,\n";
  c->output << ".debug_line_str = NULL,\n";
  c->output << ".debug_line_str_len = 0,\n";

  if (debug_line != NULL)
    c->output << "#endif /* STP_NEED_LINE_DATA */\n";

  c->output << ".sections = _stp_module_" << stpmod_idx << "_sections" << ",\n";
  c->output << ".num_sections = sizeof(_stp_module_" << stpmod_idx << "_sections)/"
            << "sizeof(struct _stp_section),\n";

  /* Don't save build-id if it is located before _stext.
   * This probably means that build-id will not be loaded at all and
   * happens for example with ARM kernel.  Allow user space modules since the
   * check fails for a shared object.
   *
   * See also:
   *    http://sourceware.org/ml/systemtap/2009-q4/msg00574.html
   */
  if (c->build_id_len > 0
      && (modname != "kernel" || (c->build_id_vaddr > base + c->stext_offset))) {
    c->output << ".build_id_bits = (unsigned char *)\"" ;
    for (int j=0; j<c->build_id_len;j++)
      c->output << "\\x" << hex
                << (unsigned short) *(c->build_id_bits+j) << dec;

    c->output << "\",\n";
    c->output << ".build_id_len = " << c->build_id_len << ",\n";

    /* XXX: kernel data boot-time relocation works differently from text.
       This hack assumes that offset between _stext and build id
       stays constant after relocation, but that's not necessarily
       correct either.  We may instead need a relocation basis different
       from _stext, such as __start_notes.  */
    if (modname == "kernel")
      c->output << ".build_id_offset = 0x" << hex << c->build_id_vaddr - (base + c->stext_offset)
                << dec << ",\n";
    // ET_DYN: task finder gives the load address. ET_EXEC: this is absolute address
    else
      c->output << ".build_id_offset = 0x" << hex
                << c->build_id_vaddr /* - base */
                << dec << ",\n";
  } else
    c->output << ".build_id_len = 0,\n";

  //initialize the note section representing unloaded
  c->output << ".notes_sect = 0,\n";

  c->output << "};\n\n";

  c->undone_unwindsym_modules.erase (modname);

  // release various malloc'd tables
  // if (eh_frame_hdr) free (eh_frame_hdr); -- nope, this one comes from the elf image in memory
  if (debug_frame_hdr) free (debug_frame_hdr);

  return DWARF_CB_OK;
}

static void dump_kallsyms(unwindsym_dump_context *c)
{
  ifstream kallsyms("/proc/kallsyms");
  unsigned stpmod_idx = c->stp_module_index;
  string line;
  unsigned size = 0;
  Dwarf_Addr start = 0;
  Dwarf_Addr end = 0;
  Dwarf_Addr prev = 0;

  c->output << "static struct _stp_symbol "
            << "_stp_module_" << stpmod_idx << "_symbols_" << 0 << "[] = {\n";

  while (getline(kallsyms, line))
    {
      Dwarf_Addr addr;
      string name;
      string module;
      char type;
      istringstream iss(line);

      iss >> hex >> addr >> type >> name >> module;

      if (name == KERNEL_RELOC_SYMBOL)
        start = addr;
      else if (name == "_end" || module != "")
        {
          end = prev;
          break;
        }

      if (!start || addr == 0 || prev == addr)
        continue;

      c->output << "  { 0x" << hex << addr - start << dec
			<< ", " << lex_cast_qstring(name) << " },\n";

      size++;
      prev = addr;
    }

  // PR30321 apply privilege separation for passes 2/3/4, esp. if invoked as root
  if ((getuid() != 0) && (size == 0))
    c->session.print_warning (_F("No kallsyms found.  Your uid=%d.", getuid()));

  c->output << "};\n";
  c->output << "static struct _stp_section _stp_module_" << stpmod_idx << "_sections[] = {\n";
  c->output << "{\n"
            << ".name = " << lex_cast_qstring(KERNEL_RELOC_SYMBOL) << ",\n"
            << ".size = 0x" << hex << end - start << dec << ",\n"
            << ".symbols = _stp_module_" << stpmod_idx << "_symbols_" << 0 << ",\n"
            << ".num_symbols = " << size << ",\n";
  c->output << "},\n";
  c->output << "};\n";
  c->output << "static struct _stp_module _stp_module_" << stpmod_idx << " = {\n";
  c->output << ".name = " << lex_cast_qstring("kernel") << ",\n";
  c->output << ".sections = _stp_module_" << stpmod_idx << "_sections" << ",\n";
  c->output << ".num_sections = sizeof(_stp_module_" << stpmod_idx << "_sections)/"
            << "sizeof(struct _stp_section),\n";
  c->output << "};\n\n";

  c->undone_unwindsym_modules.erase("kernel");
  c->stp_module_index++;
}

static int
dump_unwindsyms (Dwfl_Module *m,
                 void **userdata __attribute__ ((unused)),
                 const char *name,
                 Dwarf_Addr base,
                 void *arg)
{
  if (pending_interrupts)
    return DWARF_CB_ABORT;

  unwindsym_dump_context *c = (unwindsym_dump_context*) arg;
  assert (c);

  // skip modules/files we're not actually interested in
  string modname = name;
  if (c->session.unwindsym_modules.find(modname)
      == c->session.unwindsym_modules.end())
    return DWARF_CB_OK;

  if (c->session.verbose > 1)
    clog << "dump_unwindsyms " << name
         << " index=" << c->stp_module_index
         << " base=0x" << hex << base << dec << endl;

  // We want to extract several bits of information:
  //
  // - parts of the program-header that map the file's physical offsets to the text section
  // - section table: just a list of section (relocation) base addresses
  // - symbol table of the text-like sections, with all addresses relativized to each base
  // - the contents of .debug_frame and/or .eh_frame section, for unwinding purposes

  int res = DWARF_CB_OK;

  c->build_id_len = 0;
  c->build_id_vaddr = 0;
  c->build_id_bits = NULL;
  res = dump_build_id (m, c, name, base);

  c->seclist.clear();
  if (res == DWARF_CB_OK)
    res = dump_section_list(m, c, name, base);

  // We always need to check the symbols of the kernel if we use it,
  // for the extra_offset (also used for build_ids) and possibly
  // stp_kretprobe_trampoline_addr for the dwarf unwinder.
  c->addrmap.clear();
  if (res == DWARF_CB_OK
      && (c->session.need_symbols || ! strcmp(name, "kernel")))
    res = dump_symbol_tables (m, c, name, base);

  c->debug_frame = NULL;
  c->debug_len = 0;
  c->debug_frame_hdr = NULL;
  c->debug_frame_hdr_len = 0;
  c->debug_frame_off = 0;
  c->eh_frame = NULL;
  c->eh_frame_hdr = NULL;
  c->eh_len = 0;
  c->eh_frame_hdr_len = 0;
  c->eh_addr = 0;
  c->eh_frame_hdr_addr = 0;
  if (res == DWARF_CB_OK && c->session.need_unwind)
    res = dump_unwind_tables (m, c, name, base);

  c->debug_line = NULL;
  c->debug_line_len = 0;
  c->debug_line_str = NULL;
  c->debug_line_str_len = 0;
  if (res == DWARF_CB_OK && c->session.need_lines)
    // we dont set res = dump_line_tables() because unwindsym stuff should still
    // get dumped to the output even if gathering debug_line data fails
    (void) dump_line_tables (m, c, name, base);

  /* And finally dump everything collected in the output. */
  if (res == DWARF_CB_OK)
    res = dump_unwindsym_cxt (m, c, name, base);

  if (res == DWARF_CB_OK)
    c->stp_module_index++;

  return res;
}


// Emit symbol table & unwind data, plus any calls needed to register
// them with the runtime.
void emit_symbol_data_done (unwindsym_dump_context*, systemtap_session&);


void
add_unwindsym_iol_callback (set<string> *added, const char *data)
{
  added->insert (string (data));
}


static int
query_module (Dwfl_Module *mod,
              void **,
              const char *,
              Dwarf_Addr,
              struct dwflpp *dwflpp)
{
  dwflpp->focus_on_module(mod, NULL);
  return DWARF_CB_OK;
}


void
add_unwindsym_ldd (systemtap_session &s)
{
  std::set<std::string> added;

  for (std::set<std::string>::iterator it = s.unwindsym_modules.begin();
       it != s.unwindsym_modules.end();
       it++)
    {
      string modname = *it;
      assert (modname.length() != 0);
      if (! is_user_module (modname)) continue;

      dwflpp mod_dwflpp (s, modname, false);
      mod_dwflpp.iterate_over_modules(&query_module, &mod_dwflpp);
      if (mod_dwflpp.module) // existing binary
        {
          assert (mod_dwflpp.module_name != "");
          mod_dwflpp.iterate_over_libraries (&add_unwindsym_iol_callback, &added);
        }
    }

  s.unwindsym_modules.insert (added.begin(), added.end());
}

static int find_vdso(const char *path, const struct stat *, int type)
{
  if (type == FTW_F)
    {
      /* Assume that if the path's basename starts with 'vdso' and
       * ends with '.so', it is the vdso.
       *
       * Note that this logic should match up with the logic in the
       * _stp_vma_match_vdso() function in runtime/vma.c. */
      const char *name = strrchr(path, '/');
      if (name)
	{
	  const char *ext;

	  name++;
	  ext = strrchr(name, '.');
	  if (ext
	      && strncmp("vdso", name, 4) == 0
	      && strcmp(".so", ext) == 0)
	    vdso_paths.insert(path);
	}
    }
  return 0;
}

void
add_unwindsym_vdso (systemtap_session &s)
{
  // This is to disambiguate between -r REVISION vs -r BUILDDIR.
  // See also dwflsetup.c (setup_dwfl_kernel). In case of only
  // having the BUILDDIR we need to do a deep search (the specific
  // arch name dir in the kernel build tree is unknown).
  string vdso_dir;
  if (s.kernel_build_tree == string(s.sysroot + "/lib/modules/"
				    + s.kernel_release
				    + "/build"))
    vdso_dir = s.sysroot + "/lib/modules/" + s.kernel_release + "/vdso";
  else
    vdso_dir = s.kernel_build_tree + "/arch/";

  if (s.verbose > 1)
    clog << _("Searching for vdso candidates: ") << vdso_dir << endl;

  ftw(vdso_dir.c_str(), find_vdso, 1);

  for (set<string>::iterator it = vdso_paths.begin();
       it != vdso_paths.end();
       it++)
    {
      s.unwindsym_modules.insert(*it);
      if (s.verbose > 1)
	clog << _("vdso candidate: ") << *it << endl;
    }
}

static void
prepare_symbol_data (systemtap_session& s)
{
  // step 0: run ldd on any user modules if requested
  if (s.unwindsym_ldd)
    add_unwindsym_ldd (s);
  // step 0.5: add vdso(s) when vma tracker was requested
  if (vma_tracker_enabled (s))
    add_unwindsym_vdso (s);
  // NB: do this before the ctx.unwindsym_modules copy is taken
}

void
emit_symbol_data (systemtap_session& s)
{
  ofstream kallsyms_out (s.symbols_source.c_str ());

  if (s.runtime_usermode_p ())
    {
      kallsyms_out << "#include \"stap_common.h\"\n"
        "#include <sym.h>\n";
    }
  else
    {
      kallsyms_out << "#include <linux/module.h>\n"
        "#include <linux/kernel.h>\n"
        "#include <sym.h>\n"
        "#include \"stap_common.h\"\n";
    }

  vector<pair<string,unsigned> > seclist;
  map<unsigned, addrmap_t> addrmap;
  unwindsym_dump_context ctx = { s, kallsyms_out,
				 0, /* module index */
				 0, NULL, 0, /* build_id len, bits, vaddr */
				 ~0UL, /* stp_kretprobe_trampoline_addr */
				 0, /* stext_offset */
				 seclist, addrmap,
				 NULL, /* debug_frame */
				 0, /* debug_len */
				 NULL, /* debug_frame_hdr */
				 0, /* debug_frame_hdr_len */
				 0, /* debug_frame_off */
				 NULL, /* eh_frame */
				 NULL, /* eh_frame_hdr */
				 0, /* eh_len */
				 0, /* eh_frame_hdr_len */
				 0, /* eh_addr */
				 0, /* eh_frame_hdr_addr */
				 NULL, /* debug_line */
				 0, /* debug_line_len */
				 NULL, /* debug_line_str */
				 0, /* debug_line_str_len */
				 s.unwindsym_modules };

  // Micro optimization, mainly to speed up tiny regression tests
  // using just begin probe.
  if (s.unwindsym_modules.size () == 0)
    {
      emit_symbol_data_done(&ctx, s);
      return;
    }

  // ---- step 1: process any kernel modules listed
  set<string> offline_search_modules;
  unsigned count;
  for (set<string>::iterator it = s.unwindsym_modules.begin();
       it != s.unwindsym_modules.end();
       it++)
    {
      string foo = *it;
      if (! is_user_module (foo)) /* Omit user-space, since we're only
				     using this for kernel space
				     offline searches. */
        offline_search_modules.insert (foo);
    }
  Dwfl *dwfl = setup_dwfl_kernel (offline_search_modules, &count, s);
  /* NB: It's not an error to find a few fewer modules than requested.
     There might be third-party modules loaded (e.g. uprobes). */
  /* DWFL_ASSERT("all kernel modules found",
     count >= offline_search_modules.size()); */

  ptrdiff_t off = 0;
  do
    {
      assert_no_interrupts();
      if (ctx.undone_unwindsym_modules.empty()) break;
      off = dwfl_getmodules (dwfl, &dump_unwindsyms, (void *) &ctx, off);
    }
  while (off > 0);
  DWFL_ASSERT("dwfl_getmodules", off == 0);
  dwfl_end(dwfl);

  // ---- step 2: process any user modules (files) listed
  for (std::set<std::string>::iterator it = s.unwindsym_modules.begin();
       it != s.unwindsym_modules.end();
       it++)
    {
      string modname = *it;
      assert (modname.length() != 0);
      if (! is_user_module (modname)) continue;
      Dwfl *dwfl = setup_dwfl_user (modname);
      if (dwfl != NULL) // tolerate missing data; will warn below
        {
          ptrdiff_t off = 0;
          do
            {
              assert_no_interrupts();
              if (ctx.undone_unwindsym_modules.empty()) break;
              off = dwfl_getmodules (dwfl, &dump_unwindsyms, (void *) &ctx, off);
            }
          while (off > 0);
          DWFL_ASSERT("dwfl_getmodules", off == 0);
        }
      dwfl_end(dwfl);
    }

  // Use /proc/kallsyms if debuginfo not found.
  if (ctx.undone_unwindsym_modules.find("kernel") != ctx.undone_unwindsym_modules.end())
    dump_kallsyms(&ctx);

  emit_symbol_data_done (&ctx, s);
}

void
self_unwind_declarations(unwindsym_dump_context *ctx)
{
  ctx->output << "static uint8_t _stp_module_self_eh_frame [] = {0,};\n";
  ctx->output << "struct _stp_symbol _stp_module_self_symbols_0[] = {{0},};\n";
  ctx->output << "struct _stp_symbol _stp_module_self_symbols_1[] = {{0},};\n";
  ctx->output << "struct _stp_section _stp_module_self_sections[] = {\n";
  ctx->output << "{.name = \".symtab\", .symbols = _stp_module_self_symbols_0, .num_symbols = 0},\n";
  ctx->output << "{.name = \".text\", .symbols = _stp_module_self_symbols_1, .num_symbols = 0},\n";
  ctx->output << "};\n";
  ctx->output << "struct _stp_module _stp_module_self = {\n";
  ctx->output << ".name = \"stap_self_tmp_value\",\n";
  ctx->output << ".path = \"stap_self_tmp_value\",\n";
  ctx->output << ".num_sections = 2,\n";
  ctx->output << ".sections = _stp_module_self_sections,\n";
  ctx->output << ".eh_frame = _stp_module_self_eh_frame,\n";
  ctx->output << ".eh_frame_len = 0,\n";
  ctx->output << ".unwind_hdr_addr = 0x0,\n";
  ctx->output << ".unwind_hdr = NULL,\n";
  ctx->output << ".unwind_hdr_len = 0,\n";
  ctx->output << ".debug_frame = NULL,\n";
  ctx->output << ".debug_frame_len = 0,\n";
  ctx->output << ".debug_line = NULL,\n";
  ctx->output << ".debug_line_len = 0,\n";
  ctx->output << ".debug_line_str = NULL,\n";
  ctx->output << ".debug_line_str_len = 0,\n";
  ctx->output << "};\n";
}

void
emit_symbol_data_done (unwindsym_dump_context *ctx, systemtap_session& s)
{
  // Add a .eh_frame terminator dummy object file, much like
  // libgcc/crtstuff.c's EH_FRAME_SECTION_NAME closer.  We need this in
  // order for runtime/sym.c 
  translator_output *T_800 = s.op_create_auxiliary(true);
  T_800->newline() << "__extension__ unsigned int T_800 []"; // assumed 32-bits wide
  T_800->newline(1) << "__attribute__((used, section(\".eh_frame\"), aligned(4)))";
  T_800->newline() << "= { 0 };";
  T_800->newline(-1);
  T_800->assert_0_indent (); // flush to disk

  // Print out a definition of the runtime's _stp_modules[] globals.
  ctx->output << "\n";
  self_unwind_declarations(ctx);
   ctx->output << "struct _stp_module *_stp_modules [] = {\n";
  for (unsigned i=0; i<ctx->stp_module_index; i++)
    {
      ctx->output << "& _stp_module_" << i << ",\n";
    }
  ctx->output << "& _stp_module_self,\n";
  ctx->output << "};\n";
  ctx->output << "const unsigned _stp_num_modules = ARRAY_SIZE(_stp_modules);\n";

  ctx->output << "unsigned long _stp_kretprobe_trampoline = ";
  // Special case for -1, which is invalid in hex if host width > target width.
  if (ctx->stp_kretprobe_trampoline_addr == (unsigned long) -1)
    ctx->output << "-1;\n";
  else
    ctx->output << "0x" << hex << ctx->stp_kretprobe_trampoline_addr << dec
		<< ";\n";

  // Some nonexistent modules may have been identified with "-d".  Note them.
  if (! s.suppress_warnings)
    for (set<string>::iterator it = ctx->undone_unwindsym_modules.begin();
	 it != ctx->undone_unwindsym_modules.end();
	 it ++)
      s.print_warning (_("missing unwind/symbol data for module '")
		       + (*it) + "'");
}

struct recursion_info: public traversing_visitor
{
  recursion_info (systemtap_session& s): sess(s), nesting_max(0), recursive(false) {}
  systemtap_session& sess;
  unsigned nesting_max;
  bool recursive;
  std::vector <functiondecl *> current_nesting;

  void visit_functioncall (functioncall* e) {
    traversing_visitor::visit_functioncall (e); // for arguments

    for (unsigned fd = 0; fd < e->referents.size(); fd++)
      {
        functiondecl* referent = e->referents[fd];
        // check for nesting level
        unsigned nesting_depth = current_nesting.size() + 1;
        if (nesting_max < nesting_depth)
          {
            if (sess.verbose > 3)
              clog << _F("identified max-nested function: %s (%d)",
                         referent->name.to_string().c_str(), nesting_depth) << endl;
            nesting_max = nesting_depth;
          }

        // check for (direct or mutual) recursion
        for (unsigned j=0; j<current_nesting.size(); j++)
          if (current_nesting[j] == referent)
            {
              recursive = true;
              if (sess.verbose > 3)
                clog << _F("identified recursive function: %s",
                           referent->name.to_string().c_str()) << endl;
              return;
            }

        // non-recursive traversal
        current_nesting.push_back (referent);
        referent->body->visit (this);
        current_nesting.pop_back ();
      }
  }
};


void translate_runtime(systemtap_session& s)
{
  s.op->newline() << "#define STAP_MSG_RUNTIME_H_01 "
                  << lex_cast_qstring(_("myproc-unprivileged tapset function called "
                                        "without is_myproc checking for pid %d (euid %d)"));

  s.op->newline() << "#define STAP_MSG_LOC2C_01 "
                  << lex_cast_qstring(_("read fault [man error::fault] at 0x%lx"));
  s.op->newline() << "#define STAP_MSG_LOC2C_02 "
                  << lex_cast_qstring(_("write fault [man error::fault] at 0x%lx"));
  s.op->newline() << "#define STAP_MSG_LOC2C_03 "
                  << lex_cast_qstring(_("divide by zero in DWARF operand (%s)"));
  s.op->newline() << "#define STAP_MSG_LOC2C_04 "
                  << lex_cast_qstring(_("register access fault [man error::fault]"));
}


int
prepare_translate_pass (systemtap_session& s)
{
  int rc = 0;
  try
    {
      prepare_symbol_data (s);
    }
  catch (const semantic_error& e)
    {
      s.print_error (e);
      rc = 1;
    }

  return rc;
}


int
translate_pass (systemtap_session& s)
{
  int rc = 0;
  string comm_hdr_file = s.tmpdir + "/stap_common.h";

  s.op = new translator_output (s.translated_source);
  s.op->new_common_header (comm_hdr_file);

  // additional outputs might be found in s.auxiliary_outputs
  c_unparser cup (& s);
  s.up = & cup;
  translate_runtime(s);

  try
    {
      int64_t major=0, minor=0;
      try
	{
	  vector<string> versions;
	  tokenize (s.compatible, versions, ".");
	  if (versions.size() >= 1)
	    major = lex_cast<int64_t> (versions[0]);
	  if (versions.size() >= 2)
	    minor = lex_cast<int64_t> (versions[1]);
	  if (versions.size() >= 3 && s.verbose > 1)
	    clog << _F("ignoring extra parts of compat version: %s", s.compatible.c_str()) << endl;
	}
      catch (const runtime_error&)
	{
	  throw SEMANTIC_ERROR(_F("parse error in compatibility version: %s", s.compatible.c_str()));
	}
      if (major < 0 || major > 255 || minor < 0 || minor > 255)
	throw SEMANTIC_ERROR(_F("compatibility version out of range: %s", s.compatible.c_str()));
      s.op->newline() << "#define STAP_VERSION(a, b) ( ((a) << 8) + (b) )";
      s.op->newline() << "#ifndef STAP_COMPAT_VERSION";
      s.op->newline() << "#define STAP_COMPAT_VERSION STAP_VERSION("
		      << major << ", " << minor << ")";
      s.op->newline() << "#endif";

      // Some of our generated C code can trigger this harmless diagnostic.
      s.op->newline() << "#pragma GCC diagnostic ignored \"-Wtautological-compare\"";

      recursion_info ri (s);

      // NB: we start our traversal from the s.functions[] rather than the probes.
      // We assume that each function is called at least once, or else it would have
      // been elided already.
      for (map<string,functiondecl*>::iterator it = s.functions.begin(); it != s.functions.end(); it++)
	{
          functiondecl *fd = it->second;
          fd->body->visit (& ri);
	}

      if (s.verbose > 1)
        clog << _F("function recursion-analysis: max-nesting %d %s", ri.nesting_max,
                  (ri.recursive ? _(" recursive") : _(" non-recursive"))) << endl;
      unsigned nesting = ri.nesting_max + 1; /* to account for initial probe->function call */
      if (ri.recursive) nesting += 10;

      // This is at the very top of the file.
      // All "static" defines (not dependend on session state).
      s.op->newline() << "#include \"runtime_defines.h\"";
      if (s.perf_derived_probes)
	s.op->newline() << "#define _HAVE_PERF_ 1";
      s.op->newline() << "#include \"linux/perf_read.h\"";

      // Generated macros describing the privilege level required to load/run this module.
      s.op->newline() << "#define STP_PR_STAPUSR 0x" << hex << pr_stapusr << dec;
      s.op->newline() << "#define STP_PR_STAPSYS 0x" << hex << pr_stapsys << dec;
      s.op->newline() << "#define STP_PR_STAPDEV 0x" << hex << pr_stapdev << dec;
      s.op->newline() << "#define STP_PRIVILEGE 0x" << hex << s.privilege << dec;

      // Generate a section containing a mask of the privilege levels required to load/run this
      // module.
      s.op->newline() << "int stp_required_privilege "
		      << "__attribute__ ((section (\"" << STAP_PRIVILEGE_SECTION <<"\")))"
		      << " = STP_PRIVILEGE;";

      s.op->newline() << "#include \"stap_common.h\"";

      if (s.runtime_usermode_p ())
        {
          s.op->hdr->line() << "#include <stdint.h>";
          s.op->hdr->newline() << "#include <stddef.h>";
          s.op->hdr->newline() << "struct task_struct;";
          s.op->hdr->newline() << "#define __must_be_array(arr) 0";
          s.op->hdr->newline() << "#define ARRAY_SIZE(arr) (sizeof(arr) "
            "/ sizeof((arr)[0]) + __must_be_array(arr))";
        }

      s.op->hdr->newline() << "#ifndef MAXNESTING";
      s.op->hdr->newline() << "#define MAXNESTING " << nesting;
      s.op->hdr->newline() << "#endif";

      // Generated macros specifying how much storage is required for
      // regexp subexpressions. (TODOXXX Skip when there are no DFAs?)
      s.op->hdr->newline() << "#define STAPREGEX_MAX_MAP " << s.dfa_maxmap;
      s.op->hdr->newline() << "#define STAPREGEX_MAX_TAG " << s.dfa_maxtag;

      s.op->hdr->newline() << "#define STP_SKIP_BADVARS " << (s.skip_badvars ? 1 : 0);

      if (s.bulk_mode)
	  s.op->hdr->newline() << "#define STP_BULKMODE";

      if (s.timing || s.monitor)
	s.op->hdr->newline() << "#define STP_TIMING";
      if (!isatty(STDOUT_FILENO))
        {
          s.op->hdr->newline() << "#ifndef STP_FORCE_STDOUT_TTY";
          s.op->hdr->newline() << "#define STP_STDOUT_NOT_ATTY";
          s.op->hdr->newline() << "#endif";
        }

      if (s.need_unwind)
	s.op->hdr->newline() << "#define STP_NEED_UNWIND_DATA 1";

      if (s.need_lines)
        s.op->hdr->newline() << "#define STP_NEED_LINE_DATA 1";

      // Emit the total number of probes (not regarding merged probe handlers)
      s.op->hdr->newline() << "#define STP_PROBE_COUNT " << s.probes.size();

      s.op->hdr->newline() << "#if (defined(__arm__) || defined(__i386__) "
        "|| defined(__x86_64__) || defined(__powerpc64__)) "
        "|| defined (__s390x__) || defined(__aarch64__) || defined(__mips__)\n"
        "#ifdef STP_NEED_UNWIND_DATA\n"
        "#ifndef STP_USE_DWARF_UNWINDER\n"
        "#define STP_USE_DWARF_UNWINDER\n"
        "#endif\n"
        "#endif\n"
        "#endif";

      s.op->hdr->close ();

      // Emit systemtap_module_refresh() prototype so we can reference it
      s.op->newline() << "static void systemtap_module_refresh (const char* modname);";

      // Be sure to include runtime.h before any real code.
      s.op->newline() << "#include \"runtime.h\"";

      if (!s.runtime_usermode_p())
        {
          // When on-the-fly [dis]arming is used, module_refresh can be called from
          // both the module notifier, as well as when probes need to be
          // armed/disarmed. We need to protect it to ensure it's only run one at a
          // time.
          s.op->newline() << "#include <linux/mutex.h>";
          s.op->newline() << "static DEFINE_MUTEX(module_refresh_mutex);";

          // For some probes, on-the-fly support is provided through a
          // background timer (module_refresh_timer). We need to disable that
          // part if hrtimers are not supported.
          s.op->newline() << "#include <linux/version.h>";
          s.op->newline() << "#define STP_ON_THE_FLY_TIMER_ENABLE";
        }

      // Emit embeds ahead of time, in case they affect context layout
      for (unsigned i=0; i<s.embeds.size(); i++)
        {
          s.op->newline() << s.embeds[i]->code << "\n";
        }

      s.up->emit_common_header (); // context etc.

      if (s.need_unwind)
	s.op->newline() << "#include \"stack.c\"";

      s.op->newline() << "#include \"sym2.c\"";

      if (s.globals.size()>0)
	{
	  s.op->newline() << "struct stp_globals {";
	  s.op->indent(1);
	  for (unsigned i=0; i<s.globals.size(); i++)
	    {
	      s.up->emit_global (s.globals[i]);
	    }
	  s.op->newline(-1) << "};";

	  // We only need to statically initialize globals in kernel modules,
	  // where module parameters may want to override the script's value.  In
	  // stapdyn, the globals are actually part of the dynamic shared memory,
	  // and the static structure is merely used as a source of default values.
	  s.op->newline();
	  if (!s.runtime_usermode_p ())
	    s.op->newline() << "static struct stp_globals stp_global = {";
	  else
	   {
	     s.op->newline() << "static struct {";
	     s.op->indent(1);
	     for (unsigned i=0; i<s.globals.size(); i++)
	       {
		 assert_no_interrupts();
                 s.up->emit_global_init_type (s.globals[i]);
	       }
	     s.op->newline(-1) << "} stp_global_init = {";
	   }
	  s.op->newline(1);
	  for (unsigned i=0; i<s.globals.size(); i++)
	    {
	      assert_no_interrupts();
              s.up->emit_global_init (s.globals[i]);
	    }
	  s.op->newline(-1) << "};";

	  s.op->assert_0_indent();
	}
      else
        // stp_runtime_session wants to incorporate globals, but it
        // can be empty
	s.op->newline() << "struct stp_globals {};";

      // Common (static atomic) state of the stap session.
      s.op->newline();
      s.op->newline() << "#include \"common_session_state.h\"";

      s.op->newline() << "#include \"probe_lock.h\" ";

      s.op->newline() << "#ifdef STAP_NEED_GETTIMEOFDAY";
      s.op->newline() << "#include \"time.c\"";  // Don't we all need more?
      s.op->newline() << "#endif";

      for (map<string,stapdfa*>::iterator it = s.dfas.begin(); it != s.dfas.end(); it++)
        {
          assert_no_interrupts();
          s.op->newline();
          try
            {
              it->second->emit_declaration (s.op);
            }
          catch (const semantic_error &e)
            {
              s.print_error(e);
            }
        }
      s.op->assert_0_indent();

      for (map<string,functiondecl*>::iterator it = s.functions.begin(); it != s.functions.end(); it++)
	{
          assert_no_interrupts();
	  s.op->newline();
	  s.up->emit_functionsig (it->second);
	}
      s.op->assert_0_indent();


      // Let's find some stats for the embedded pp strings.  Maybe they
      // are small and uniform enough to justify putting char[MAX]'s into
      // the array instead of relocated char*'s.
      size_t pp_max = 0, pn_max = 0, location_max = 0, derivation_max = 0;
      size_t pp_tot = 0, pn_tot = 0, location_tot = 0, derivation_tot = 0;
      for (unsigned i=0; i<s.probes.size(); i++)
        {
          derived_probe* p = s.probes[i];
#define DOIT(var,expr) do {                             \
        size_t var##_size = (expr) + 1;                 \
        var##_max = max (var##_max, var##_size);        \
        var##_tot += var##_size; } while (0)
          DOIT(pp, lex_cast_qstring(*p->sole_location()).size());
          DOIT(pn, lex_cast_qstring(*p->script_location()).size());
          DOIT(location, lex_cast_qstring(p->tok->location).size());
          DOIT(derivation, lex_cast_qstring(p->derived_locations()).size());
#undef DOIT
        }

      // Decide whether it's worthwhile to use char[] or char* by comparing
      // the amount of average waste (max - avg) to the relocation data size
      // (3 native long words).
#define CALCIT(var)                                                             \
      if (s.verbose > 2)                                                        \
        clog << "adapt " << #var << ":" << var##_max << "max - " << var##_tot << "/" << s.probes.size() << "tot =>"; \
      if ((var##_max-(var##_tot/s.probes.size())) < (3 * sizeof(void*)))        \
        {                                                                       \
          s.op->newline() << "const char " << #var << "[" << var##_max << "];"; \
          if (s.verbose > 2)                                                    \
            clog << "[]" << endl;                                               \
        }                                                                       \
      else                                                                      \
        {                                                                       \
          s.op->newline() << "const char * const " << #var << ";";              \
          if (s.verbose > 2)                                                    \
            clog << "*" << endl;                                                \
        }

      s.op->newline();
      s.op->newline() << "struct stap_probe {";
      s.op->newline(1) << "const size_t index;";
      s.op->newline() << "void (* const ph) (struct context*);";
      s.op->newline() << "unsigned cond_enabled:1;"; // just one bit required
      s.op->newline() << "#if defined(STP_TIMING) || defined(STP_ALIBI)";
      CALCIT(location);
      CALCIT(derivation);
      s.op->newline() << "#define STAP_PROBE_INIT_TIMING(L, D) "
                      << ".location=(L), .derivation=(D),";
      s.op->newline() << "#else";
      s.op->newline() << "#define STAP_PROBE_INIT_TIMING(L, D)";
      s.op->newline() << "#endif";
      CALCIT(pp);
      s.op->newline() << "#ifdef STP_NEED_PROBE_NAME";
      CALCIT(pn);
      s.op->newline() << "#define STAP_PROBE_INIT_NAME(PN) .pn=(PN),";
      s.op->newline() << "#else";
      s.op->newline() << "#define STAP_PROBE_INIT_NAME(PN)";
      s.op->newline() << "#endif";
      s.op->newline() << "#define STAP_PROBE_INIT(I, PH, PP, PN, L, D) "
                      << "{ .index=(I), .ph=(PH), .cond_enabled=1, .pp=(PP), "
                      << "STAP_PROBE_INIT_NAME(PN) "
                      << "STAP_PROBE_INIT_TIMING(L, D) "
                      << "}";
      s.op->newline(-1) << "} static stap_probes[];";
      s.op->assert_0_indent();
#undef CALCIT

      // Run a varuse_collecting_visitor over probes that need global
      // variable locks.  We'll use this information later in
      // emit_lock()/emit_unlock().
      for (unsigned i=0; i<s.probes.size(); i++)
	{
          assert_no_interrupts();
          s.probes[i]->session_index = i;
          if (s.probes[i]->needs_global_locks())
	    s.probes[i]->body->visit (&cup.vcv_needs_global_locks);
          // XXX: also visit s.probes[i]->sole_condition() ?
	}
      s.op->assert_0_indent();

      for (unsigned i=0; i<s.probes.size(); i++)
        {
          assert_no_interrupts();
          s.up->emit_probe (s.probes[i]);
        }
      s.op->assert_0_indent();

      s.op->newline() << "static struct stap_probe stap_probes[] = {";
      s.op->indent(1);
      for (unsigned i=0; i<s.probes.size(); ++i)
        {
          derived_probe* p = s.probes[i];
          s.op->newline() << "STAP_PROBE_INIT(" << i << ", &" << p->name() << ", "
                          << lex_cast_qstring (*p->sole_location()) << ", "
                          << lex_cast_qstring (*p->script_location()) << ", "
                          << lex_cast_qstring (p->tok->location) << ", "
                          << lex_cast_qstring (p->derived_locations()) << "),";
        }
      s.op->newline(-1) << "};";

      if (s.runtime_usermode_p())
        {
          s.op->newline() << "static const char* stp_probe_point(size_t index) {";
          s.op->newline(1) << "if (index < ARRAY_SIZE(stap_probes))";
          s.op->newline(1) << "return stap_probes[index].pp;";
          s.op->newline(-1) << "return NULL;";
          s.op->newline(-1) << "}";
          s.op->assert_0_indent();
        }

      for (map<string,functiondecl*>::iterator it = s.functions.begin(); it != s.functions.end(); it++)
        {
          assert_no_interrupts();
          s.op->newline();
          s.up->emit_function (it->second);
        }

      s.op->assert_0_indent();
      s.op->newline();
      s.up->emit_module_init ();
      s.op->assert_0_indent();
      s.op->newline();
      s.up->emit_module_refresh ();
      s.op->assert_0_indent();
      s.op->newline();
      s.up->emit_module_exit ();
      s.op->assert_0_indent();
      s.up->emit_kernel_module_init ();
      s.op->assert_0_indent();
      s.up->emit_kernel_module_exit ();
      s.op->assert_0_indent();
      s.op->newline();

      emit_symbol_data (s);

      s.op->newline() << "MODULE_DESCRIPTION(\"systemtap-generated probe\");";
      s.op->newline() << "MODULE_LICENSE(\"GPL\");";

      for (unsigned i = 0; i < s.modinfos.size(); i++)
        {
          const string& mi = s.modinfos[i];
          size_t loc = mi.find('=');
          string tag = mi.substr (0, loc);
          string value = mi.substr (loc+1);
          s.op->newline() << "MODULE_INFO(" << tag << "," << lex_cast_qstring(value) << ");";
        }

      s.op->assert_0_indent();

      if (s.runtime_usermode_p())
        s.up->emit_global_init_setters();
      else
        // PR10298: attempt to avoid collisions with symbols
        for (unsigned i=0; i<s.globals.size(); i++)
          {
            s.op->newline();
            s.up->emit_global_param (s.globals[i]);
          }
      s.op->assert_0_indent();
    }
  catch (const semantic_error& e)
    {
      s.print_error (e);
    }

  s.op->line() << "\n";

  delete s.op;
  s.op = 0;
  s.up = 0;

 for (unsigned i=0; i<s.auxiliary_outputs.size(); i++)
   s.auxiliary_outputs[i]->close();
  
  return rc + s.num_errors();
}

/* vim: set sw=2 ts=8 cino=>4,n-2,{2,^-2,t0,(0,u0,w1,M1 : */
