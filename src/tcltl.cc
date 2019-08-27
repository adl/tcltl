// -*- coding: utf-8 -*-
// Copyright (C) 2019 Laboratoire de Recherche et Développement
// de l'Epita (LRDE).
//
// This file is part of TCLTL, a model checker for timed-automata.
//
// TCLTL is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 3 of the License, or
// (at your option) any later version.
//
// TCLTL is distributed in the hope that it will be useful, but WITHOUT
// ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
// or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public
// License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.


// A lot of code in this file is inspired from Spot's interface
// with LTSmin, as seen in Spot's spot/ltsmin/ltsmin.cc file.

#include <iostream>

#include <tchecker/parsing/parsing.hh>
#include <tchecker/utils/log.hh>
#include <tchecker/zg/zg_ta.hh>
#include <tchecker/ts/allocators.hh>
#include <tchecker/ts/builder.hh>

#include <spot/misc/fixpool.hh>
#include <spot/twaalgos/dot.hh>
#include <spot/tl/parse.hh>
#include <spot/tl/print.hh>
#include <spot/twaalgos/translate.hh>
#include <spot/twaalgos/emptiness.hh>

#include "tcltl.hh"

typedef tchecker::zg::ta::elapsed_extraLUplus_local_t zg_t;
typedef zg_t::transition_t transition_t;

typedef enum { OP_EQ, OP_NE, OP_LT, OP_GT, OP_LE, OP_GE, OP_AT } relop;

struct one_prop
{
  int var_num;
  relop op;
  int val;
  int bddvar;           // if "var_num op val" is true, output bddvar,
                        // else its negation
};

typedef std::vector<one_prop> prop_list;


struct tc_model_details final
{
public:
  const tchecker::parsing::system_declaration_t* sysdecl;
  tchecker::zg::ta::model_t* model;

  tc_model_details(tchecker::parsing::system_declaration_t const * sys,
                  tchecker::zg::ta::model_t* m)
    : sysdecl(sys), model(m)
  {
  }

  ~tc_model_details()
  {
    delete model;
    delete sysdecl;
  }
};


struct tcltl_state final: public spot::state
{
  tcltl_state(spot::fixed_size_pool* p, zg_t::shared_state_ptr_t zg)
    : pool_(p), hash_val_(hash_value(*zg)), count_(1), zg_state_(zg)
  {
  }

  tcltl_state* clone() const override
  {
    ++count_;
    return const_cast<tcltl_state*>(this);
  }

  void destroy() const override
  {
    if (--count_)
      return;
    this->~tcltl_state(); // this destroys zg_state_
    pool_->deallocate(const_cast<tcltl_state*>(this));
  }

  size_t hash() const override
  {
    return hash_val_;
  }

  int compare(const state* other) const override
  {
    if (this == other)
      return 0;
    const tcltl_state* o = spot::down_cast<const tcltl_state*>(other);
    if (hash_val_ < o->hash_val_)
      return -1;
    if (hash_val_ > o->hash_val_)
      return 1;
    // FIXME: We really want <, but tchecker does not have it.
    // https://github.com/ticktac-project/tchecker/issues/23
    return *zg_state() != *o->zg_state();
  }

  zg_t::shared_state_ptr_t zg_state() const
  {
    return zg_state_;
  }
private:

  ~tcltl_state()
  {
  }

  spot::fixed_size_pool* pool_;
  unsigned hash_val_;
  mutable unsigned count_;
  zg_t::shared_state_ptr_t zg_state_;
};


struct callback_context
{
  typedef std::list<spot::state*> transitions_t;
  transitions_t transitions;
  int state_size;
  void* pool;
  ~callback_context()
  {
    for (auto t: transitions)
      t->destroy();
  }
};

class tcltl_succ_iterator final: public spot::kripke_succ_iterator
{
public:

  tcltl_succ_iterator(const callback_context* cc,
                      bdd cond)
    : kripke_succ_iterator(cond), cc_(cc)
  {
  }

  void recycle(const callback_context* cc, bdd cond)
  {
    delete cc_;
    cc_ = cc;
    kripke_succ_iterator::recycle(cond);
  }

