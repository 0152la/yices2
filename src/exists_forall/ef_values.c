/*
 * This file is part of the Yices SMT Solver.
 * Copyright (C) 2017 SRI International.
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
 * Build a value table mapping a value to a list of terms.
 */

#include <stdint.h>
#include <stdio.h>

#include "utils/int_vectors.h"
#include "utils/int_hash_sets.h"
#include "utils/memalloc.h"
#include "exists_forall/ef_values.h"

#include "yices.h"
#include "io/yices_pp.h"
#include "terms/term_explorer.h"
#include "terms/term_substitution.h"



/*
 * Initialize the value table
 */
void init_ef_table(ef_table_t *vtable, value_table_t *vtbl, term_manager_t *mgr, term_table_t *terms) {
  init_ptr_hmap(&vtable->map, 0);
  init_ptr_hmap(&vtable->type_map, 0);
  init_int_hmap(&vtable->val_map, 0);
  vtable->vtbl = vtbl;
  vtable->mgr = mgr;
  vtable->terms = terms;

  init_val_converter(&vtable->convert, vtbl, mgr, terms);
  init_int_hmap(&vtable->priority, 0);
  init_int_hmap(&vtable->var_rep, 0);
}


/*
 * Delete the value table and all ivector objects
 */
void delete_ef_table(ef_table_t *vtable) {
  ptr_hmap_pair_t *p;
  ptr_hmap_t *map;

  map = &vtable->map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    ivector_t* list_vector = p->val;
    if (list_vector != NULL) {
      delete_ivector(list_vector);
      safe_free(list_vector);
    }
  }
  delete_ptr_hmap(map);

  map = &vtable->type_map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    ivector_t* list_vector = p->val;
    if (list_vector != NULL) {
      delete_ivector(list_vector);
      safe_free(list_vector);
    }
  }
  delete_ptr_hmap(map);

  delete_int_hmap(&vtable->val_map);

  vtable->vtbl = NULL;
  vtable->mgr = NULL;
  vtable->terms = NULL;

  delete_val_converter(&vtable->convert);
  delete_int_hmap(&vtable->priority);
  delete_int_hmap(&vtable->var_rep);
}


/*
 * Reset the value table and all ivector objects
 */
void reset_ef_table(ef_table_t *vtable, value_table_t *vtbl, term_manager_t *mgr, term_table_t *terms) {
  ptr_hmap_pair_t *p;
  ptr_hmap_t *map;

  map = &vtable->map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    ivector_t* list_vector = p->val;
    if (list_vector != NULL) {
      delete_ivector(list_vector);
      safe_free(list_vector);
    }
  }
  ptr_hmap_reset(map);

  map = &vtable->type_map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    ivector_t* list_vector = p->val;
    if (list_vector != NULL) {
      delete_ivector(list_vector);
      safe_free(list_vector);
    }
  }
  ptr_hmap_reset(map);

  int_hmap_reset(&vtable->val_map);

  vtable->vtbl = vtbl;
  vtable->mgr = mgr;
  vtable->terms = terms;

  delete_val_converter(&vtable->convert);
  init_val_converter(&vtable->convert, vtbl, mgr, terms);
  int_hmap_reset(&vtable->priority);
  int_hmap_reset(&vtable->var_rep);
}


/*
 * Print the value table and all ivector objects
 */
