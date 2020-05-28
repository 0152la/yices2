/*
 * This file is part of the Yices SMT Solver.
 * Copyright (C) 2020 SRI International.
 *
 * Yices is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Yices is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Yices.  If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Skolemization for the EF solver.
 */


#include <stdint.h>
#include <stdio.h>

#include "exists_forall/ef_skolemize.h"

#include "yices.h"
#include "io/yices_pp.h"
#include "terms/term_explorer.h"


#define TRACE 0

/*
 * Initialize the skolemize object
 */
void init_ef_skolemize(ef_skolemize_t *sk, ef_analyzer_t *analyzer, bool f_ite, bool f_iff) {
  sk->analyzer = analyzer;
  sk->flatten_ite = f_ite;
  sk->flatten_iff = f_iff;

  sk->mgr = analyzer->manager;
  sk->terms = analyzer->terms;
  init_ivector(&sk->uvars, 10);
}


/*
 * Delete the skolemize object
 */
void delete_ef_skolemize(ef_skolemize_t *sk) {
  sk->analyzer = NULL;
  sk->flatten_ite = false;
  sk->flatten_iff = false;

  sk->mgr = NULL;
  sk->terms = NULL;
  delete_ivector(&sk->uvars);
}


/*
 * Construct a composite term by updating it's children.
 *
 */
static inline term_t ef_update_composite(ef_skolemize_t *sk, term_t t, ivector_t *args) {
  term_manager_t *tm;
  term_table_t *terms;
  term_kind_t kind;
  term_t result;
  uint32_t n;
  term_t *c;

  tm = sk->mgr;
  terms = sk->terms;
  kind = term_kind(terms, t);
  n = args->size;
  c = args->data;

#if TRACE
    printf("Updating: %s\n", yices_term_to_string(t, 120, 1, 0));
    yices_pp_term_array(stdout, n, c, 120, 120, 0, 0);
#endif

  result = NULL_TERM;
  switch (kind) {
  case EQ_TERM:            // equality
    assert(n == 2);
    result = mk_eq(tm, c[0], c[1]);
    break;
  case OR_TERM:            // n-ary OR
    assert(n > 1);
    result = mk_or(tm, n, c);
    break;
  case XOR_TERM:           // n-ary XOR
    result = mk_xor(tm, n, c);
    break;
  case BV_ARRAY:
    assert(n >= 1);
    result = mk_bvarray(tm, n, c);
    break;
  case BV_DIV:
    assert(n == 2);
    result = mk_bvdiv(tm, c[0], c[1]);
    break;
  case BV_REM:
    assert(n == 2);
    result = mk_bvrem(tm, c[0], c[1]);
    break;
  case BV_SDIV:
    assert(n == 2);
    result = mk_bvsdiv(tm, c[0], c[1]);
    break;
  case BV_SREM:
    assert(n == 2);
    result = mk_bvsrem(tm, c[0], c[1]);
    break;
  case BV_SMOD:
    assert(n == 2);
    result = mk_bvsmod(tm, c[0], c[1]);
    break;
  case BV_SHL:
    assert(n == 2);
    result = mk_bvshl(tm, c[0], c[1]);
    break;
  case BV_LSHR:
    assert(n == 2);
    result = mk_bvlshr(tm, c[0], c[1]);
    break;
  case BV_ASHR:
    assert(n == 2);
    result = mk_bvashr(tm, c[0], c[1]);
    break;
  case BV_EQ_ATOM:
    assert(n == 2);
    result = mk_bveq(tm, c[0], c[1]);
    break;
  case BV_GE_ATOM:
    assert(n == 2);
    result = mk_bvge(tm, c[0], c[1]);
    break;
  case BV_SGE_ATOM:
    assert(n == 2);
    result = mk_bvsge(tm, c[0], c[1]);
    break;
  case ITE_TERM: {
    assert(n == 3);
    type_t tau = term_type(tm->terms, c[1]);
    result = mk_ite(tm, c[0], c[1], c[2], tau);
  }
  break;
  case APP_TERM:
    result = mk_application(tm, c[0], n-1, &c[1]);
    break;
  default:
    assert(false);
  }

  return result;
}