  ~tcltl_succ_iterator()
  {
    delete cc_;
  }

  virtual bool first() override
  {
    it_ = cc_->transitions.begin();
    return it_ != cc_->transitions.end();
  }

  virtual bool next() override
  {
    ++it_;
    return it_ != cc_->transitions.end();
  }

  virtual bool done() const override
  {
    return it_ == cc_->transitions.end();
  }

  virtual spot::state* dst() const override
  {
    return (*it_)->clone();
  }

private:
  const callback_context* cc_;
  callback_context::transitions_t::const_iterator it_;
};



class tcltl_kripke final: public spot::kripke
{
  typedef zg_t::state_pool_allocator_t<zg_t::shared_state_t> state_allocator_t;
  typedef zg_t::transition_singleton_allocator_t<zg_t::transition_t>
    transition_allocator_t;
  typedef tchecker::ts::allocator_t<state_allocator_t, transition_allocator_t>
    allocator_t;
  typedef tchecker::ts::builder_ok_t<zg_t::ts_t, allocator_t> builder_t;
  // Keep a shared pointer to the model and system so that they are
  // not deallocated before this Kripke structure.
  tc_model_details_ptr tcmd_;
  zg_t::ts_t ts_;
  mutable allocator_t allocator_;
  mutable builder_t builder_;
  const prop_list* ps_;
  bdd alive_prop;
  bdd dead_prop;
  mutable spot::fixed_size_pool statepool_;
public:

  tcltl_kripke(tchecker::gc_t& gc,
               tc_model_details_ptr tcmd,
               const spot::bdd_dict_ptr& dict,
               const prop_list* ps, spot::formula dead)
    : kripke(dict),
      tcmd_(tcmd),
      ts_(*tcmd->model),
      allocator_(gc, std::make_tuple(*tcmd->model, 100000), std::tuple<>()),
      builder_(ts_, allocator_),
      ps_(ps),
      statepool_(sizeof(tcltl_state))
  {
    // Register the "dead" proposition.  There are three cases to
    // consider:
    //  * If DEAD is "false", it means we are not interested in finite
    //    sequences of the system.
    //  * If DEAD is "true", we want to check finite sequences as well
    //    as infinite sequences, but do not need to distinguish them.
    //  * If DEAD is any other string, this is the name a property
    //    that should be true when looping on a dead state, and false
    //    otherwise.
    // We handle these three cases by setting ALIVE_PROP and DEAD_PROP
    // appropriately.  ALIVE_PROP is the bdd that should be ANDed
    // to all transitions leaving a live state, while DEAD_PROP should
    // be ANDed to all transitions leaving a dead state.
    if (dead.is_ff())
      {
        alive_prop = bddtrue;
        dead_prop = bddfalse;
      }
    else if (dead.is_tt())
      {
        alive_prop = bddtrue;
        dead_prop = bddtrue;
      }
    else
      {
        int var = dict->register_proposition(dead, this);
        dead_prop = bdd_ithvar(var);
        alive_prop = bdd_nithvar(var);
      }
  }

  ~tcltl_kripke()
  {
    if (iter_cache_)
      {
        delete iter_cache_;
        iter_cache_ = nullptr;
      }
    // https://github.com/ticktac-project/tchecker/issues/19
    allocator_.destruct_all();
    dict_->unregister_all_my_variables(ps_);
    delete ps_;
  }

  virtual tcltl_state* get_init_state() const override
  {
    tcltl_state* res = nullptr;
    bool first = true;
    auto initial_range = builder_.initial();
    for (auto it = initial_range.begin(); ! it.at_end(); ++it)
      if (first)
        {
          builder_t::state_ptr_t st;
          builder_t::transition_ptr_t trans;
          std::tie(st, trans) = *it;
          first = false;
          res = new(statepool_.allocate()) tcltl_state(&statepool_, st);
        }
      else
        {
          throw std::runtime_error("multiple initial states not supported");
        }
    return res;
  }