void print_ef_table(FILE *f, ef_table_t *vtable) {
  ptr_hmap_pair_t *p;
  ptr_hmap_t *map;
  int_hmap_t *imap;
  int_hmap_pair_t *ip;
  ivector_t *v;

  fprintf(f, "\n== EF VALUE TYPES ==\n");
  map = &vtable->type_map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    v = p->val;
    yices_pp_type(f, p->key, 100, 1, 10);
    fprintf(f, " -> ");
    yices_pp_term_array(f, v->size, v->data, 120, UINT32_MAX, 0, 1);
  }

  fprintf(f, "\n== EF VALUES ==\n");
  imap = &vtable->val_map;
  for (ip = int_hmap_first_record(imap);
       ip != NULL;
       ip = int_hmap_next_record(imap, ip)) {
    pp_value(f, vtable->vtbl, ip->key);
    fprintf(f, " -> %s\n", yices_term_to_string(ip->val, 120, 1, 0));
  }

  fprintf(f, "\n== EF PRIORITY ==\n");
  imap = &vtable->priority;
  for (ip = int_hmap_first_record(imap);
       ip != NULL;
       ip = int_hmap_next_record(imap, ip)) {
    fprintf(f, "%s -> %d\n", yices_term_to_string(ip->key, 120, 1, 0), ip->val);
  }

  fprintf(f, "\n== EF VALUE TERMS ==\n");
  map = &vtable->map;
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    v = p->val;
    fprintf(f, "%s -> ", yices_term_to_string(p->key, 120, 1, 0));
    yices_pp_term_array(f, v->size, v->data, 120, UINT32_MAX, 0, 1);
  }
  fprintf(f, "\n");
}


/*
 * Add / update var priority
 */
static void store_term_priority(ef_table_t *vtable, term_t var, uint32_t priority) {
  int_hmap_pair_t *p;

  p = int_hmap_get(&vtable->priority, var);
  p->val = priority;
}


/*
 * Add / update tvalue representative
 */
static void store_rep(ef_table_t *vtable, term_t tvalue, term_t var) {
  int_hmap_pair_t *p;

  p = int_hmap_get(&vtable->var_rep, tvalue);
  if (p->val < 0)
    p->val = var;
}

/*
 * Calculate var priority
 */
static uint32_t calculate_priority(ef_table_t *vtable, term_t xc) {
  composite_term_t *app;
  term_t f;
  int_hmap_pair_t *p;
  uint32_t i, m, result;


  assert(term_kind(vtable->terms, xc) == APP_TERM);

  app = app_term_desc(vtable->terms, xc);
  m = app->arity - 1;
  result = 1;

  for(i=1; i<=m; i++) {
    f = app->arg[i];

    p = int_hmap_find(&vtable->priority, f);
    if (p == NULL) {
      return 0;
    }
    result += p->val;
  }

  return result;
}


/*
 * Store mapping type to value
 */
void store_type_value(ef_table_t *vtable, value_t value, term_t tvalue, bool check) {
  ptr_hmap_pair_t *r;
  value_kind_t kind;
  type_t tau;

  if (check) {
    r = ptr_hmap_find(&vtable->map, tvalue);
    if (r != NULL)
      return;
  }

  kind = object_kind(vtable->vtbl, value);
  switch (kind) {
  case BOOLEAN_VALUE:
  case RATIONAL_VALUE:
  case BITVECTOR_VALUE:
  case UNINTERPRETED_VALUE:
    break;

  default:
    return;
  }

  tau = term_type(vtable->terms, tvalue);
  r = ptr_hmap_get(&vtable->type_map, tau);
  if (r->val == NULL) {
    r->val = safe_malloc(sizeof(ivector_t));
    init_ivector(r->val, 0);
  }
  ivector_push(r->val, tvalue);
}


/*
 * Store mapping value to var
 */
static void store_term_value(ef_table_t *vtable, term_t var, value_t value) {
  int_hmap_pair_t *vm;
  ptr_hmap_pair_t *m;
  term_t tvalue;

  vm = int_hmap_get(&vtable->val_map, value);
  if (vm->val < 0) {
    tvalue = convert_val(&vtable->convert, value);
    vm->val = tvalue;

    m = ptr_hmap_get(&vtable->map, tvalue);
    assert (m->val == NULL);
    m->val = safe_malloc(sizeof(ivector_t));
    init_ivector(m->val, 0);
    store_type_value(vtable, value, tvalue, false);
  }
  else {
    tvalue = vm->val;

    m = ptr_hmap_get(&vtable->map, tvalue);
    assert (m->val != NULL);
  }

  ivector_push(m->val, var);
  if (term_is_atomic(vtable->terms, var)) {
    store_term_priority(vtable, var, 0);
    store_term_priority(vtable, tvalue, 0);
    store_rep(vtable, tvalue, var);
  }
}