/*
 * Skolemize variable x using uvars as skolem arguments
 */
ef_skolem_t ef_skolem_term(ef_analyzer_t *ef, term_t x, uint32_t n, term_t *uvars) {
  type_t *domt;
  type_t rt;
  uint32_t i;
  term_table_t *terms;
  ef_skolem_t skolem;

  terms = ef->terms;
  ef->num_skolem++;

  domt = (type_t *) safe_malloc(n * sizeof(type_t));
  for (i=0; i<n; i++) {
    domt[i] = term_type(terms, uvars[i]);
  }
  rt = term_type(terms, x);

  type_t funct = yices_function_type(n, domt, rt);
  skolem.func = yices_new_uninterpreted_term(funct);

  char name[50];
  sprintf (name, "skolem%d_%s", ef->num_skolem, yices_get_term_name(x));
  yices_set_term_name(skolem.func, name);

  skolem.fapp = yices_application(skolem.func, n, uvars);

#if TRACE
  printf("Skolemization: %s --> %s", yices_get_term_name(x), yices_term_to_string(skolem.fapp, 120, 1, 0));
#endif
  return skolem;
}


/*
 * Skolemize existentials in an analyzer
 */
static term_t ef_skolem_body(ef_skolemize_t *sk, term_t t) {
  ef_analyzer_t *ef;
  term_table_t *terms;
  ivector_t *uvars;
  term_t body;
  composite_term_t *d;
  int_hmap_pair_t *r;
  uint32_t i, n;
  term_t *a;

  ef = sk->analyzer;
  terms = sk->terms;
  uvars = &sk->uvars;

  /* the existential case
   * t is (NOT (FORALL x_0 ... x_k : body)
   * body is the last argument in the term descriptor
   */
  d = forall_term_desc(terms, t);
  n = d->arity - 1;
  assert(n >= 1);
  a = d->arg;
  body = opposite_term(d->arg[n]);

  if (uvars->size == 0) {
    for(i = 0; i < n; i++){
      r = int_hmap_find(&ef->existentials, a[i]);
      assert(r == NULL);
      int_hmap_add(&ef->existentials, a[i], a[i]);
    }
  }
  else {
    term_subst_t subst;
    term_t *skolems;
    ef_skolem_t sk;

    skolems = (term_t *) safe_malloc(n * sizeof(term_t));

    for(i = 0; i < n; i++){
      r = int_hmap_find(&ef->existentials, a[i]);
      assert(r == NULL);

      sk = ef_skolem_term(ef, a[i], uvars->size, uvars->data);
      skolems[i] = sk.fapp;
      int_hmap_add(&ef->existentials, a[i], sk.func);
    }

    init_term_subst(&subst, ef->manager, n, a, skolems);
    body = apply_term_subst(&subst, body);
    delete_term_subst(&subst);

    safe_free(skolems);
  }

  return body;
}


/*
 * Convert a term to negated normal form and skolemize
 *
 */