  virtual
  tcltl_succ_iterator* succ_iter(const spot::state* st) const override
  {
    callback_context* cc = new callback_context;
    cc->pool = &statepool_;
    auto zs = spot::down_cast<const tcltl_state*>(st);
    zg_t::shared_state_ptr_t z = zs->zg_state();
    auto outgoing_range = builder_.outgoing(z);
    for (auto it = outgoing_range.begin(); !it.at_end(); ++it)
      {
        builder_t::state_ptr_t st;
        builder_t::transition_ptr_t trans;
        std::tie(st, trans) = *it;
        tcltl_state* res =
          new(statepool_.allocate()) tcltl_state(&statepool_, st);
        cc->transitions.push_back(res);
      }

    bdd scond = state_condition(st);

    if (!cc->transitions.empty())
      {
        scond &= alive_prop;
      }
    else
      {
        scond &= dead_prop;
        // Add a self-loop to dead-states if we care about these.
        if (scond != bddfalse)
          cc->transitions.emplace_back(st->clone());
      }

    if (iter_cache_)
      {
        tcltl_succ_iterator* it =
          spot::down_cast<tcltl_succ_iterator*>(iter_cache_);
        it->recycle(cc, scond);
        iter_cache_ = nullptr;
        return it;
      }
    return new tcltl_succ_iterator(cc, scond);
  }

  virtual
  bdd state_condition(const spot::state* st) const override
  {
    bdd cond = bddtrue;
    auto zs = spot::down_cast<const tcltl_state*>(st)->zg_state();
    auto& vals = zs->intvars_valuation();
    auto& vloc = zs->vloc();
    for (const one_prop& prop: *ps_)
      {
        bool res = false;
        if (prop.op == OP_AT)
          {
            res = vloc[prop.var_num]->id() == unsigned(prop.val);
          }
        else
          {
            int val = vals[prop.var_num];
            int ref = prop.val;
            switch (prop.op)
              {
              case OP_EQ:
                res = val == ref;
                break;
              case OP_NE:
                res = val != ref;
                break;
              case OP_LT:
                res = val < ref;
                break;
              case OP_GT:
                res = val > ref;
                break;
              case OP_LE:
                res = val <= ref;
                break;
              case OP_GE:
                res = val >= ref;
                break;
              case OP_AT:
                // unreachable
                break;
              }
          }
        cond &= (res ? bdd_ithvar : bdd_nithvar)(prop.bddvar);
      }
    return cond;
  }

  virtual
  std::string format_state(const spot::state *st) const override
  {
    auto& model = ts_.model();
    auto zs = spot::down_cast<const tcltl_state*>(st)->zg_state();
    tchecker::zg::ta::state_outputter_t
      so(model.system_integer_variables().index(),
         model.system_clock_variables().index());
    std::stringstream s;
    so.output(s, *zs);
    return s.str();
  }

};