/*
 * Store function mapping values to var
 */
static void store_func_values(ef_table_t *vtable, term_t func, value_t c) {
  val_converter_t *convert;
  value_table_t *table;
  term_table_t *terms;
  type_table_t *types;
  type_t tau;

  convert = &vtable->convert;
  table = vtable->vtbl;
  terms = vtable->terms;
  types = terms->types;
  tau = term_type(terms, func);

  assert(yices_type_is_function(tau));

  value_fun_t *fun;
  value_map_t *mp;
  uint32_t m, n, i, j;
  term_t vari;
  value_t valuei;
  term_t *args;

  assert(0 <= c && c < table->nobjects && table->kind[c] == FUNCTION_VALUE);

  fun = table->desc[c].ptr;
  assert(is_function_type(types, fun->type));

  m = fun->arity;
  n = fun->map_size;
  i = 0;

  if (!is_unknown(table, fun->def)) {
    valuei = fun->def;
    // TODO
    printf("warning: need to handle default values in function interpretations\n");
  }

  if (n != 0) {
    // entries present in map
    assert(m > 0);
    args = (term_t *) safe_malloc(m * sizeof(term_t));

    for (; i<n; i++) {
      mp = vtbl_map(table, fun->map[i]);
      assert(mp->arity == m);

      for (j=0; j<m; j++) {
        args[j] = convert_value(convert, mp->arg[j]);
      }

      vari = mk_application(convert->manager, func, m, args);
      valuei = mp->val;
      store_term_value(vtable, vari, valuei);
    }

    safe_free(args);
  }
}


/*
 * Fill the value table
 */
void fill_ef_table(ef_table_t *vtable, term_t *vars, value_t *values, uint32_t k) {
  uint32_t i;
  value_kind_t kind;

  // first pass: process top-level terms
  for (i=0; i<k; i++) {
    store_term_value(vtable, vars[i], values[i]);
  }

  // second pass: process function values
  for (i=0; i<k; i++) {
    kind = object_kind(vtable->vtbl, values[i]);
    if (kind == FUNCTION_VALUE)
      store_func_values(vtable, vars[i], values[i]);
  }

  // third pass: process function instances
  uint32_t j, n, m, prio, best_prio;
  int_queue_t queue;
  ptr_hmap_pair_t *p;
  ptr_hmap_t *map;
  int_hmap_t *var_rep;
  term_t tvalue, x, best_x;
  ivector_t *v;

  map = &vtable->map;
  var_rep = &vtable->var_rep;
  m = map->size;

  init_int_queue(&queue, m);
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    if (int_hmap_find(var_rep, p->key) == NULL) {
      int_queue_push(&queue, p->key);
    }
  }
  m = queue.size;
  j = 0;
  while(!int_queue_is_empty(&queue)) {
    tvalue = int_queue_pop(&queue);
    p = ptr_hmap_find(map, tvalue);
    assert(p != NULL);
    v = p->val;
    n = v->size;

    best_prio = UINT32_MAX;
    best_x = NULL_TERM;
    assert(n != 0);

    for(i=0; i<n; i++) {
      x = v->data[i];
      prio = calculate_priority(vtable, x);
      if (prio > 0) {
        store_term_priority(vtable, x, prio);
        if (prio < best_prio) {
          best_prio = prio;
          best_x = x;
        }
      }
    }
    if (best_x != NULL_TERM) {
      store_term_priority(vtable, tvalue, best_prio);
      store_rep(vtable, tvalue, best_x);
      m--;
      j = 0;
    }
    else {
      j++;
      int_queue_push(&queue, tvalue);
    }
    if (j >= m) {
      printf("Unable to clear dependency for %s\n", yices_term_to_string(tvalue, 120, 1, 0));
      print_ef_table(stdout, vtable);
      assert(0);
    }
  }
}