static term_t ef_skolemize_term(ef_skolemize_t *sk, term_t t) {
  term_manager_t *mgr;
  term_table_t *terms;
  term_kind_t kind;
  composite_term_t *d;
  uint32_t i, n;
  ivector_t args;
  term_t result, u, v;

  mgr = sk->mgr;
  terms = sk->terms;
  kind = term_kind(terms, t);
  result = NULL_TERM;

  if (term_is_atomic(terms, unsigned_term(t)))
    result = t;
  else {
    n = term_num_children(terms, t);
    init_ivector(&args, n);

    if (is_neg_term(t)) {
      switch (kind) {
      case ITE_TERM:
      case ITE_SPECIAL:
        d = ite_term_desc(terms, t);
        assert(d->arity == 3);
        if (sk->flatten_ite && is_boolean_term(terms, d->arg[1])) {
          assert(is_boolean_term(terms, d->arg[2]));
          /*
           * t is (not (ite C A B))
           *    u := (C => not A)
           *    v := (not C => not B)
           */
          u = d->arg[1];                                    // A
          v = d->arg[2];                                    // B
          u = opposite_term(u);                             // (not A)
          v = opposite_term(v);                             // (not B)
          u = mk_implies(mgr, d->arg[0], u);                // (C => u)
          v = mk_implies(mgr, opposite_term(d->arg[0]), v); // (not C) => v
          u = ef_skolemize_term(sk, u);
          v = ef_skolemize_term(sk, v);
          ivector_push(&args, u);
          ivector_push(&args, v);
          result = mk_and(mgr, 2, args.data);
        }
        break;

      case EQ_TERM:
        d = eq_term_desc(terms, t);
        assert(d->arity == 2);
        if (sk->flatten_iff && is_boolean_term(terms, d->arg[0])) {
          assert(is_boolean_term(terms, d->arg[1]));
          /*
           * t is (not (iff A B))
           * flatten to not(A => B) or not(B => A)
           *
           */
          u = mk_implies(mgr, d->arg[0], d->arg[1]); // (u => v)
          v = mk_implies(mgr, d->arg[1], d->arg[0]); // (v => u);
          u = opposite_term(u);
          v = opposite_term(v);
          u = ef_skolemize_term(sk, u);
          v = ef_skolemize_term(sk, v);
          ivector_push(&args, u);
          ivector_push(&args, v);
          result = mk_or(mgr, 2, args.data);
        }
        break;

      case OR_TERM:
        d = or_term_desc(terms, t);
        /*
         * t is (not (or a[0] ... a[n-1]))
         * it flattens to (and (not a[0]) ... (not a[n-1]))
         */
        n = d->arity;
        for (i=0; i<n; i++) {
          u = opposite_term(d->arg[i]);
          v = ef_skolemize_term(sk, u);
          ivector_push(&args, v);
        }
        result = mk_and(mgr, n, args.data);
        break;

      case FORALL_TERM:
        /*
         * t is (not (forall .. body))
         * it flattens to (exists .. (not body))
         */
        u = ef_skolem_body(sk, t);
        result = ef_skolemize_term(sk, u);
        break;

      default:
        break;
      }

      v = unsigned_term(t);
      n = term_num_children(terms, v);
      if (result == NULL_TERM) {
        for(i=0; i<n; i++) {
          u = term_child(terms, v, i);
          u = ef_skolemize_term(sk, u);
          ivector_push(&args, u);
        }
        result = opposite_term(ef_update_composite(sk, v, &args));
      }
    }
    else {
      switch (kind) {
      case ITE_TERM:
      case ITE_SPECIAL:
        d = ite_term_desc(terms, t);
        assert(d->arity == 3);
        if (sk->flatten_ite && is_boolean_term(terms, d->arg[1])) {
          assert(is_boolean_term(terms, d->arg[2]));
          /*
           * t is (ite C A B)
           *    u := (C => A)
           *    v := (not C => B)
           */
          u = d->arg[1];                                    // A
          v = d->arg[2];                                    // B
          u = mk_implies(mgr, d->arg[0], u);                // (C => u)
          v = mk_implies(mgr, opposite_term(d->arg[0]), v); // (not C) => v
          u = ef_skolemize_term(sk, u);
          v = ef_skolemize_term(sk, v);
          ivector_push(&args, u);
          ivector_push(&args, v);
          result = mk_and(mgr, 2, args.data);
        }
        break;

      case EQ_TERM:
        d = eq_term_desc(terms, t);
        assert(d->arity == 2);
        if (sk->flatten_iff && is_boolean_term(terms, d->arg[0])) {
          assert(is_boolean_term(terms, d->arg[1]));
          /*
           * t is (iff A B)
           * flatten to (A => B) and (B => A)
           *
           */
          u = mk_implies(mgr, d->arg[0], d->arg[1]); // (u => v)
          v = mk_implies(mgr, d->arg[1], d->arg[0]); // (v => u);
          u = ef_skolemize_term(sk, u);
          v = ef_skolemize_term(sk, v);
          ivector_push(&args, u);
          ivector_push(&args, v);
          result = mk_and(mgr, 2, args.data);
        }
        break;

      case FORALL_TERM:
        /*
         * t is (forall .. body)
         * it flattens to body
         */
        d = forall_term_desc(terms, t);
        n = d->arity - 1;
        for (i=0; i<n; i++) {
          ivector_push(&sk->uvars, d->arg[i]);
        }

        u = d->arg[n];
        result = ef_skolemize_term(sk, u);

        for (i=0; i<n; i++) {
          ivector_pop(&sk->uvars);
        }
        break;

      default:
        break;
      }

      if (result == NULL_TERM) {
        for(i=0; i<n; i++) {
          u = term_child(terms, t, i);
          u = ef_skolemize_term(sk, u);
          ivector_push(&args, u);
        }
        result = ef_update_composite(sk, unsigned_term(t), &args);
      }
    }

    delete_ivector(&args);
  }

  assert(result != NULL_TERM);
  return result;
}