void
convert_aps(const spot::atomic_prop_set* aps,
            const tchecker::zg::ta::model_t& model,
            spot::bdd_dict_ptr dict, spot::formula dead,
            prop_list& out)
{
  int errors = 0;
  std::ostringstream err;

  const auto& sys = model.system();
  const auto& procidx = sys.processes();
  const auto& locations = sys.locations();
  auto const& intvars = model.system_integer_variables();
  auto const& varsidx = intvars.index();

  for (spot::atomic_prop_set::const_iterator ap = aps->begin();
       ap != aps->end(); ++ap)
    {
      if (*ap == dead)
        continue;

      const std::string& str = ap->ap_name();
      const char* s = str.c_str();

      // Skip any leading blank.
      while (*s && (*s == ' ' || *s == '\t'))
        ++s;
      if (!*s)
        {
          err << "Proposition `" << str << "' cannot be parsed.\n";
          ++errors;
          continue;
        }


      char* name = (char*) malloc(str.size() + 1);
      char* name_p = name;
      char* lastdot = nullptr;
      while (*s && (*s != '=') && *s != '<' && *s != '!'  && *s != '>')
        {

          if (*s == ' ' || *s == '\t')
            ++s;
          else
            {
              if (*s == '.')
                lastdot = name_p;
              *name_p++ = *s++;
            }
        }
      *name_p = 0;

      if (name == name_p)
        {
          err << "Proposition `" << str << "' cannot be parsed.\n";
          free(name);
          ++errors;
          continue;
        }

      // Lookup the name
      int varid;
      try
        {
          varid = varsidx.key(name);
        }
      catch (const std::invalid_argument&)
        {
          varid = -1;
        }

      if (varid < 0)
        {
          // We may have a name such as X.Y.Z
          // If it is not a known variable, it might mean
          // an enumerated variable X.Y with value Z.
          int procid = -1;
          if (lastdot)
            {
              *lastdot++ = 0;
              try
                {
                  procid = procidx.key(name);
                }
              catch (const std::invalid_argument&)
                {
                  procid = -1;
                }
            }

          if (procid < 0)
            {
              err << "No variable or process `" << name
                  << "' found in model (for proposition `"
                  << str << "').\n";
              free(name);
              ++errors;
              continue;
            }

          // We have found a process name, lastdot is
          // pointing to its location.
          try
            {
              varid = sys.location(name, lastdot)->id();
            }
          catch (const std::invalid_argument&)
            {
              err << "No location `" << lastdot << "' known for process `"
                  << name << "'.\n";
              // FIXME: list possible locations.
              free(name);
              ++errors;
              continue;
            }

          // At this point, *s should be 0.
          if (*s)
            {
              err << "Trailing garbage `" << s
                  << "' at end of proposition `"
                  << str << "'.\n";
              free(name);
              ++errors;
              continue;
            }

          // Record that X.Y must be equal to Z.
          int v = dict->register_proposition(*ap, &out);
          one_prop p = { procid, OP_AT, varid, v };
          out.emplace_back(p);
          free(name);
          continue;
        }

      if (!*s)                // No operator?  Assume "!= 0".
        {
          int v = dict->register_proposition(*ap, &out);
          one_prop p = { varid, OP_NE, 0, v };
          out.emplace_back(p);
          free(name);
          continue;
        }

      relop op;

      switch (*s)
        {
        case '!':
          if (s[1] != '=')
            goto report_error;
          op = OP_NE;
          s += 2;
          break;
        case '=':
          if (s[1] != '=')
            goto report_error;
          op = OP_EQ;
          s += 2;
          break;
        case '<':
          if (s[1] == '=')
            {
              op = OP_LE;
              s += 2;
            }
          else
            {
              op = OP_LT;
              ++s;
            }
          break;
        case '>':
          if (s[1] == '=')
            {
              op = OP_GE;
              s += 2;
            }
          else
            {
              op = OP_GT;
              ++s;
            }
          break;
        default:
        report_error:
          err << "Unexpected `" << s
              << "' while parsing atomic proposition `" << str
              << "'.\n";
          ++errors;
          free(name);
          continue;
        }

      while (*s && (*s == ' ' || *s == '\t'))
        ++s;

      char* s_end;
      int val = strtol(s, &s_end, 10);
      if (s == s_end)
        {
          err << "Failed to parse `" << s << "' as an integer.\n";
          ++errors;
          free(name);
          continue;
        }
      s = s_end;
      free(name);

      while (*s && (*s == ' ' || *s == '\t'))
        ++s;
      if (*s)
        {
          err << "Unexpected `" << s
              << "' while parsing atomic proposition `" << str
              << "'.\n";
          ++errors;
          continue;
        }

      int v = dict->register_proposition(*ap, &out);
      one_prop p = { varid, op, val, v };
      out.emplace_back(p);
    }

  if (errors)
    throw std::runtime_error(err.str());
}

tc_model::tc_model(tc_model_details* tcm)
  : priv_(tcm)
{
}

tc_model tc_model::load(const std::string filename)
{
  tchecker::log_t log(std::cerr);
  auto* sysdecl = tchecker::parsing::parse_system_declaration(filename, log);

  if (sysdecl == nullptr)
    throw std::runtime_error("system declaration could not be built");
  auto tcm = new tc_model_details(sysdecl,
                                  new tchecker::zg::ta::model_t(*sysdecl, log));
  return tc_model(tcm);
}