/*
 * SUBSTITUTION
 */


/*
 * Check whether t is either a variable or an uninterpreted term
 * - t must be a good positive term
 */
static bool term_is_var(term_table_t *terms, term_t t) {
  assert(good_term(terms, t) && is_pos_term(t));
  switch (term_kind(terms, t)) {
  case UNINTERPRETED_TERM:
  case CONSTANT_TERM:
    return true;

  default:
    return false;
 }
}

/*
 * Apply the substitution defined by var and value to term t
 * - n = size of arrays var and value
 * - return code < 0 means that an error occurred during the substitution
 *   (cf. apply_term_subst in term_substitution.h).
 */
static term_t term_substitution(ef_table_t *vtable, term_t *var, term_t *value, uint32_t n, term_t t) {
  term_subst_t subst;
  term_t g;
  int_hmap_pair_t *p;
  uint32_t i;
  term_t x;

  subst.mngr = vtable->mgr;
  subst.terms = vtable->terms;
  init_int_hmap(&subst.map, 0);
  init_subst_cache(&subst.cache);
  init_istack(&subst.stack);
  subst.rctx = NULL;

  for (i=0; i<n; i++) {
    x = var[i];
    p = int_hmap_get(&subst.map, x);
    p->val = value[i];

    assert(is_pos_term(x));
    assert(term_is_var(subst.terms, x));
    assert(good_term(subst.terms, p->val));
  }

  g = apply_term_subst(&subst, t);
  delete_term_subst(&subst);

  return g;
}


/*
 * Get value representative helper
 */
term_t ef_get_value_rep(ef_table_t *vtable, term_t value, int_hset_t *requests) {
  ptr_hmap_pair_t *r;

  r = ptr_hmap_find(&vtable->map, value);
  if (r == NULL) {
    printf("Unable to find a representative for term: %s\n", yices_term_to_string(value, 120, 1, 0));
    assert(0);
    return value;
  }
  else {
    term_t x, best_x;
    int_hmap_pair_t *p;
    uint32_t i, n, m;
    ivector_t *v;
    uint32_t best_prio;

    p = int_hmap_find(&vtable->var_rep, value);
    if (p != NULL) {
      best_x = p->val;
    }
    else {
      v = r->val;
      n = v->size;
      best_prio = UINT32_MAX;
      best_x = NULL_TERM;
      assert(n != 0);

      for(i=0; i<n; i++) {
        x = v->data[i];
        p = int_hmap_find(&vtable->priority, x);
        if (p != NULL) {
          if (p->val < best_prio) {
            best_prio = p->val;
            best_x = x;
          }
        }
        if (best_x == NULL_TERM)
          best_x = x;
      }
      store_rep(vtable, value, best_x);
      assert(0);
    }

    if (!term_is_composite(vtable->terms, best_x)) {
      return best_x;
    }

    // function value
    int_hset_add(requests, value);

    composite_term_t *app;
    ivector_t args, argsrep;
    term_t xcrep;
    term_t f, frep;
    bool present;

    assert(term_kind(vtable->terms, best_x) == APP_TERM);

    app = app_term_desc(vtable->terms, best_x);
    m = app->arity - 1;

    init_ivector(&args, m);
    init_ivector(&argsrep, m);

    for(i=1; i<=m; i++) {
      f = app->arg[i];

      present = int_hset_member(requests, f);
      if (present) {
        printf("Circular dependency encountered while finding a representative for term: %s\n", yices_term_to_string(value, 120, 1, 0));
        assert(0);
      }

      frep = ef_get_value_rep(vtable, f, requests);
      if (f != frep) {
        ivector_push(&args, f);
        ivector_push(&argsrep, frep);
      }
    }

    xcrep = term_substitution(vtable, args.data, argsrep.data, args.size, best_x);

    delete_ivector(&args);
    delete_ivector(&argsrep);

    return xcrep;
  }
}


/*
 * Get value representative
 */