/*
 * Get the skolemized version of term t
 */
term_t ef_skolemize(ef_skolemize_t *sk, term_t t) {
  return ef_skolemize_term(sk, t);
}


/*
 * Skolemize existentials in an analyzer
 */
term_t ef_analyzer_add_existentials(ef_analyzer_t *ef, bool toplevel, int_hmap_t *parent, term_t t) {
  uint32_t i, m, n;
  ivector_t uvars;
  int_hmap_pair_t *r;
  term_t p;
  composite_term_t *d;
  term_table_t *terms;
  term_t *a;
  term_t body;

    terms = ef->terms;
    init_ivector(&uvars, 10);

    /* the existential case
     * t is (NOT (FORALL x_0 ... x_k : body)
     * body is the last argument in the term descriptor
     */
    d = forall_term_desc(terms, t);
    n = d->arity - 1;
    assert(n >= 1);
    a = d->arg;
    body = opposite_term(d->arg[n]);

  if (!toplevel) {
    r = int_hmap_find(parent, t);
    while(r != NULL) {
      p = r->val;
      if (term_kind(terms, p) == FORALL_TERM) {
        if (is_pos_term(p)) {
          d = forall_term_desc(terms, p);
          m = d->arity;
          assert(m >= 2);
          for (i=0; i<m-1; i++) {
            ivector_push(&uvars, d->arg[i]);
          }
        }
      }
      r = int_hmap_find(parent, p);
    }
  }

  if (uvars.size == 0) {
    for(i = 0; i < n; i++){
      r = int_hmap_find(&ef->existentials, a[i]);
      assert(r == NULL);
      int_hmap_add(&ef->existentials, a[i], a[i]);
    }
  }
  else {
    term_subst_t subst;
    term_t *skolems;
    ef_skolem_t sk;

    skolems = (term_t *) safe_malloc(n * sizeof(term_t));

    for(i = 0; i < n; i++){
      r = int_hmap_find(&ef->existentials, a[i]);
      assert(r == NULL);

      sk = ef_skolem_term(ef, a[i], uvars.size, uvars.data);
      skolems[i] = sk.fapp;
      int_hmap_add(&ef->existentials, a[i], sk.func);
    }

    init_term_subst(&subst, ef->manager, n, a, skolems);
    body = apply_term_subst(&subst, body);
    delete_term_subst(&subst);

    safe_free(skolems);
  }

  delete_ivector(&uvars);
  return body;
}