void tc_model::dump_info(std::ostream& out) const
{
  auto& s = priv_->model->system();
  // auto const& events = s.events();
  // for (const auto& e: events)
  //   out << "evt " << events.key(e) << '=' << events.value(e) << '\n';
  const auto& process_index = s.processes();
  bool first = true;
  for (const auto* loc: s.locations())
    {
      if (first)
        {
          std::cout
            << "The following location(s) may be used in the formula:\n";
          first = false;
        }
      std::string const & process_name = process_index.value(loc->pid());
      out << "- " // << loc->id() << '='
          << process_name << "." << loc->name() << '\n';
    }
  auto const& intvars = priv_->model->system_integer_variables();
  auto const& idx = intvars.index();
  first = true;
  for (const auto v: idx)
    {
      if (first)
        {
          std::cout
            << "The following variable(s) may be used in the formula:\n";
          first = false;
        }
      auto id = idx.key(v);
      tchecker::intvar_info_t const & info = intvars.info(id);
      out << "- " << /* id << '=' << */ idx.value(v)
          << " (" << info.min() << ".." << info.max() << ")\n";
    }
}

spot::kripke_ptr tc_model::kripke(tchecker::gc_t& gc,
                                  const spot::atomic_prop_set* to_observe,
                                  spot::bdd_dict_ptr dict,
                                  spot::formula dead)
{
  prop_list* ps = new prop_list;
  try
    {
      convert_aps(to_observe, *priv_->model, dict, dead, *ps);
    }
  catch (const std::runtime_error&)
    {
      dict->unregister_all_my_variables(ps);
      delete ps;
      throw;
    }
  auto res =
    std::make_shared<tcltl_kripke>(gc, priv_, dict, ps, dead);
  // All atomic propositions have been registered to the bdd_dict
  // for iface, but we also need to add them to the automaton so
  // twa::ap() works.
  for (auto ap: *to_observe)
    res->register_ap(ap);
  if (dead.is(spot::op::ap))
    res->register_ap(dead);
  return res;
}


int main(int argc, char * argv[])
{
  if (argc < 2 || argc > 4) {
    std::cerr << "Usage: " << argv[0] << " filename [formula] [-D]\n";
    return 1;
  }

  auto dict = spot::make_bdd_dict();

  spot::formula f;

  bool dot = false;
  if (argc > 1 && strncmp(argv[argc - 1], "-D", 3) == 0)
    {
      dot = 1;
      --argc;
    }

  std::string orig_formula;
  if (argc >= 3)
  {
    // Parse the input formula.
    orig_formula = argv[2];
    spot::parsed_formula pf = spot::parse_infix_psl(argv[2]);
    if (pf.format_errors(std::cerr))
      return 1;
    // Translate its negation.
    f = spot::formula::Not(pf.f);
  }

  tchecker::log_t log(std::cerr);
  int exit_code = 0;
  try {
    tchecker::gc_t gc;
    tc_model m = tc_model::load(std::string(argv[1]));
    if (f)
      {
        spot::twa_graph_ptr af = spot::translator(dict).run(f);
        spot::atomic_prop_set ap;
        spot::atomic_prop_collect(f, &ap);
        spot::twa_ptr k = m.kripke(gc, &ap, dict);
        if (dot)
          k = spot::make_twa_graph(k, spot::twa::prop_set::all(), true);
        gc.start();
        if (auto run = k->intersecting_run(af))
          {
            exit_code = 1;
            if (!dot)
              {
                std::cout
                  << "formula is violated by the following run:\n" << *run;
              }
            else
              {
                run->highlight(5);
                k->set_named_prop("automaton-name",
                                  new std::string(std::string(argv[1]) +
                                                  "\ncounterexample for "
                                                  + orig_formula));
                spot::print_dot(std::cout, k, ".kvAn");
              }
          }
        else
          {
            std::cout << "formula is verified\n";
          }
        gc.stop();
      }
    else
      {
        if (dot)
          {
            spot::atomic_prop_set ap;
            auto k = m.kripke(gc, &ap, dict);
            gc.start();
            k->set_named_prop("automaton-name", new std::string(argv[1]));
            spot::print_dot(std::cout, k, ".kvA");
            gc.stop();
          }
        else
          {
            m.dump_info(std::cout);
          }
      }
  }
  catch (std::exception const & e) {
    log.error(e.what());
    return 2;
  }
  return exit_code;
}