term_t ef_get_value(ef_table_t *vtable, term_t value) {
  int_hset_t value_requests;
  term_t rep;

  init_int_hset(&value_requests, 2);
  rep = ef_get_value_rep(vtable, value, &value_requests);
  delete_int_hset(&value_requests);

  return rep;
}


/*
 * Set values from the value table
 */
void ef_set_values_from_table(ef_table_t *vtable, term_t *vars, term_t *values, uint32_t n) {
  uint32_t i;
  term_t x;

  for (i=0; i<n; i++) {
    x = values[i];
    if (is_utype_term(vtable->terms, x)) {
      // replace x by representative
      values[i] = ef_get_value(vtable, x);
    }
  }
}


static term_t constraint_distinct_elements(ivector_t *v) {
  if (v->size < 2)
    return yices_true();
  else
    return yices_distinct(v->size, v->data);
}

term_t constraint_distinct(ef_table_t *vtable) {
  ptr_hmap_pair_t *p;
  ptr_hmap_t *map;
  type_t tau;
  ivector_t *v;
  term_t result;

  map = &vtable->type_map;
  result = yices_true();
  for (p = ptr_hmap_first_record(map);
       p != NULL;
       p = ptr_hmap_next_record(map, p)) {
    tau = p->key;
    if (yices_type_is_uninterpreted(tau)) {
      v = p->val;
      result = yices_and2(result, constraint_distinct_elements(v));
    }
  }

  return result;
}

term_t constraint_distinct_filter(ef_table_t *vtable, uint32_t n, term_t *vars) {
  ptr_hmap_t map;
  ptr_hmap_pair_t *p;
  uint32_t i;
  type_t tau;
  ivector_t *v;
  term_t t, result;

  init_ptr_hmap(&map, 0);
  result = yices_true();

  for(i=0; i<n; i++) {
    t = vars[i];
    tau = term_type(vtable->terms, t);

    if (yices_type_is_uninterpreted(tau)) {
      p = ptr_hmap_get(&map, tau);
      if (p->val == NULL) {
        p->val = safe_malloc(sizeof(ivector_t));
        init_ivector(p->val, 0);
      }

      ivector_push(p->val, t);
    }
  }

  for (p = ptr_hmap_first_record(&map);
       p != NULL;
       p = ptr_hmap_next_record(&map, p)) {
    v = p->val;
    result = yices_and2(result, constraint_distinct_elements(v));
  }

  for (p = ptr_hmap_first_record(&map);
       p != NULL;
       p = ptr_hmap_next_record(&map, p)) {
    ivector_t* list_vector = p->val;
    if (list_vector != NULL) {
      delete_ivector(list_vector);
      safe_free(list_vector);
    }
  }
  delete_ptr_hmap(&map);


  return result;
}

static term_t constraint_scalar_element(ef_table_t *vtable, term_t t, int32_t bound) {
  term_t result, u;
  type_t tau;
  ptr_hmap_pair_t *r;
  int_hmap_pair_t *p;
  ivector_t *v;
  ivector_t eq;
  uint32_t n, i;

  result = yices_true();
  tau = yices_type_of_term(t);

  if (yices_type_is_uninterpreted(tau)) {
    r = ptr_hmap_find(&vtable->type_map, tau);

    if (r != NULL) {
      v = r->val;
      n = v->size;

      init_ivector(&eq, n);

      for(i=0; i<n; i++) {
        u = v->data[i];
        if (bound >= 0) {
          p = int_hmap_find(&vtable->priority, u);
          if(p != NULL && p->val > bound)
              continue;
          }
        ivector_push(&eq, yices_eq(t, u));
      }
      result = yices_and2(result, yices_or(eq.size, eq.data));

      delete_ivector(&eq);
    }
  }
  return result;
}

term_t constraint_scalar(ef_table_t *vtable, uint32_t n, term_t *t, int32_t bound) {
  term_t result;
  uint32_t i;

  result = yices_true();
  for(i=0; i<n; i++) {
    result = yices_and2(result, constraint_scalar_element(vtable, t[i], bound));
  }

  return result;
}
