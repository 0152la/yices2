/*
 * GLOBAL TERM/TYPE DATABASE
 */

/*
 * This module implements the term and type construction API defined in yices.h.
 * It also implements the functions defined in yices_extensions.h for managing 
 * buffers and converting buffers to terms.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "refcount_strings.h"
#include "int_array_sort.h"
#include "dl_lists.h"
#include "int_vectors.h"
#include "bit_tricks.h"
#include "bv64_constants.h"

#include "types.h"
#include "pprod_table.h"
#include "bit_expr.h"
#include "terms.h"

#include "bit_term_conversion.h"
#include "bvlogic_buffers.h"
#include "arith_buffer_terms.h"
#include "bvarith_buffer_terms.h"
#include "bvarith64_buffer_terms.h"

#include "yices.h"
#include "yices_extensions.h"
#include "yices_globals.h"


/*
 * Visibility control: all extern functions declared here are in libyices's API
 *
 * Other extern functions should have visibility=hidden (cf. Makefile).
 *
 * Note: adding __declspec(dllexport) should have the same effect
 * on cygwin or mingw but that does not seem to work.
 */
#if defined(CYGWIN) || defined(MINGW)
#define EXPORTED __declspec(dllexport)
#else
#define EXPORTED __attribute__((visibility("default")))
#endif



/*
 * Global tables:
 * - type table
 * - term table
 * - power product table
 * - node table
 *
 * Auxiliary structures:
 * - object stores used by the arithmetic and bit-vector 
 *   arithmetic buffers
 */
// global tables
static type_table_t types;
static term_table_t terms;
static pprod_table_t pprods;
static node_table_t nodes;

// arithmetic stores
static object_store_t arith_store;
static object_store_t bvarith_store;
static object_store_t bvarith64_store;

// error report
static error_report_t error;

// auxiliary rationals
static rational_t r0;
static rational_t r1;

// auxiliary bitvector constants
static bvconstant_t bv0;
static bvconstant_t bv1;
static bvconstant_t bv2;

// generic integer vector
static ivector_t vector0;


/*
 * Initial sizes of the type and term tables.
 */
#define INIT_TYPE_SIZE  16
#define INIT_TERM_SIZE  64


/*
 * Doubly-linked list of arithmetic buffers
 */
typedef struct {
  dl_list_t header;
  arith_buffer_t buffer;
} arith_buffer_elem_t;

static dl_list_t arith_buffer_list;


/*
 * Doubly-linked list of bitvector arithmetic buffers
 */
typedef struct {
  dl_list_t header;
  bvarith_buffer_t buffer;
} bvarith_buffer_elem_t;

static dl_list_t bvarith_buffer_list;


/*
 * Variant: 64bit buffers
 */
typedef struct {
  dl_list_t header;
  bvarith64_buffer_t buffer;
} bvarith64_buffer_elem_t;

static dl_list_t bvarith64_buffer_list;


/*
 * Doubly-linked list of bitvector buffers
 */
typedef struct {
  dl_list_t header;
  bvlogic_buffer_t buffer;
} bvlogic_buffer_elem_t;

static dl_list_t bvlogic_buffer_list;


/*
 * Auxiliary buffer for internal conversion of equality and
 * disequalities to arithmetic atoms.
 */
static arith_buffer_t *internal_arith_buffer;

/*
 * Auxiliary buffers for bitvector polynomials.
 */
static bvarith_buffer_t *internal_bvarith_buffer;
static bvarith64_buffer_t *internal_bvarith64_buffer;

/*
 * Auxiliary bitvector buffer.
 */
static bvlogic_buffer_t *internal_bvlogic_buffer;


/*
 * Global table; initially all 7 pointers are NULL
 */
yices_globals_t __yices_globals = {
  NULL, NULL, NULL, NULL, NULL, NULL, NULL,
};




/**********************************
 *  ARITHMETIC-BUFFER ALLOCATION  *
 *********************************/

/*
 * Get header of buffer b, assuming b is embedded into an arith_buffer_elem
 */
static inline dl_list_t *arith_buffer_header(arith_buffer_t *b) {
  return (dl_list_t *)(((char *)b) - offsetof(arith_buffer_elem_t, buffer));
}

/*
 * Get buffer of header l
 */
static inline arith_buffer_t *arith_buffer(dl_list_t *l) {
  return (arith_buffer_t *)(((char *) l) + offsetof(arith_buffer_elem_t, buffer));
}

/*
 * Allocate an arithmetic buffer and insert it into the list
 */
static inline arith_buffer_t *alloc_arith_buffer(void) {
  arith_buffer_elem_t *new_elem;

  new_elem = (arith_buffer_elem_t *) safe_malloc(sizeof(arith_buffer_elem_t));
  list_insert_next(&arith_buffer_list, &new_elem->header);
  return &new_elem->buffer;
}

/*
 * Remove b from the list and free b
 */
static inline void free_arith_buffer(arith_buffer_t *b) {
  dl_list_t *elem;

  elem = arith_buffer_header(b);
  list_remove(elem);
  safe_free(elem);
}

/*
 * Clean up the arith buffer list: free all elements and empty the list
 */
static void free_arith_buffer_list(void) {
  dl_list_t *elem, *aux;

  elem = arith_buffer_list.next;
  while (elem != &arith_buffer_list) {
    aux = elem->next;
    delete_arith_buffer(arith_buffer(elem));
    safe_free(elem);
    elem = aux;
  }

  clear_list(&arith_buffer_list);
}



/********************************************
 *  BITVECTOR ARITHMETIC BUFFER ALLOCATION  *
 *******************************************/

/*
 * Get header of buffer b, assuming b is embedded into an bvarith_buffer_elem
 */
static inline dl_list_t *bvarith_buffer_header(bvarith_buffer_t *b) {
  return (dl_list_t *)(((char *)b) - offsetof(bvarith_buffer_elem_t, buffer));
}

/*
 * Get buffer of header l
 */
static inline bvarith_buffer_t *bvarith_buffer(dl_list_t *l) {
  return (bvarith_buffer_t *)(((char *) l) + offsetof(bvarith_buffer_elem_t, buffer));
}

/*
 * Allocate a bv-arithmetic buffer and insert it into the list
 */
static inline bvarith_buffer_t *alloc_bvarith_buffer(void) {
  bvarith_buffer_elem_t *new_elem;

  new_elem = (bvarith_buffer_elem_t *) safe_malloc(sizeof(bvarith_buffer_elem_t));
  list_insert_next(&bvarith_buffer_list, &new_elem->header);
  return &new_elem->buffer;
}

/*
 * Remove b from the list and free b
 */
static inline void free_bvarith_buffer(bvarith_buffer_t *b) {
  dl_list_t *elem;

  elem = bvarith_buffer_header(b);
  list_remove(elem);
  safe_free(elem);
}

/*
 * Clean up the arith buffer list: free all elements and empty the list
 */
static void free_bvarith_buffer_list(void) {
  dl_list_t *elem, *aux;

  elem = bvarith_buffer_list.next;
  while (elem != &bvarith_buffer_list) {
    aux = elem->next;
    delete_bvarith_buffer(bvarith_buffer(elem));
    safe_free(elem);
    elem = aux;
  }

  clear_list(&bvarith_buffer_list);
}



/*********************************
 *  BVARITH64 BUFFER ALLOCATION  *
 ********************************/

/*
 * Get header of buffer b, assuming b is embedded into an bvarith64_buffer_elem
 */
static inline dl_list_t *bvarith64_buffer_header(bvarith64_buffer_t *b) {
  return (dl_list_t *)(((char *)b) - offsetof(bvarith64_buffer_elem_t, buffer));
}

/*
 * Get buffer of header l
 */
static inline bvarith64_buffer_t *bvarith64_buffer(dl_list_t *l) {
  return (bvarith64_buffer_t *)(((char *) l) + offsetof(bvarith64_buffer_elem_t, buffer));
}

/*
 * Allocate a bv-arithmetic buffer and insert it into the list
 */
static inline bvarith64_buffer_t *alloc_bvarith64_buffer(void) {
  bvarith64_buffer_elem_t *new_elem;

  new_elem = (bvarith64_buffer_elem_t *) safe_malloc(sizeof(bvarith64_buffer_elem_t));
  list_insert_next(&bvarith64_buffer_list, &new_elem->header);
  return &new_elem->buffer;
}

/*
 * Remove b from the list and free b
 */
static inline void free_bvarith64_buffer(bvarith64_buffer_t *b) {
  dl_list_t *elem;

  elem = bvarith64_buffer_header(b);
  list_remove(elem);
  safe_free(elem);
}

/*
 * Clean up the buffer list: free all elements and empty the list
 */
static void free_bvarith64_buffer_list(void) {
  dl_list_t *elem, *aux;

  elem = bvarith64_buffer_list.next;
  while (elem != &bvarith_buffer_list) {
    aux = elem->next;
    delete_bvarith64_buffer(bvarith64_buffer(elem));
    safe_free(elem);
    elem = aux;
  }

  clear_list(&bvarith64_buffer_list);
}



/*****************************
 *  LOGIC BUFFER ALLOCATION  *
 ****************************/

/*
 * Get header of buffer b, assuming b is embedded into an bvlogic_buffer_elem
 */
static inline dl_list_t *bvlogic_buffer_header(bvlogic_buffer_t *b) {
  return (dl_list_t *)(((char *)b) - offsetof(bvlogic_buffer_elem_t, buffer));
}

/*
 * Get buffer of header l
 */
static inline bvlogic_buffer_t *bvlogic_buffer(dl_list_t *l) {
  return (bvlogic_buffer_t *)(((char *) l) + offsetof(bvlogic_buffer_elem_t, buffer));
}

/*
 * Allocate an arithmetic buffer and insert it into the list
 */
static inline bvlogic_buffer_t *alloc_bvlogic_buffer(void) {
  bvlogic_buffer_elem_t *new_elem;

  new_elem = (bvlogic_buffer_elem_t *) safe_malloc(sizeof(bvlogic_buffer_elem_t));
  list_insert_next(&bvlogic_buffer_list, &new_elem->header);
  return &new_elem->buffer;
}

/*
 * Remove b from the list and free b
 */
static inline void free_bvlogic_buffer(bvlogic_buffer_t *b) {
  dl_list_t *elem;

  elem = bvlogic_buffer_header(b);
  list_remove(elem);
  safe_free(elem);
}

/*
 * Clean up the arith buffer list: free all elements and empty the list
 */
static void free_bvlogic_buffer_list(void) {
  dl_list_t *elem, *aux;

  elem = bvlogic_buffer_list.next;
  while (elem != &bvlogic_buffer_list) {
    aux = elem->next;
    delete_bvlogic_buffer(bvlogic_buffer(elem));
    safe_free(elem);    
    elem = aux;
  }

  clear_list(&bvlogic_buffer_list);
}






/***********************
 *  INTERNAL BUFFERS   *
 **********************/

/*
 * Return the internal arithmetic buffer
 * - allocate it if needed
 */
static arith_buffer_t *get_internal_arith_buffer(void) {
  arith_buffer_t *b;

  b = internal_arith_buffer;
  if (b == NULL) {
    b = alloc_arith_buffer();
    init_arith_buffer(b, &pprods, &arith_store);
    internal_arith_buffer = b;
  }

  return b;
}


/*
 * Same thing: internal_bvarith buffer
 */
static bvarith_buffer_t *get_internal_bvarith_buffer(void) {
  bvarith_buffer_t *b;

  b = internal_bvarith_buffer;
  if (b == NULL) {
    b = alloc_bvarith_buffer();
    init_bvarith_buffer(b, &pprods, &bvarith_store);
    internal_bvarith_buffer = b;
  }

  return b;
}


/*
 * Same thing: internal_bvarith64 buffer
 */
static bvarith64_buffer_t *get_internal_bvarith64_buffer(void) {
  bvarith64_buffer_t *b;

  b = internal_bvarith64_buffer;
  if (b == NULL) {
    b = alloc_bvarith64_buffer();
    init_bvarith64_buffer(b, &pprods, &bvarith64_store);
    internal_bvarith64_buffer = b;
  }

  return b;
}


/*
 * Same thing: internal_bvlogic buffer
 */
static bvlogic_buffer_t *get_internal_bvlogic_buffer(void) {
  bvlogic_buffer_t *b;

  b = internal_bvlogic_buffer;
  if (b == NULL) {
    b = alloc_bvlogic_buffer();
    init_bvlogic_buffer(b, &nodes);
    internal_bvlogic_buffer = b;
  }

  return b;
}





/***************************************
 *  GLOBAL INITIALIZATION AND CLEANUP  *
 **************************************/

/*
 * Initialize the table of global objects
 */
static void init_globals(yices_globals_t *glob) {
  glob->types = &types;
  glob->terms = &terms;
  glob->pprods = &pprods;
  glob->nodes = &nodes;
  glob->arith_store = &arith_store;
  glob->bvarith_store = &bvarith_store;
  glob->bvarith64_store = &bvarith64_store;
}


/*
 * Reset all to NULL
 */
static void clear_globals(yices_globals_t *glob) {
  glob->types = NULL;
  glob->terms = NULL;
  glob->pprods = NULL;
  glob->nodes = NULL;
  glob->arith_store = NULL;
  glob->bvarith_store = NULL;
  glob->bvarith64_store = NULL;
}


/*
 * Initialize all global objects
 */
EXPORTED void yices_init(void) {
  error.code = NO_ERROR;

  init_bvconstants();
  init_bvconstant(&bv0);
  init_bvconstant(&bv1);
  init_bvconstant(&bv2);

  init_rationals();
  q_init(&r0);
  q_init(&r1);

  init_ivector(&vector0, 10);

  // tables
  init_type_table(&types, INIT_TYPE_SIZE);
  init_pprod_table(&pprods, 0);
  init_node_table(&nodes, 0);
  init_term_table(&terms, INIT_TERM_SIZE, &types, &pprods);

  // object stores
  init_mlist_store(&arith_store);
  init_bvmlist_store(&bvarith_store);
  init_bvmlist64_store(&bvarith64_store);

  // buffer lists
  clear_list(&arith_buffer_list);
  clear_list(&bvarith_buffer_list);
  clear_list(&bvarith64_buffer_list);
  clear_list(&bvlogic_buffer_list);

  // internal buffers (allocated on demand)
  internal_arith_buffer = NULL;
  internal_bvarith_buffer = NULL;
  internal_bvarith64_buffer = NULL;
  internal_bvlogic_buffer = NULL;

  // prepare the global table
  init_globals(&__yices_globals);
}


/*
 * Cleanup: delete all tables and internal data structures
 */
EXPORTED void yices_cleanup(void) {
  clear_globals(&__yices_globals);

  // internal buffers will be freed via free_arith_buffer_list,
  // free_bvarith_buffer_list, and free_bvlogic_buffer_list
  internal_arith_buffer = NULL;
  internal_bvarith_buffer = NULL;
  internal_bvarith64_buffer = NULL;
  internal_bvlogic_buffer = NULL;

  free_bvlogic_buffer_list();
  free_bvarith_buffer_list();
  free_bvarith64_buffer_list();
  free_arith_buffer_list();

  delete_term_table(&terms);
  delete_node_table(&nodes);
  delete_pprod_table(&pprods);
  delete_type_table(&types);

  delete_mlist_store(&arith_store);
  delete_bvmlist_store(&bvarith_store);
  delete_bvmlist64_store(&bvarith64_store);

  delete_ivector(&vector0);

  q_clear(&r0); // not necessary
  q_clear(&r1);
  cleanup_rationals();

  delete_bvconstant(&bv2);
  delete_bvconstant(&bv1);
  delete_bvconstant(&bv0);
  cleanup_bvconstants();
}



/*
 * Get the last error report
 */
EXPORTED error_report_t *yices_get_error_report(void) {
  return &error;
}


/*
 * Get the last error code
 */
EXPORTED error_code_t yices_get_error_code(void) {
  return error.code;
}

/*
 * Clear the last error report
 */
EXPORTED void yices_clear_error(void) {
  error.code = NO_ERROR;
}




/***********************
 *  BUFFER ALLOCATION  *
 **********************/

/*
 * These functions are not part of the API.
 * They are exported to be used by other yices modules.
 */

/*
 * Allocate an arithmetic buffer, initialized to the zero polynomial.
 * Add it to the buffer list
 */
arith_buffer_t *yices_new_arith_buffer() {
  arith_buffer_t *b;
  
  b = alloc_arith_buffer();
  init_arith_buffer(b, &pprods, &arith_store);
  return b;
}


/*
 * Free an allocated buffer
 */
void yices_free_arith_buffer(arith_buffer_t *b) {
  delete_arith_buffer(b);
  free_arith_buffer(b);
}


/*
 * Allocate and initialize a bvarith_buffer
 * - the buffer is initialized to 0b0...0 (with n bits)
 * - n must be positive and no more than YICES_MAX_BVSIZE
 */
bvarith_buffer_t *yices_new_bvarith_buffer(uint32_t n) {
  bvarith_buffer_t *b;

  b = alloc_bvarith_buffer();
  init_bvarith_buffer(b, &pprods, &bvarith_store);
  bvarith_buffer_prepare(b, n);

  return b;
}


/*
 * Free an allocated bvarith_buffer
 */
void yices_free_bvarith_buffer(bvarith_buffer_t *b) {
  delete_bvarith_buffer(b);
  free_bvarith_buffer(b);
}


/*
 * Allocate and initialize a bvarith64_buffer
 * - the buffer is initialized to 0b0000..0 (with n bits)
 * - n must be between 1 and 64
 */
bvarith64_buffer_t *yices_new_bvarith64_buffer(uint32_t n) {
  bvarith64_buffer_t *b;

  b = alloc_bvarith64_buffer();
  init_bvarith64_buffer(b, &pprods, &bvarith64_store);
  bvarith64_buffer_prepare(b, n);

  return b;
}


/*
 * Free an allocated bvarith64_buffer
 */
void yices_free_bvarith64_buffer(bvarith64_buffer_t *b) {
  delete_bvarith64_buffer(b);
  free_bvarith64_buffer(b);
}


/*
 * Allocate and initialize a bvlogic buffer
 * - the buffer is empty (bitsize = 0)
 */
bvlogic_buffer_t *yices_new_bvlogic_buffer(void) {
  bvlogic_buffer_t *b;

  b = alloc_bvlogic_buffer();
  init_bvlogic_buffer(b, &nodes);

  return b;
}


/*
 * Free buffer b allocated by the previous function
 */
void yices_free_bvlogic_buffer(bvlogic_buffer_t *b) {
  bvlogic_buffer_clear(b);
  delete_bvlogic_buffer(b);
  free_bvlogic_buffer(b);
}



/***********************************************
 *  CONVERSION OF ARITHMETIC BUFFERS TO TERMS  *
 **********************************************/

/*
 * These functions are not part of the API.
 * They are exported to be used by other yices modules.
 */

/*
 * Convert b to a term and reset b.
 *
 * Normalize b first then apply the following simplification rules:
 * 1) if b is a constant, then a constant rational is created
 * 2) if b is of the form 1.t then t is returned
 * 3) if b is of the form 1.t_1^d_1 x ... x t_n^d_n, then a power product is returned
 * 4) otherwise, a polynomial term is returned
 */
term_t arith_buffer_get_term(arith_buffer_t *b) {
  mlist_t *m;
  pprod_t *r;
  uint32_t n;
  term_t t;

  assert(b->ptbl == &pprods);

  arith_buffer_normalize(b);

  n = b->nterms;
  if (n == 0) {
    t = zero_term;
  } else if (n == 1) {
    m = b->list; // unique monomial of b
    r = m->prod;
    if (r == empty_pp) {
      // constant polynomial
      t = arith_constant(&terms, &m->coeff);
    } else if (q_is_one(&m->coeff)) {
      // term or power product
      t =  pp_is_var(r) ? var_of_pp(r) : pprod_term(&terms, r);
    } else {
    // can't simplify
      t = arith_poly(&terms, b);
    }
  } else {
    t = arith_poly(&terms, b);
  }

  arith_buffer_reset(b);
  assert(good_term(&terms, t) && is_arithmetic_term(&terms, t));

  return t;
}


/*
 * Construct the atom (b == 0) then reset b.
 *
 * Normalize b first.
 * - simplify to true if b is the zero polynomial
 * - simplify to false if b is constant and non-zero
 * - rewrite to (t1 == t2) if that's possible.
 * - otherwise, create a polynomial term t from b
 *   and return the atom (t == 0).
 */
term_t arith_buffer_get_eq0_atom(arith_buffer_t *b) {
  pprod_t *r1, *r2;
  term_t t1, t2, t;

  assert(b->ptbl == &pprods);

  arith_buffer_normalize(b);

  if (arith_buffer_is_zero(b)) {
    t = true_term;
  } else if (arith_buffer_is_nonzero(b)) {
    t = false_term;
  } else {
    r1 = empty_pp;
    r2 = empty_pp;
    if (arith_buffer_is_equality(b, &r1, &r2)) {
      // convert to (t1 == t2)
      t1 = pp_is_var(r1) ? var_of_pp(r1) : pprod_term(&terms, r1);
      t2 = pp_is_var(r2) ? var_of_pp(r2) : pprod_term(&terms, r2);
    
      // normalize
      if (t1 > t2) {
	t = t1; t1 = t2; t2 = t;
      }

      t = arith_bineq_atom(&terms, t1, t2);
    } else {
      t = arith_poly(&terms, b);
      t = arith_eq_atom(&terms, t);
    }
  }

  arith_buffer_reset(b);
  assert(good_term(&terms, t) && is_boolean_term(&terms, t));

  return t;
}


/*
 * Construct the atom (b >= 0) then reset b.
 *
 * Normalize b first then check for simplifications.
 * - simplify to true or false if b is a constant
 * - otherwise term t from b and return the atom (t >= 0)
 */
term_t arith_buffer_get_geq0_atom(arith_buffer_t *b) {
  term_t t;

  assert(b->ptbl == &pprods);

  arith_buffer_normalize(b);

  if (arith_buffer_is_nonneg(b)) {
    t = true_term;
  } else if (arith_buffer_is_neg(b)) {
    t = false_term;
  } else {
    t = arith_poly(&terms, b);
    t = arith_geq_atom(&terms, t);
  }

  arith_buffer_reset(b);
  assert(good_term(&terms, t) && is_boolean_term(&terms, t));

  return t;
}


/*
 * Atom (b <= 0): rewritten to (-b >= 0)
 */
term_t arith_buffer_get_leq0_atom(arith_buffer_t *b) {
  term_t t;

  assert(b->ptbl == &pprods);

  arith_buffer_normalize(b);

  if (arith_buffer_is_nonpos(b)) {
    t = true_term;
  } else if (arith_buffer_is_pos(b)) {
    t = false_term;
  } else {
    arith_buffer_negate(b); // b remains normalized
    t = arith_poly(&terms, b);
    t = arith_geq_atom(&terms, t);
  }

  arith_buffer_reset(b);
  assert(good_term(&terms, t) && is_boolean_term(&terms, t));

  return t;
}


/*
 * Atom (b > 0): rewritten to (not (b <= 0))
 */
term_t arith_buffer_get_gt0_atom(arith_buffer_t *b) {
  term_t t;

  t = arith_buffer_get_leq0_atom(b);
#ifndef NDEBUG
  return not_term(&terms, t);
#else 
  return opposite_term(t);
#endif
}


/*
 * Atom (b < 0): rewritten to (not (b >= 0))
 */
term_t arith_buffer_get_lt0_atom(arith_buffer_t *b) {
  term_t t;

  t = arith_buffer_get_geq0_atom(b);
#ifndef NDEBUG
  return not_term(&terms, t);
#else 
  return opposite_term(t);
#endif
}



/********************************************
 *  CONVERSION OF BVLOGIC BUFFERS TO TERMS  *
 *******************************************/

/*
 * These functions are not part of the API.
 * They are exported to be used by other yices modules.
 */

/*
 * Convert buffer b to a bv_constant term
 * - side effect: use bv0
 */
static term_t bvlogic_buffer_get_bvconst(bvlogic_buffer_t *b) {
  assert(bvlogic_buffer_is_constant(b));

  bvlogic_buffer_get_constant(b, &bv0);
  return bvconst_term(&terms, bv0.bitsize, bv0.data);
}


/*
 * Convert buffer b to a bv-array term
 * - side effect: use vector0
 */
static term_t bvlogic_buffer_get_bvarray(bvlogic_buffer_t *b) {
  uint32_t i, n;

  assert(b->nodes == &nodes);

  // translate each bit of b into a boolean term
  // we store the translation in b->bit
  n = b->bitsize;
  for (i=0; i<n; i++) {
    b->bit[i] = convert_bit_to_term(&terms, &nodes, &vector0, b->bit[i]);
  }

  // build the term (bvarray b->bit[0] ... b->bit[n-1])
  return bvarray_term(&terms, n, b->bit);
}


/*
 * Convert b to a term then reset b.
 * - b must not be empty.
 * - build a bitvector constant if possible
 * - if b is of the form (select 0 t) ... (select k t) and t has bitsize (k+1)
 *   then return t
 * - otherwise build a bitarray term
 */
term_t bvlogic_buffer_get_term(bvlogic_buffer_t *b) {
  term_t t;
  uint32_t n;

  n = b->bitsize;
  assert(n > 0);
  if (bvlogic_buffer_is_constant(b)) {
    if (n <= 64) {
      // small constant
      t = bv64_constant(&terms, n, bvlogic_buffer_get_constant64(b));
    } else {
      // wide constant
      t = bvlogic_buffer_get_bvconst(b);
    }

  } else {
    t = bvlogic_buffer_get_var(b);
    if (t < 0 || term_bitsize(&terms, t) != n) {
      // not a variable
      t = bvlogic_buffer_get_bvarray(b);
    }
  }

  assert(is_bitvector_term(&terms, t) && term_bitsize(&terms, t) == n);

  bvlogic_buffer_clear(b);
  
  return n;
}




/********************************************
 *  CONVERSION OF BVARITH BUFFERS TO TERMS  *
 *******************************************/

/*
 * Store array [false_term, ..., false_term] into vector v
 */
static void bvarray_set_zero_bv(ivector_t *v, uint32_t n) {
  uint32_t i;

  assert(0 < n && n <= YICES_MAX_BVSIZE);
  resize_ivector(v, n);
  for (i=0; i<n; i++) {
    v->data[i] = false_term;
  }
}

/*
 * Store constant c into vector v
 */
static void bvarray_copy_constant(ivector_t *v, uint32_t n, uint32_t *c) {
  uint32_t i;

  assert(0 < n && n <= YICES_MAX_BVSIZE);
  resize_ivector(v, n);
  for (i=0; i<n; i++) {
    v->data[i] = bool2term(bvconst_tst_bit(c, i));
  }
}

/*
 * Same thing for a small constant c
 */
static void bvarray_copy_constant64(ivector_t *v, uint32_t n, uint64_t c) {
  uint32_t i;

  assert(0 < n && n <= 64);
  resize_ivector(v, n);
  for (i=0; i<n; i++) {
    v->data[i] = bool2term(tst_bit64(c, i));
  }
}


/*
 * Check whether v + a * x can be converted to (v | (x << k))  for some k
 * - a must be an array of n boolean terms
 * - return true if that can be done and update v to (v | (x << k))
 * - otherwise, return false and keep v unchanged
 */
static bool bvarray_check_addmul(ivector_t *v, uint32_t n, uint32_t *c, term_t *a) {
  uint32_t i, w;
  int32_t k;

  w = (n + 31) >> 5; // number of words in c
  if (bvconst_is_zero(c, w)) {
    return true;
  }

  k = bvconst_is_power_of_two(c, w);
  if (k < 0) {
    return false;
  }

  // c is 2^k check whether v + (a << k) is equal to v | (a << k)
  assert(0 <= k && k < n);
  for (i=k; i<n; i++) {
    if (v->data[i] != false_term && a[i-k] != false_term) {
      return false;
    }
  }

  // update v here
  for (i=k; i<n; i++) {
    if (a[i-k] != false_term) {
      assert(v->data[i] == false_term);
      v->data[i] = v->data[i-k];
    }
  }

  return true;
}


/*
 * Same thing for c stored as a small constant (64 bits at most)
 */
static bool bvarray_check_addmul64(ivector_t *v, uint32_t n, uint64_t c, term_t *a) {
  uint32_t i, k;

  assert(0 < n && n <= 64 && c == norm64(c, n));

  if (c == 0) {
    return true;
  }

  k = ctz64(c); // k = index of the rightmost 1 in c
  assert(0 <= k && k <= 63);
  if (c != (((uint64_t) 1) << k)) {
    // c != 2^k
    return false;
  }

  // c is 2^k check whether v + (a << k) is equal to v | (a << k)
  assert(0 <= k && k < n);
  for (i=k; i<n; i++) {
    if (v->data[i] != false_term && a[i-k] != false_term) {
      return false;
    }
  }

  // update v here
  for (i=k; i<n; i++) {
    if (a[i-k] != false_term) {
      assert(v->data[i] == false_term);
      v->data[i] = v->data[i-k];
    }
  }

  return true;
}




/*
 * Check whether power product r is equal to a bit-array term t
 * - if so return t's descriptor, otherwise return NULL
 */
static composite_term_t *pprod_get_bvarray(pprod_t *r) {  
  composite_term_t *bv;
  term_t t;

  bv = NULL;
  if (pp_is_var(r)) {
    t = var_of_pp(r);
    if (term_kind(&terms, t) == BV_ARRAY) {
      bv = composite_for_idx(&terms, index_of(t));
    }
  }

  return bv;
}

/*
 * Attempt to convert a bvarith buffer to a bv-array term
 * - b = bvarith buffer (list of monomials)
 * - return NULL_TERM if the conversion fails
 * - return a term t if the conversion succeeds.
 * - side effect: use vector0
 */
static term_t convert_bvarith_to_bvarray(bvarith_buffer_t *b) {
  composite_term_t *bv;
  bvmlist_t *m;
  uint32_t n;

  n = b->bitsize;
  m = b->list; // first monomial
  if (m->prod == empty_pp) {
    // copy constant into vector0
    bvarray_copy_constant(&vector0, n, m->coeff);
    m = m->next;
  } else {
    // initialze vector0 to 0
    bvarray_set_zero_bv(&vector0, n);
  }

  while (m->next != NULL) {
    bv = pprod_get_bvarray(m->prod);
    if (bv == NULL) return NULL_TERM;

    assert(bv->arity == n);

    // try to convert coeff * v into shift + bitwise or 
    if (! bvarray_check_addmul(&vector0, n, m->coeff, bv->arg)) {
      return NULL_TERM;  // conversion failed
    }
    m = m->next;
  }

  // Success: construct a bit array from log_buffer
  return bvarray_term(&terms, n, vector0.data);
}


/*
 * Attempt to convert a bvarith64 buffer to a bv-array term
 * - b = bvarith buffer (list of monomials)
 * - return NULL_TERM if the conversion fails
 * - return a term t if the conversion succeeds.
 * - side effect: use vector0
 */
static term_t convert_bvarith64_to_bvarray(bvarith64_buffer_t *b) {
  composite_term_t *bv;
  bvmlist64_t *m;
  uint32_t n;

  n = b->bitsize;
  m = b->list; // first monomial
  if (m->prod == empty_pp) {
    // copy constant into vector0
    bvarray_copy_constant64(&vector0, n, m->coeff);
    m = m->next;
  } else {
    // initialze vector0 to 0
    bvarray_set_zero_bv(&vector0, n);
  }

  while (m->next != NULL) {
    bv = pprod_get_bvarray(m->prod);
    if (bv == NULL) return NULL_TERM;

    assert(bv->arity == n);

    // try to convert coeff * v into shift + bitwise or 
    if (! bvarray_check_addmul64(&vector0, n, m->coeff, bv->arg)) {
      return NULL_TERM;  // conversion failed
    }
    m = m->next;
  }

  // Success: construct a bit array from log_buffer
  return bvarray_term(&terms, n, vector0.data);
}


/*
 * Constant bitvector with all bits 0
 * - n = bitsize (must satisfy 0 < n && n <= YICES_MAX_BVSIZE)
 * - side effect: modify bv0
 */
static term_t make_zero_bv(uint32_t n) {
  assert(0 < n && n <= YICES_MAX_BVSIZE);
  bvconstant_set_all_zero(&bv0, n);
  return bvconst_term(&terms, bv0.bitsize, bv0.data);
}


/*
 * These functions are not part of the API.
 * They are exported to be used by other yices modules.
 */

/*
 * Normalize b then convert it to a term and reset b
 *
 * if b is reduced to a single variable x, return the term attached to x
 * if b is reduced to a power product, return that
 * if b is constant, build a BV_CONSTANT term
 * if b can be converted to a BV_ARRAY term do it
 * otherwise construct a BV_POLY
 */
term_t bvarith_buffer_get_term(bvarith_buffer_t *b) {
  bvmlist_t *m;
  pprod_t *r;
  uint32_t n, p, k;
  term_t t;

  assert(b->bitsize > 0);
  
  bvarith_buffer_normalize(b);

  n = b->bitsize;
  k = (n + 31) >> 5;
  p = b->nterms;
  if (p == 0) {
    // zero 
    t = make_zero_bv(n);
    goto done;
  }

  if (p == 1) {
    m = b->list; // unique monomial of b
    r = m->prod;
    if (r == empty_pp) {
      // constant 
      t = bvconst_term(&terms, n, m->coeff);
      goto done;
    }
    if (bvconst_is_one(m->coeff, k)) {
      // power product
      t = pp_is_var(r) ? var_of_pp(r) : pprod_term(&terms, r);
      goto done;
    } 
  }

  // try to convert to a bvarray term
  t = convert_bvarith_to_bvarray(b);
  if (t == NULL_TERM) {
    // conversion failed: build a bvpoly
    t = bv_poly(&terms, b);
  }

 done:
  bvarith_buffer_prepare(b, 32); // reset b, any positive n would do
  assert(is_bitvector_term(&terms, t) && term_bitsize(&terms, t) == n);

  return t;  
}



/*
 * Normalize b then convert it to a term and reset b
 *
 * if b is reduced to a single variable x, return the term attached to x
 * if b is reduced to a power product, return that
 * if b is constant, build a BV64_CONSTANT term
 * if b can be converted to a BV_ARRAY term do it
 * otherwise construct a BV64_POLY
 */
term_t bvarith64_buffer_get_term(bvarith64_buffer_t *b) {
  bvmlist64_t *m;
  pprod_t *r;
  uint32_t n, p;
  term_t t;

  assert(b->bitsize > 0);
  
  bvarith64_buffer_normalize(b);

  n = b->bitsize;
  p = b->nterms;
  if (p == 0) {
    // zero 
    t = make_zero_bv(n);
    goto done;
  }

  if (p == 1) {
    m = b->list; // unique monomial of b
    r = m->prod;
    if (r == empty_pp) {
      // constant 
      t = bv64_constant(&terms, n, m->coeff);
      goto done;
    }
    if (m->coeff == 1) {
      // power product
      t = pp_is_var(r) ? var_of_pp(r) : pprod_term(&terms, r);
      goto done;
    }
  }

  // try to convert to a bvarray term
  t = convert_bvarith64_to_bvarray(b);
  if (t == NULL_TERM) {
    // conversion failed: build a bvpoly
    t = bv64_poly(&terms, b);
  }

 done:
  bvarith64_buffer_prepare(b, 32); // reset b, any positive n would do
  assert(is_bitvector_term(&terms, t) && term_bitsize(&terms, t) == n);

  return t;  
}



/******************
 *  TYPECHECKING  *
 *****************/

/*
 * All check_ functions return true if the check succeeds.
 * Otherwise they return false and set the error code and diagnostic data.
 */
// Check whether n is positive
static bool check_positive(uint32_t n) {
  if (n == 0) {
    error.code = POS_INT_REQUIRED;
    error.badval = n;
    return false;
  }
  return true;
}

// Check whether n is less than YICES_MAX_ARITY
static bool check_arity(uint32_t n) {
  if (n > YICES_MAX_ARITY) {
    error.code = TOO_MANY_ARGUMENTS;
    error.badval = n;
    return false;
  }
  return true;
}

// Check whether n is less than YICES_MAX_VARS
static bool check_maxvars(uint32_t n) {
  if (n > YICES_MAX_VARS) {
    error.code = TOO_MANY_VARS;
    error.badval = n;
    return false;
  }
  return true;
}

// Check whether n is less than YICES_MAX_BVSIZE
static bool check_maxbvsize(uint32_t n) {
  if (n > YICES_MAX_BVSIZE) {
    error.code = MAX_BVSIZE_EXCEEDED;
    error.badval = n;
    return false;    
  }
  return true;
}

// Check whether d is no more than YICES_MAX_DEGREE
static bool check_maxdegree(uint32_t d) {
  if (d > YICES_MAX_DEGREE) {
    error.code = DEGREE_OVERFLOW;
    error.badval = d;
    return false;
  }
  return true;
}

// Check whether tau is a valid type
static bool check_good_type(type_table_t *tbl, type_t tau) {
  if (bad_type(tbl, tau)) { 
    error.code = INVALID_TYPE;
    error.type1 = tau;
    error.index = -1;
    return false;
  }
  return true;
}

// Check whether all types in a[0 ... n-1] are valid
static bool check_good_types(type_table_t *tbl, uint32_t n, type_t *a) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (bad_type(tbl, a[i])) {
      error.code = INVALID_TYPE;
      error.type1 = a[i];
      error.index = i;
      return false;
    }
  }
  return true;
}

// Check whether tau is uninterpreted or scalar and whether 
// i a valid constant index for type tau.
static bool check_good_constant(type_table_t *tbl, type_t tau, int32_t i) {
  type_kind_t kind;

  if (bad_type(tbl, tau)) {
    error.code = INVALID_TYPE;
    error.type1 = tau;
    error.index = -1;
    return false;
  }
  kind = type_kind(tbl, tau);
  if (kind != UNINTERPRETED_TYPE && kind != SCALAR_TYPE) {
    error.code = SCALAR_OR_UTYPE_REQUIRED;
    error.type1 = tau;
    return false;
  }
  if (i < 0 || 
      (kind == SCALAR_TYPE && i >= scalar_type_cardinal(tbl, tau))) {
    error.code = INVALID_CONSTANT_INDEX;
    error.type1 = tau;
    error.badval = i;
    return false;
  }
  return true;
}

// Check whether t is a valid term
static bool check_good_term(term_table_t *tbl, term_t t) {
  if (bad_term(tbl, t)) {
    error.code = INVALID_TERM;
    error.term1 = t;
    error.index = -1;
    return false;
  }
  return true;
}

// Check that terms in a[0 ... n-1] are valid
static bool check_good_terms(term_table_t *tbl, uint32_t n, term_t *a) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (bad_term(tbl, a[i])) {
      error.code = INVALID_TERM;
      error.term1 = a[i];
      error.index = i;
      return false;
    }
  }
  return true;
}

// check that terms a[0 ... n-1] have types that match tau[0 ... n-1].
static bool check_arg_types(term_table_t *tbl, uint32_t n, term_t *a, type_t *tau) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (! is_subtype(tbl->types, term_type(tbl, a[i]), tau[i])) {
      error.code = TYPE_MISMATCH;
      error.term1 = a[i];
      error.type1 = tau[i];
      error.index = i;
      return false;
    }
  }

  return true;
}

// check whether (f a[0] ... a[n-1]) is typecorrect
static bool check_good_application(term_table_t *tbl, term_t f, uint32_t n, term_t *a) {
  function_type_t *ft;

  if (! check_positive(n) || 
      ! check_good_term(tbl, f) || 
      ! check_good_terms(tbl, n, a)) {
    return false;
  }

  if (! is_function_term(tbl, f)) {
    error.code = FUNCTION_REQUIRED;
    error.term1 = f;
    return false;
  }  

  ft = function_type_desc(tbl->types, term_type(tbl, f));
  if (n != ft->ndom) {
    error.code = WRONG_NUMBER_OF_ARGUMENTS;
    error.type1 = term_type(tbl, f);
    error.badval = n;
    return false;
  }

  return check_arg_types(tbl, n, a, ft->domain);
}

// Check whether t is a boolean term. t must be a valid term
static bool check_boolean_term(term_table_t *tbl, term_t t) {
  if (! is_boolean_term(tbl, t)) {
    error.code = TYPE_MISMATCH;
    error.term1 = t;
    error.type1 = bool_type(tbl->types);
    error.index = -1;
    return false;
  }
  return true;
}


// Check whether t is an arithmetic term, t must be valid.
static bool check_arith_term(term_table_t *tbl, term_t t) {
  if (! is_arithmetic_term(tbl, t)) {
    error.code = ARITHTERM_REQUIRED;
    error.term1 = t;
    return false;
  }
  return true;
}


// Check whether t is a bitvector term, t must be valid
static bool check_bitvector_term(term_table_t *tbl, term_t t) {
  if (! is_bitvector_term(tbl, t)) {
    error.code = BITVECTOR_REQUIRED;
    error.term1 = t;
    return false;
  }
  return true;
}


// Check whether t1 and t2 have compatible types (i.e., (= t1 t2) is well-typed)
// t1 and t2 must both be valid
static bool check_compatible_terms(term_table_t *tbl, term_t t1, term_t t2) {
  type_t tau1, tau2;

  tau1 = term_type(tbl, t1);
  tau2 = term_type(tbl, t2);
  if (! compatible_types(tbl->types, tau1, tau2)) {
    error.code = INCOMPATIBLE_TYPES;
    error.term1 = t1;
    error.type1 = tau1;
    error.term2 = t2;
    error.type2 = tau2;
    return false;
  }

  return true;
}


// Check whether (= t1 t2) is type correct
static bool check_good_eq(term_table_t *tbl, term_t t1, term_t t2) {
  return check_good_term(tbl, t1) && check_good_term(tbl, t2) &&
    check_compatible_terms(tbl, t1, t2);
}

// Check whether t1 and t2 are two valid arithmetic terms
static bool check_both_arith_terms(term_table_t *tbl, term_t t1, term_t t2) {
  return check_good_term(tbl, t1) && check_good_term(tbl, t2) &&
    check_arith_term(tbl, t1) && check_arith_term(tbl, t2);
}

// Check that t1 and t2 are bitvectors of the same size
static bool check_compatible_bv_terms(term_table_t *tbl, term_t t1, term_t t2) {
  return check_good_term(tbl, t1) && check_good_term(tbl, t2)
    && check_bitvector_term(tbl, t1) && check_bitvector_term(tbl, t2) 
    && check_compatible_terms(tbl, t1, t2);
}


// Check whether terms a[0 ... n-1] are all boolean
static bool check_boolean_args(term_table_t *tbl, uint32_t n, term_t *a) {
  uint32_t i;

  for (i=0; i<n; i++) {
    if (! is_boolean_term(tbl, a[i])) {
      error.code = TYPE_MISMATCH;
      error.term1 = a[i];
      error.type1 = bool_type(tbl->types);
      error.index = i;
      return false;
    }
  }

  return true;
}


// Check whether (tuple-select i t) is well-typed
static bool check_good_select(term_table_t *tbl, uint32_t i, term_t t) {
  type_t tau;

  if (! check_good_term(tbl, t)) {
    return false;
  }

  tau = term_type(tbl, t);
  if (type_kind(tbl->types, tau) != TUPLE_TYPE) {
    error.code = TUPLE_REQUIRED;
    error.term1 = t;
    return false;
  }

  if (i >= tuple_type_arity(tbl->types, tau)) {
    error.code = INVALID_TUPLE_INDEX;
    error.type1 = tau;
    error.badval = i;
    return false;
  }
  
  return true;
}

// Check that (update f (a_1 ... a_n) v) is well typed
static bool check_good_update(term_table_t *tbl, term_t f, uint32_t n, term_t *a, term_t v) {
  function_type_t *ft;

  if (! check_positive(n) || 
      ! check_good_term(tbl, f) ||
      ! check_good_term(tbl, v) ||
      ! check_good_terms(tbl, n, a)) {
    return false;
  }

  if (! is_function_term(tbl, f)) {
    error.code = FUNCTION_REQUIRED;
    error.term1 = f;
    return false;
  }  

  ft = function_type_desc(tbl->types, term_type(tbl, f));
  if (n != ft->ndom) {
    error.code = WRONG_NUMBER_OF_ARGUMENTS;
    error.type1 = term_type(tbl, f);
    error.badval = n;
    return false;
  }

  if (! is_subtype(tbl->types, term_type(tbl, v), ft->range)) {
    error.code = TYPE_MISMATCH;
    error.term1 = v;
    error.type1 = ft->range;
    error.index = -1;
    return false;
  }

  return check_arg_types(tbl, n, a, ft->domain);
}


// Check (distinct t_1 ... t_n)
static bool check_good_distinct_term(term_table_t *tbl, uint32_t n, term_t *a) {
  uint32_t i;
  type_t tau;

  if (! check_positive(n) ||
      ! check_arity(n) ||
      ! check_good_terms(tbl, n, a)) {
    return false;
  }

  tau = term_type(tbl, a[0]); 
  for (i=1; i<n; i++) {
    tau = super_type(tbl->types, tau, term_type(tbl, a[i]));
    if (tau == NULL_TYPE) {
      error.code = INCOMPATIBLE_TYPES;
      error.term1 = a[0];
      error.type1 = term_type(tbl, a[0]);
      error.term2 = a[i];
      error.type2 = term_type(tbl, a[i]);
      return false;
    }
  }

  return true;
}

// Check quantified formula (FORALL/EXISTS (v_1 ... v_n) body)
// v must be sorted.
static bool check_good_quantified_term(term_table_t *tbl, uint32_t n, term_t *v, term_t body) {
  int32_t i;

  if (! check_positive(n) ||
      ! check_maxvars(n) ||
      ! check_good_term(tbl, body) ||
      ! check_good_terms(tbl, n, v) ||
      ! check_boolean_term(tbl, body)) {
    return false;
  }

  for (i=0; i<n; i++) {
    if (term_kind(tbl, v[i]) != VARIABLE) {      
      error.code = VARIABLE_REQUIRED;
      error.term1 = v[i];
      error.index = i;
      return false;
    }
  }

  for (i=1; i<n; i++) {
    if (v[i-1] == v[i]) {
      error.code = DUPLICATE_VARIABLE;
      error.term1 = v[i];
      error.index = i;
      return false;
    }
  }

  return true;
}


// Check whether (tuple-select i t v) is well-typed
static bool check_good_tuple_update(term_table_t *tbl, uint32_t i, term_t t, term_t v) {
  type_t tau;
  tuple_type_t *desc;

  if (! check_good_term(tbl, t) ||
      ! check_good_term(tbl, v)) {
    return false;
  }

  tau = term_type(tbl, t);
  if (type_kind(tbl->types, tau) != TUPLE_TYPE) {
    error.code = TUPLE_REQUIRED;
    error.term1 = t;
    return false;
  }

  desc = tuple_type_desc(tbl->types, tau);
  if (i >= desc->nelem) {
    error.code = INVALID_TUPLE_INDEX;
    error.type1 = tau;
    error.badval = i;
    return false;
  }

  if (! is_subtype(tbl->types, term_type(tbl, v), desc->elem[i])) {
    error.code = TYPE_MISMATCH;
    error.term1 = v;
    error.type1 = desc->elem[i];
    error.index = -1;
    return false;
  }
  
  return true;
}


// Check that a polynomial has degree at most MAX_DEGREE
static inline bool check_arith_buffer_degree(arith_buffer_t *b) {
  return check_maxdegree(arith_buffer_degree(b));
}


// Same thing for bitvector polynomials
static inline bool check_bvarith_buffer_degree(bvarith_buffer_t *b) {
  return check_maxdegree(bvarith_buffer_degree(b));
}

static inline bool check_bvarith64_buffer_degree(bvarith64_buffer_t *b) {
  return check_maxdegree(bvarith64_buffer_degree(b));
}

// Check that the degree of term t is at most MAX_DEGREE
static inline bool check_term_degree(term_table_t *tbl, term_t t) {
  return check_maxdegree(term_degree(tbl, t));
}



// Check whether t is a bitvector term of size n
static bool check_bitsize(term_table_t *tbl, term_t t, uint32_t n) {
  uint32_t s;

  s = term_bitsize(tbl, t);
  if (s != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = s;
    return false;
  }
  return true;
}


// Check whether i is a valid shift for bitvectors of size n
static bool check_bitshift(uint32_t i, uint32_t n) {
  if (i > n) {
    error.code = INVALID_BITSHIFT;
    error.badval = i;
    return false;
  }

  return true;
}

// Check whether [i, j] is a valid segment for bitvectors of size n
static bool check_bitextract(uint32_t i, uint32_t j, uint32_t n) {
  if (i < 0 || i > j || j >= n) {
    error.code = INVALID_BVEXTRACT;
    return false;
  }
  return true;
}


// Check whether terms a[0] to a[n-1] are all bitvector terms of size p
static bool check_good_bitvectors(term_table_t *tbl, uint32_t n, term_t *a, uint32_t p) {
  uint32_t i, s;
  term_t t;

  for (i=0; i<n; i++) {
    t = a[i];
    if (! is_bitvector_term(tbl, t)) {
      error.code = BITVECTOR_REQUIRED;
      error.term1 = t;
      error.index = i;
      return false;
    }

    s = term_bitsize(tbl, t);
    if (s != p) {
      error.code = INCOMPATIBLE_BVSIZES;
      error.term1 = t;
      error.index = i;
      error.badval = s;      
      return false;
    }
  }

  return true;
}






/***********************
 *  TYPE CONSTRUCTORS  *
 **********************/

EXPORTED type_t yices_bool_type() {
  return bool_type(&types);
}

EXPORTED type_t yices_int_type() {
  return int_type(&types);
}

EXPORTED type_t yices_real_type() {
  return real_type(&types);
}

EXPORTED type_t yices_bv_type(uint32_t size) {
  if (! check_positive(size) || ! check_maxbvsize(size)) {
    return NULL_TYPE;
  }
  return bv_type(&types, size);
}

EXPORTED type_t yices_new_uninterpreted_type() {
  return new_uninterpreted_type(&types);
}

EXPORTED type_t yices_new_scalar_type(uint32_t card) {
  if (! check_positive(card)) {
    return NULL_TYPE;
  }
  return new_scalar_type(&types, card);
}

EXPORTED type_t yices_tuple_type(uint32_t n, type_t elem[]) {
  if (! check_positive(n) || 
      ! check_arity(n) ||
      ! check_good_types(&types, n, elem)) {
    return NULL_TYPE;
  }
  return tuple_type(&types, n, elem);
}

EXPORTED type_t yices_function_type(uint32_t n, type_t dom[], type_t range) {
  if (! check_positive(n) || 
      ! check_arity(n) ||
      ! check_good_type(&types, range) || 
      ! check_good_types(&types, n, dom)) {
    return NULL_TYPE;
  }
  return function_type(&types, range, n, dom);
}






#if 0



/*******************************
 *  BOOLEAN-TERM CONSTRUCTORS  *
 ******************************/

/*
 * Parameters must all be type correct.
 */

/*
 * Negate non-constant boolean term
 */
static term_t bool_negate(term_table_t *tbl, term_t x) {
  if (term_kind(tbl, x) == NOT_TERM) {
    return not_term_arg(tbl, x);
  } else {
    return not_term(tbl, x);
  }
}


/*
 * Simplifications:
 *   not true  --> false
 *   not false --> true
 *   not not x --> x
 */
static term_t mk_not(term_table_t *tbl, term_t x) {
  if (x == true_term(tbl)) return false_term(tbl);
  if (x == false_term(tbl)) return true_term(tbl);
  return bool_negate(tbl, x);
}


/*
 * check whether x == not y.
 */
static bool opposite_bool_terms(term_table_t *tbl, term_t x, term_t y) {
  return (term_kind(tbl, x) == NOT_TERM && not_term_arg(tbl, x) == y) 
    || (term_kind(tbl, y) == NOT_TERM && not_term_arg(tbl, y) == x);
}


/*
 * Simplifications:
 *   x or x       --> x
 *   x or true    --> true
 *   x or false   --> x
 *   x or (not x) --> true
 *
 * Normalization: put smaller index first
 */
static term_t mk_binary_or(term_table_t *tbl, term_t x, term_t y) {
  term_t aux[2];
  
  if (x == y) return x;
  if (x == true_term(tbl)) return x;
  if (y == true_term(tbl)) return y;
  if (x == false_term(tbl)) return y;
  if (y == false_term(tbl)) return x;
  if (opposite_bool_terms(tbl, x, y)) return true_term(tbl);

  if (x < y) {
    aux[0] = x; aux[1] = y;
  } else {
    aux[0] = y; aux[1] = x;
  }

  return or_term(tbl, 2, aux);
}


/*
 * Simplifications:
 *   x and x       --> x
 *   x and true    --> x
 *   x and false   --> false
 *   x and (not x) --> false
 *
 * Otherwise;
 *   x and y --> not (or (not x) (not y))
 */
static term_t mk_binary_and(term_table_t *tbl, term_t x, term_t y) {
  term_t aux[2];

  if (x == y) return x;
  if (x == true_term(tbl)) return y;
  if (y == true_term(tbl)) return x;
  if (x == false_term(tbl)) return x;
  if (y == false_term(tbl)) return y;
  if (opposite_bool_terms(tbl, x, y)) return false_term(tbl);

  // x := not x and y := not y
  x = bool_negate(tbl, x);
  y = bool_negate(tbl, y);

  if (x < y) {
    aux[0] = x; aux[1] = y;
  } else {
    aux[0] = y; aux[1] = x;
  }

  return not_term(tbl, or_term(tbl, 2, aux));
}


/*
 * Check whether x is of the form (not u) where u is uninterpreted
 */
static inline bool not_bool_var(term_table_t *tbl, term_t x) {
  return term_kind(tbl, x) == NOT_TERM && term_kind(tbl, not_term_arg(tbl, x)) == UNINTERPRETED_TERM;
}

/*
 * Simplifications:
 *    iff x x       --> true
 *    iff x true    --> x
 *    iff x false   --> not x
 *    iff x (not x) --> false
 * 
 *    iff (not x) (not y) --> eq x y 
 *
 * Optional simplification:
 *    iff (not x) y       --> not (eq x y) 
 *
 * Smaller index is on the left-hand-side of eq
 */
static term_t mk_iff(term_table_t *tbl, term_t x, term_t y) {
  term_t aux;
  bool negate;

  if (x == y) return true_term(tbl);
  if (x == true_term(tbl)) return y;
  if (y == true_term(tbl)) return x;
  if (x == false_term(tbl)) return mk_not(tbl, y);
  if (y == false_term(tbl)) return mk_not(tbl, x);
  if (opposite_bool_terms(tbl, x, y)) return false_term(tbl);

  negate = false;

#if 0
  // rewrite (iff (not x) y) to not (eq x y)
  if (term_kind(tbl, x) == NOT_TERM) {
    x = not_term_arg(tbl, x);
    negate = true;
  }
  if (term_kind(tbl, y) == NOT_TERM) {
    y = not_term_arg(tbl, y);
    negate = ! negate;
  }
#else 
  /*
   * variant: 
   * - rewrite (iff (not x) y) to (eq x (not y)) if x is uninterpreted
   * - rewrite (iff (not x) (not y)) to (eq x y)
   */
  if ((term_kind(tbl, x) == NOT_TERM && term_kind(tbl, y) == NOT_TERM)
      || not_bool_var(tbl, x) || not_bool_var(tbl, y)) {
    x = mk_not(tbl, x);
    y = mk_not(tbl, y); 
  }
#endif

  // swap if x > y
  if (x > y) {
    aux = x; x = y; y = aux;
  }
  aux = eq_term(tbl, x, y);

  return negate ? not_term(tbl, aux) : aux;
}


/*
 * Simplifications:
 *    xor x x       --> false
 *    xor x true    --> not x
 *    xor x false   --> x
 *    xor x (not x) --> true
 * 
 *    xor (not x) (not y) --> (not (eq x y))
 *    xor (not x) y       --> (eq x y)
 *    xor x y             --> (not (eq x y))
 *
 * Smaller index is on the left-hand-side of eq
 */
static term_t mk_xor(term_table_t *tbl, term_t x, term_t y) {
  term_t aux;
  bool negate;

  if (x == y) return false_term(tbl);
  if (x == false_term(tbl)) return y;
  if (y == false_term(tbl)) return x;
  if (x == true_term(tbl)) return mk_not(tbl, y);
  if (y == true_term(tbl)) return mk_not(tbl, x);
  if (opposite_bool_terms(tbl, x, y)) return true_term(tbl);

  negate = true;
  if (term_kind(tbl, x) == NOT_TERM) {
    x = not_term_arg(tbl, x);
    negate = ! negate;
  }
  if (term_kind(tbl, y) == NOT_TERM) {
    y = not_term_arg(tbl, y);
    negate = ! negate;
  }

  // swap if x > y
  if (x > y) {
    aux = x; x = y; y = aux;
  }
  aux = eq_term(tbl, x, y);

  return negate ? not_term(tbl, aux) : aux;
}


/*
 * Simplifications:
 *    implies x x        --> true
 *    implies true x     --> x
 *    implies false x    --> true
 *    implies x true     --> true
 *    implies x false    --> (not x)
 *    implies x (not x)  --> (not x)
 *
 *    implies x y    --> (or (not x) y)
 */
static term_t mk_implies(term_table_t *tbl, term_t x, term_t y) {
  term_t aux[2];

  if (x == y) return true_term(tbl);
  if (x == true_term(tbl)) return y;
  if (x == false_term(tbl)) return true_term(tbl);
  if (y == true_term(tbl)) return true_term(tbl);
  if (y == false_term(tbl)) return bool_negate(tbl, x);
  if (opposite_bool_terms(tbl, x, y)) return y;

  x = bool_negate(tbl, x);
  if (x < y) {
    aux[0] = x; aux[1] = y;
  } else {
    aux[0] = y; aux[1] = x;
  }

  return or_term(tbl, 2, aux);
}



/*
 * Build (bv-eq x (ite c y z))
 * - c not true/false
 */
static term_t mk_bveq_ite(term_table_t *tbl, term_t c, term_t x, term_t y, term_t z) {
  term_t ite, aux;

  assert(term_type(tbl, x) == term_type(tbl, y) && term_type(tbl, x) == term_type(tbl, z));

  ite = ite_term(tbl, c, y, z, term_type(tbl, y));

  // normalize (bveq x ite): put smaller index on the left
  if (x > ite) {
    aux = x; x = ite; ite = aux;
  }

  return bveq_atom(tbl, x, ite);
}

/*
 * Special constructor for (ite c (bveq x y) (bveq z u))
 * 
 * Apply lift-if rule:
 * (ite c (bveq x y) (bveq x u))  ---> (bveq x (ite c y u))
 */
static term_t mk_lifted_ite_bveq(term_table_t *tbl, term_t c, term_t t, term_t e) {
  bv_atom_t *eq1, *eq2;
  term_t x;

  assert(term_kind(tbl, t) == BV_EQ_ATOM && term_kind(tbl, e) == BV_EQ_ATOM);

  eq1 = bvatom_desc(tbl, t);
  eq2 = bvatom_desc(tbl, e);

  x = eq1->left;
  if (x == eq2->left)  return mk_bveq_ite(tbl, c, x, eq1->right, eq2->right);
  if (x == eq2->right) return mk_bveq_ite(tbl, c, x, eq1->right, eq2->left);

  x = eq1->right;
  if (x == eq2->left)  return mk_bveq_ite(tbl, c, x, eq1->left, eq2->right);
  if (x == eq2->right) return mk_bveq_ite(tbl, c, x, eq1->left, eq2->left);

  return ite_term(tbl, c, t, e, bool_type(tbl->types));
}

/*
 * Simplifications:
 *  ite c x x        --> x
 *  ite true x y     --> x
 *  ite false x y    --> y
 *
 *  ite c x (not x)  --> (c == x)
 *
 *  ite c c x        --> c or x
 *  ite c x c        --> c and x
 *  ite c (not c) x  --> (not c) and x
 *  ite c x (not c)  --> (not c) or x
 *
 *  ite x true y     --> x or y
 *  ite x y false    --> x and y
 *  ite x false y    --> (not x) and y
 *  ite x y true     --> (not x) or y
 *
 * Otherwise:
 *  ite (not c) x y  --> ite c y x
 */
static term_t mk_bool_ite(term_table_t *tbl, term_t c, term_t t, term_t e) {
  term_t aux;

  if (t == e) return t;
  if (c == true_term(tbl)) return t;
  if (c == false_term(tbl)) return e;

  if (opposite_bool_terms(tbl, t, e)) return mk_iff(tbl, c, t);
  
  if (c == t) return mk_binary_or(tbl, c, e);
  if (c == e) return mk_binary_and(tbl, c, t);
  if (opposite_bool_terms(tbl, c, t)) return mk_binary_and(tbl, t, e);
  if (opposite_bool_terms(tbl, c, e)) return mk_binary_or(tbl, t, e);

  if (t == true_term(tbl)) return mk_binary_or(tbl, c, e);
  if (e == false_term(tbl)) return mk_binary_and(tbl, c, t);
  if (t == false_term(tbl)) return mk_binary_and(tbl, mk_not(tbl, c), e);
  if (e == true_term(tbl)) return mk_binary_or(tbl, mk_not(tbl, c), t);


  if (term_kind(tbl, c) == NOT_TERM) {
    c = not_term_arg(tbl, c);
    aux = t; t = e; e = aux;
  }

  if (term_kind(tbl, t) == BV_EQ_ATOM && term_kind(tbl, e) == BV_EQ_ATOM) {
    return mk_lifted_ite_bveq(tbl, c, t, e);
  }

  return ite_term(tbl, c, t, e, bool_type(tbl->types));
}



/*
 * Construction of (or a[0] ... a[n-1])
 * - all terms are assumed valid (and boolean)
 * - array a is modified (sorted)
 * - n must be positive
 */
static term_t mk_or(term_table_t *tbl, uint32_t n, term_t *a) {
  uint32_t i, j;
  term_t x, y;

  assert(n > 0);
  sort_bool_terms(tbl, n, a);

  j = 0;

  x = a[0];
  if (x == true_term(tbl)) {
    return true_term(tbl);
  }

  if (x != false_term(tbl)) { 
    a[j] = x;
    j ++;
  }

  for (i=1; i<n; i++) {
    y = a[i];
    if (x != y) {
      if (y == true_term(tbl) || opposite_bool_terms(tbl, x, y)) {
	return true_term(tbl);
      }
      assert(y != false_term(tbl));
      x = y;
      a[j] = x;
      j ++;
    }
  }

  if (j <= 1) { 
    // if j == 0, then x == false_term
    // if j == 1, then x == unique non-false term in a
    return x;
  } else {
    return or_term(tbl, j, a);
  }
}


/*
 * Construction of (and a[0] ... a[n-1])
 * - all terms are assumed valid (and boolean)
 * - array a is modified (sorted)
 * - n must be positive
 */
static term_t mk_and(term_table_t *tbl, uint32_t n, term_t *a) {
  uint32_t i, j;
  term_t x, y;

  assert(n > 0);
  sort_bool_terms(tbl, n, a);

  j = 0;

  x = a[0];
  if (x == false_term(tbl)) {
    return false_term(tbl);
  }

  if (x != true_term(tbl)) { 
    a[j] = x;
    j ++;
  }

  for (i=1; i<n; i++) {
    y = a[i];
    if (x != y) {
      if (y == false_term(tbl) || opposite_bool_terms(tbl, x, y)) {
	return false_term(tbl);
      }
      assert(y != true_term(tbl));
      x = y;
      a[j] = x;
      j ++;
    }
  }

  if (j <= 1) { 
    // if j == 0, then x == true_term
    // if j == 1, then x == unique non-true term in a
    return x;
  } else {
    for (i=0; i<j; i++) {
      a[i] = bool_negate(tbl, a[i]);
    }
    return not_term(tbl, or_term(tbl, j, a));
  }
}







/******************************
 *  CHECKS FOR DISEQUALITIES  *
 *****************************/

/*
 * Check whether two terms x and y (of the same type) cannot be equal.
 * Warning: recursive call could blow up on nested tuples.
 */
static bool disequal_terms(term_table_t *tbl, term_t x, term_t y);


/*
 * The following base cases are handled:
 * - x and y are both TERM_CONSTANT
 * - x and y are boolean  constant, or x = (not y).
 * - x and y are both ARITH_TERM (with polynomials p1 and p2 such that 
 *   (p1 - p2) is a non-zero constant)
 * - x and y are both BVARITH_TERM (with bvexpressions p1 and p2 such that
 *    (p1 - p2) is a non-zero bit-vector constant)
 * - x and y are both BVLOGIC_TERM (with bits x_i and y_i distinct, for some i)
 */
static inline bool disequal_constant_terms(term_t x, term_t y) {
  return x != y;
}

static inline bool disequal_boolean_terms(term_table_t *tbl, term_t x, term_t y) {
  return (x == true_term(tbl) && y == false_term(tbl))
    || (x == false_term(tbl) && y == true_term(tbl))
    || opposite_bool_terms(tbl, x, y);
}


static bool disequal_arith_terms(term_table_t *tbl, term_t x, term_t y) {
  type_kind_t kx, ky;
  arith_var_t vx, vy;

  kx = term_kind(tbl, x);
  ky = term_kind(tbl, y);
  vx = term_theory_var(tbl, x);
  vy = term_theory_var(tbl, y);

  if (kx == ARITH_TERM && ky == ARITH_TERM) {
    return must_disequal_polynomial(arith_term_desc(tbl, x), arith_term_desc(tbl, y));
  } else if (kx == ARITH_TERM && vy != null_theory_var) {
    return polynomial_is_const_plus_var(arith_term_desc(tbl, x), vy);
  } else if (ky == ARITH_TERM && vx != null_theory_var) {
    return polynomial_is_const_plus_var(arith_term_desc(tbl, y), vx);
  } else {
    return false;
  }
}

static bool disequal_bitvector_terms(term_table_t *tbl, term_t x, term_t y) {
  term_kind_t kx, ky;
  bvconst_term_t *cx, *cy;
  int32_t n;

  kx = term_kind(tbl, x);
  ky = term_kind(tbl, y);

  if (kx == ky) {
    switch (kx) {
    case BV_LOGIC_TERM:
      return bvlogic_must_disequal_expr(bvlogic_term_desc(tbl, x), bvlogic_term_desc(tbl, y));
    case BV_ARITH_TERM:
      return bvarith_must_disequal_expr(bvarith_term_desc(tbl, x), bvarith_term_desc(tbl, y));
    case BV_CONST_TERM:
      cx = bvconst_term_desc(tbl, x);
      cy = bvconst_term_desc(tbl, y);
      n = cx->nbits;
      assert(n == cy->nbits);    
      return bvconst_neq(cx->bits, cy->bits, (n + 31) >> 5);
    default:
      return false;
    }

  } else if (kx == BV_CONST_TERM && ky == BV_LOGIC_TERM) {
    cx = bvconst_term_desc(tbl, x);
    n = cx->nbits;
    return bvlogic_must_disequal_constant(bvlogic_term_desc(tbl, y), n, cx->bits);

  } else if (kx == BV_LOGIC_TERM && ky == BV_CONST_TERM) {
    cy = bvconst_term_desc(tbl, y);
    n = cy->nbits;
    return bvlogic_must_disequal_constant(bvlogic_term_desc(tbl, x), n, cy->bits);

  } else {
    return false;
  }
}


/*
 * Tuple terms x and y are trivially distinct if they have components 
 * x_i and y_i that are trivially distinct.
 */
static bool disequal_tuple_terms(term_table_t *tbl, term_t x, term_t y) {
  tuple_term_t *tuple_x, *tuple_y;
  int32_t i, n;

  assert(term_type(tbl, x) == term_type(tbl, y));

  tuple_x = tuple_term_desc(tbl, x);
  tuple_y = tuple_term_desc(tbl, y);

  n = tuple_x->nargs;
  assert(n == tuple_y->nargs);
  for (i=0; i<n; i++) {
    if (disequal_terms(tbl, tuple_x->arg[i], tuple_y->arg[i])) {
      return true;
    }
  }
  return false;
}


/*
 * (update f (x1 ... xn) a) is trivially distinct from (update f (x1 ... xn) b)
 * if a is trivially distinct from b.
 */
static bool disequal_update_terms(term_table_t *tbl, term_t x, term_t y) {
  int32_t i, n;
  update_term_t *update_x, *update_y;

  assert(term_type(tbl, x) == term_type(tbl, y));

  update_x = update_term_desc(tbl, x);
  update_y = update_term_desc(tbl, y);

  if (update_x->fun != update_y->fun) return false;

  n = update_x->nargs;
  assert(n == update_y->nargs);
  for (i=0; i<n; i++) {
    if (update_x->arg[i] != update_y->arg[i]) return false;
  }

  return disequal_terms(tbl, update_x->newval, update_y->newval);
}


/*
 * Top level
 */
static bool disequal_terms(term_table_t *tbl, term_t x, term_t y) {
  term_kind_t kind;

  if (is_boolean_term(tbl, x)) {
    assert(is_boolean_term(tbl, y));
    return disequal_boolean_terms(tbl, x, y);
  }

  if (is_arithmetic_term(tbl, x)) {
    assert(is_arithmetic_term(tbl, y));
    return disequal_arith_terms(tbl, x, y);
  }

  if (is_bitvector_term(tbl, x)) {
    assert(is_bitvector_term(tbl, y));
    return disequal_bitvector_terms(tbl, x, y);
  }

  kind = term_kind(tbl, x);
  if (kind != term_kind(tbl, y)) return false;

  switch (kind) {
  case CONSTANT_TERM:
    return disequal_constant_terms(x, y);
  case TUPLE_TERM:
    return disequal_tuple_terms(tbl, x, y);
  case UPDATE_TERM:
    return disequal_update_terms(tbl, x, y);
  default:
    return false;
  }
}


/*
 * Auxiliary functions for simplification
 */
// check whether terms a[0...n-1] and b[0 .. n-1] are equal
static bool equal_term_arrays(int32_t n, term_t *a, term_t *b) {
  int32_t i;

  for (i=0; i<n; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

// check whether a[i] cannot be equal to b[i] for one i
static bool disequal_term_arrays(term_table_t *tbl, int32_t n, term_t *a, term_t *b) {
  int32_t i;

  for (i=0; i<n; i++) {
    if (disequal_terms(tbl, a[i], b[i])) return true;
  }

  return false;
}

// check whether all elements of a are disequal
// this is expensive: quadratic cost, but should fail quickly on most examples
static bool pairwise_disequal_terms(term_table_t *tbl, int32_t n, term_t *a) {
  int32_t i, j;

  for (i=0; i<n; i++) {
    for (j=i+1; j<n; j++) {
      if (! disequal_terms(tbl, a[i], a[j])) return false;
    }
  }

  return true;
}





/**********************************
 *   LIFT FOR IF-THEN-ELSE TERMS  *
 *********************************/

/*
 * Cheap lift-if decomposition:
 * - decompose (ite c x y) (ite c z u) ---> [c, x, z, y, u]
 * - decompose (ite c x y) z           ---> [c, x, z, y, z]
 * - decompose x (ite c y z)           ---> [c, x, y, x, z]
 *
 * The result is stored into the lift_result_t object:
 * - for example: [c, x, z, y, u] is stored as
 *    cond = c,  left1 = x, left2 = z,  right1 = y, right2 = u
 * - the function return true if the decomposition succeeds, false otherwise
 */
typedef struct lift_result_s {
  term_t cond;
  term_t left1, left2;
  term_t right1, right2;
} lift_result_t;


static bool check_for_lift_if(term_table_t *tbl, term_t t1, term_t t2, lift_result_t *d) {
  ite_term_t *ite1, *ite2;
  term_t cond;

  if (term_kind(tbl, t1) == ITE_TERM) {
    if (term_kind(tbl, t2) == ITE_TERM) {
      // both are (if-then-else ..) 
      ite1 = ite_term_desc(tbl, t1);
      ite2 = ite_term_desc(tbl, t2);
      
      cond = ite1->cond;
      if (cond == ite2->cond) {
	d->cond = cond;
	d->left1 = ite1->then_arg;
	d->left2 = ite2->then_arg;
	d->right1 = ite1->else_arg;
	d->right2 = ite2->else_arg;
	return true;
      } 

    } else {
      // t1 is (if-then-else ..) t2 is not
      ite1 = ite_term_desc(tbl, t1);
      d->cond = ite1->cond;
      d->left1 = ite1->then_arg;
      d->left2 = t2;
      d->right1 = ite1->else_arg;
      d->right2 = t2;
      return true;
      
    }
  } else if (term_kind(tbl, t2) == ITE_TERM) {
    // t2 is (if-then-else ..) t1 is not

    ite2 = ite_term_desc(tbl, t2);
    d->cond = ite2->cond;
    d->left1 = t1;
    d->left2 = ite2->then_arg;
    d->right1 = t1;
    d->right2 = ite2->else_arg;
    return true;
  }
 
 return false;  
}




/****************************************************
 *  LIFT COMMON FACTORS IN ARITHMETIC IF-THEN-ELSE  *
 ***************************************************/

/*
 * If t and e are polynomials with integer variables
 * then we can write t as (a * t') and e as (a * e')
 * where a = gcd of coefficients of t and e.
 * Then (ite c t e) is rewritten to a * (ite c t' e')
 */
static term_t mk_integer_polynomial_ite(term_table_t *tbl, term_t c, term_t t, term_t e) {
  polynomial_t *p, *q;
  arith_buffer_t *b;

  assert(is_integer_term(tbl, t) && is_integer_term(tbl, e));

  p = arith_term_desc(tbl, t);  // then part
  q = arith_term_desc(tbl, e);  // else part

  if (!polynomial_is_zero(p) && !polynomial_is_zero(q)) {
    monarray_common_factor(p->mono, &r0); // r0 = gcd of coefficients of p
    monarray_common_factor(q->mono, &r1); // r1 = gcd of coefficients of q
    q_gcd(&r0, &r1);  // r0 = common factor a above

    assert(q_is_pos(&r0) && q_is_integer(&r0));
    if (! q_is_one(&r0)) {
      // use internal arith buffer for operations
      b = get_internal_arith_buffer();

      // construct p' := 1/r0 * p
      arith_buffer_reset(b);
      arith_buffer_add_monarray(b, p->mono);
      arith_buffer_div_const(b, &r0);
      t = arith_term(tbl, b);

      // construct q' := 1/r0 * q
      arith_buffer_reset(b);
      arith_buffer_add_monarray(b, q->mono);
      arith_buffer_div_const(b, &r0);
      e = arith_term(tbl, b);

      // (ite c p' q')
      t = ite_term(tbl, c, t, e, int_type(tbl->types));

      // built r0 * t
      arith_buffer_reset(b);
      arith_buffer_add_mono(b, get_arithmetic_variable(tbl, t), &r0);
      return arith_term(tbl, b);
    }
  }
  
  // no common factor to lift
  return ite_term(tbl, c, t, e, int_type(tbl->types));
}





/***********************
 *  TERM CONSTRUCTORS  *
 **********************/

EXPORTED term_t yices_true() {
  return true_term(&terms);
}

EXPORTED term_t yices_false() {
  return false_term(&terms);
}

EXPORTED term_t yices_constant(type_t tau, int32_t index) {
  if (! check_good_constant(&types, tau, index)) {
    return NULL_TERM;
  }
  return constant_term(&terms, tau, index);
}

EXPORTED term_t yices_new_uninterpreted_term(type_t tau) {
  if (! check_good_type(&types, tau)) {
    return NULL_TERM;
  }
  return new_uninterpreted_term(&terms, tau);
}

EXPORTED term_t yices_variable(type_t tau, int32_t index) {
  if (! check_nonneg(index) || 
      ! check_good_type(&types, tau)) {
    return NULL_TERM;
  }
  return variable(&terms, tau, index);
}


/*
 * Simplifications if fun is an update term:
 *   ((update f (a_1 ... a_n) v) a_1 ... a_n)   -->  v
 *   ((update f (a_1 ... a_n) v) x_1 ... x_n)   -->  (f x_1 ... x_n)
 *         if x_i must disequal a_i
 */
EXPORTED term_t yices_application(term_t fun, int32_t n, term_t arg[]) {
  update_term_t *update;

  if (! check_good_application(&terms, fun, n, arg)) {
    return NULL_TERM;
  }

  while (term_kind(&terms, fun) == UPDATE_TERM) {
    // fun is (update f (a_1 ... a_n) v)
    update = update_term_desc(&terms, fun);
    assert(update->nargs == n);

    if (equal_term_arrays(n, update->arg, arg)) {
      return update->newval;
    }
    
    if (disequal_term_arrays(&terms, n, update->arg, arg)) {
      // ((update f (a_1 ... a_n) v) x_1 ... x_n) ---> (f x_1 ... x_n)
      // repeat simplification if f is an update term again
      fun = update->fun;
    } else {
      break;
    }
  }

  return app_term(&terms, fun, n, arg);
}


/*
 * Simplifications
 *    ite true x y   --> x
 *    ite false x y  --> y
 *    ite c x x      --> x
 *
 * Otherwise:
 *    ite (not c) x y --> ite c y x
 *
 * Plus special trick for integer polynomials:
 *    ite c (d * p1) (d * p2) --> d * (ite c p1 p2)
 */
EXPORTED term_t yices_ite(term_t cond, term_t then_term, term_t else_term) {
  term_t aux;
  type_t tau;

  // Check type correctness: first steps
  if (! check_good_term(&terms, cond) || 
      ! check_good_term(&terms, then_term) ||
      ! check_good_term(&terms, else_term) || 
      ! check_boolean_term(&terms, cond)) {
    return NULL_TERM;
  }

  // Check whether then/else are compatible and get the supertype
  tau = super_type(&types, term_type(&terms, then_term), term_type(&terms, else_term));

  if (tau == NULL_TYPE) {
    // type error
    error.code = INCOMPATIBLE_TYPES;
    error.term1 = then_term;
    error.type1 = term_type(&terms, then_term);
    error.term2 = else_term;
    error.type2 = term_type(&terms, else_term);
    return NULL_TERM;
  }

  // boolean ite
  if (is_boolean_term(&terms, then_term)) {
    assert(is_boolean_term(&terms, else_term));
    return mk_bool_ite(&terms, cond, then_term, else_term);
  }

  // non-boolean:
  if (then_term == else_term) return then_term;
  if (cond == true_term(&terms)) return then_term;
  if (cond == false_term(&terms)) return else_term;

  if (term_kind(&terms, cond) == NOT_TERM) {
    cond = not_term_arg(&terms, cond);
    aux = then_term; then_term = else_term; else_term = aux;
  }

#if 1
  // DISABLE THIS FOR BASELINE TESTING
  // check whether both sides are integer polynomials
  if (is_integer_type(tau) 
      && term_kind(&terms, then_term) == ARITH_TERM 
      && term_kind(&terms, else_term) == ARITH_TERM) {
    return mk_integer_polynomial_ite(&terms, cond, then_term, else_term);
  }
#endif

  return ite_term(&terms, cond, then_term, else_term, tau);
}


/*
 * Equality: convert to boolean, arithmetic, or bitvector equality
 */
static term_t mk_bveq(term_t left, term_t right);
static term_t mk_aritheq(term_t left, term_t right);

EXPORTED term_t yices_eq(term_t left, term_t right) {
  term_t aux;

  if (! check_good_eq(&terms, left, right)) {
    return NULL_TERM;
  }

  if (is_boolean_term(&terms, left)) {
    assert(is_boolean_term(&terms, right));
    return mk_iff(&terms, left, right);
  }

  if (is_arithmetic_term(&terms, left)) {
    assert(is_arithmetic_term(&terms, right));
    return mk_aritheq(left, right);
  }

  if (is_bitvector_term(&terms, left)) {
    assert(is_bitvector_term(&terms, right));
    return mk_bveq(left, right);
  }

  // general case
  if (left == right) return true_term(&terms);
  if (disequal_terms(&terms, left, right)) {
    return false_term(&terms);
  }

  // put smaller index on the left
  if (left > right) {
    aux = left; left = right; right = aux;
  }

  return eq_term(&terms, left, right);
}


/*
 * Disequality
 */
static term_t mk_bvneq(term_t left, term_t right);
static term_t mk_arithneq(term_t left, term_t right);

EXPORTED term_t yices_neq(term_t left, term_t right) {
  term_t aux;

  if (! check_good_eq(&terms, left, right)) {
    return NULL_TERM;
  }

  if (is_boolean_term(&terms, left)) {
    assert(is_boolean_term(&terms, right));
    return mk_xor(&terms, left, right);
  }

  if (is_arithmetic_term(&terms, left)) {
    assert(is_arithmetic_term(&terms, right));
    return mk_arithneq(left, right);
  }

  if (is_bitvector_term(&terms, left)) {
    assert(is_bitvector_term(&terms, right));
    return mk_bvneq(left, right);
  }

  // non-boolean
  if (left == right) return false_term(&terms);
  if (disequal_terms(&terms, left, right)) {
    return true_term(&terms);
  }

  // put smaller index on the left
  if (left > right) {
    aux = left; left = right; right = aux;
  }

  return not_term(&terms, eq_term(&terms, left, right));
}


/*
 * or and and may modify arg
 */
EXPORTED term_t yices_or(int32_t n, term_t arg[]) {
  if (! check_nonneg(n) || 
      ! check_arity(n) ||
      ! check_good_terms(&terms, n, arg) || 
      ! check_boolean_args(&terms, n, arg)) {
    return NULL_TERM;
  }

  switch (n) {
  case 0:
    return false_term(&terms);
  case 1:
    return arg[0];
  case 2:
    return mk_binary_or(&terms, arg[0], arg[1]);
  default:
    return mk_or(&terms, n, arg);
  }
}

EXPORTED term_t yices_and(int32_t n, term_t arg[]) {
  if (! check_nonneg(n) || 
      ! check_arity(n) ||
      ! check_good_terms(&terms, n, arg) || 
      ! check_boolean_args(&terms, n, arg)) {
    return NULL_TERM;
  }

  switch (n) {
  case 0:
    return true_term(&terms);
  case 1:
    return arg[0];
  case 2:
    return mk_binary_and(&terms, arg[0], arg[1]);
  default:
    return mk_and(&terms, n, arg);
  }
}

EXPORTED term_t yices_not(term_t arg) {
  if (! check_good_term(&terms, arg) || 
      ! check_boolean_term(&terms, arg)) {
    return NULL_TERM;
  }

  return mk_not(&terms, arg);
}

EXPORTED term_t yices_xor(term_t left, term_t right) {
  if (! check_good_term(&terms, left) ||
      ! check_good_term(&terms, right) || 
      ! check_boolean_term(&terms, left) || 
      ! check_boolean_term(&terms, right)) {
    return NULL_TERM;
  }

  return mk_xor(&terms, left, right);
}

EXPORTED term_t yices_iff(term_t left, term_t right) {
  if (! check_good_term(&terms, left) ||
      ! check_good_term(&terms, right) || 
      ! check_boolean_term(&terms, left) || 
      ! check_boolean_term(&terms, right)) {
    return NULL_TERM;
  }

  return mk_iff(&terms, left, right);
}

EXPORTED term_t yices_implies(term_t left, term_t right) {
  if (! check_good_term(&terms, left) ||
      ! check_good_term(&terms, right) || 
      ! check_boolean_term(&terms, left) || 
      ! check_boolean_term(&terms, right)) {
    return NULL_TERM;
  }

  return mk_implies(&terms, left, right);
}


/*
 * Simplification:
 *   (mk_tuple (select 0 x) ... (select n-1 x)) --> x
 */
EXPORTED term_t yices_tuple(int32_t n, term_t arg[]) {
  int32_t i;
  term_t x, a;

  if (! check_positive(n) || 
      ! check_arity(n) ||
      ! check_good_terms(&terms, n, arg)) {
    return NULL_TERM;
  }

  a = arg[0];
  if (term_kind(&terms, a) == SELECT_TERM && select_term_index(&terms, a) == 0) {
    x = select_term_arg(&terms, a);
    for (i = 1; i<n; i++) {
      a = arg[i];
      if (term_kind(&terms, a) != SELECT_TERM ||
	  select_term_index(&terms, a) != i ||
	  select_term_arg(&terms, a) != x) {
	return tuple_term(&terms, n, arg);
      }
    }
    return x;
  }

  return tuple_term(&terms, n, arg);
}


/*
 * Simplification: (select i (mk_tuple x_1 ... x_n))  --> x_i
 */
EXPORTED term_t yices_select(int32_t index, term_t tuple) {
  if (! check_good_select(&terms, index, tuple)) {
    return NULL_TERM;
  }

  // simplify
  if (term_kind(&terms, tuple) == TUPLE_TERM) {
    return tuple_term_arg(&terms, tuple, index);
  } else {
    return select_term(&terms, index, tuple);
  }
}



/*
 * Simplification: 
 *  (update (update f (a_1 ... a_n) v) (a_1 ... a_n) v') --> (update f (a_1 ... a_n) v')
 * TBD
 */
EXPORTED term_t yices_update(term_t fun, int32_t n, term_t arg[], term_t new_v) {
  if (! check_good_update(&terms, fun, n, arg, new_v)) {
    return NULL_TERM;
  }

  return update_term(&terms, fun, n, arg, new_v);
}



/*
 * (distinct t1 ... t_n):
 *
 * if n == 1 --> true
 * if n == 2 --> (diseq t1 t2)
 * if t_i and t_j are equal --> false
 * if all are disequal --> true
 *
 * More simplifications uses type information,
 *  (distinct f g h) --> false if f g h are boolean.
 */
EXPORTED term_t yices_distinct(int32_t n, term_t arg[]) {
  int32_t i;
  type_t tau;

  if (n == 2) {
    return yices_neq(arg[0], arg[1]);
  }
  
  if (! check_positive(n) ||
      ! check_arity(n) ||
      ! check_good_distinct_term(&terms, n, arg)) {
    return NULL_TERM;
  }

  if (n == 1) {
    return true_term(&terms);
  }

  // check for finite types
  tau = term_type(&terms, arg[0]);
  if (tau == bool_type(&types) ||
      (type_kind(&types, tau) == SCALAR_TYPE && scalar_type_cardinal(&types, tau) < n)) {
    return false_term(&terms);
  }

  
  // check if two of the terms are equal
  int_array_sort(arg, n);
  for (i=1; i<n; i++) {
    if (arg[i] == arg[i-1]) {
      return false_term(&terms);
    }
  }

  // WARNING: THIS CAN BE EXPENSIVE
  if (pairwise_disequal_terms(&terms, n, arg)) {
    return true_term(&terms);
  }

  return distinct_term(&terms, n, arg);
}


/*
 * (tuple-update tuple index new_v) is (tuple with component i set to new_v)
 *
 * If tuple is (mk-tuple x_0 ... x_i ... x_n-1) then 
 *  (tuple-update t i v) is (mk-tuple x_0 ... v ... x_n-1)
 * 
 * Otherwise, 
 *  (tuple-update t i v) is (mk-tuple (select t 0) ... v  ... (select t n-1))
 *              
 */
static term_t mk_tuple_aux(term_table_t *tbl, term_t tuple, int32_t n, int32_t i, term_t v) {
  term_t a[n]; // GCC/C99 extension: can cause stack overflow if n is large
  int32_t j;
  tuple_term_t *desc;

  if (term_kind(tbl, tuple) == TUPLE_TERM) {
    desc = tuple_term_desc(tbl, tuple);
    for (j=0; j<n; j++) {
      if (i == j) {
	a[j] = v;
      } else {
	a[j] = desc->arg[j];
      }
    }
  } else {
    for (j=0; j<n; j++) {
      if (i == j) {
	a[j] = v;
      } else {
	a[j] = select_term(tbl, j, tuple);
      }
    }    
  }

  return tuple_term(tbl, n, a);
}

EXPORTED term_t yices_tuple_update(term_t tuple, int32_t index, term_t new_v) {
  int32_t n;

  if (! check_good_tuple_update(&terms, tuple, index, new_v)) {
    return NULL_TERM;
  }

  n = tuple_term_nargs(&terms, tuple);
  return mk_tuple_aux(&terms, tuple, n, index, new_v);
}



/*
 * Sort variables in increasing order (of term index)
 *
 * Simplification
 *  (forall (x_1::t_1 ... x_n::t_n) true) --> true
 *  (forall (x_1::t_1 ... x_n::t_n) false) --> false (types are nonempty)
 *
 *  (exists (x_1::t_1 ... x_n::t_n) true) --> true
 *  (exists (x_1::t_1 ... x_n::t_n) false) --> false (types are nonempty)
 */
EXPORTED term_t yices_forall(int32_t n, term_t var[], term_t body) {
  if (n > 1) { 
    int_array_sort(var, n);    
  }

  if (! check_good_quantified_term(&terms, n, var, body)) {
    return NULL_TERM;
  }

  if (body == true_term(&terms)) return body;
  if (body == false_term(&terms)) return body;

  return forall_term(&terms, n, var, body);
}

EXPORTED term_t yices_exists(int32_t n, term_t var[], term_t body) {
  if (n > 1) { 
    int_array_sort(var, n);    
  }

  if (! check_good_quantified_term(&terms, n, var, body)) {
    return NULL_TERM;
  }

  if (body == true_term(&terms)) return body;
  if (body == false_term(&terms)) return body;

  // (not (forall ... (not body))
  return not_term(&terms, forall_term(&terms, n, var, bool_negate(&terms, body)));
}






/*************************
 *  RATIONAL CONSTANTS   *
 ************************/

/*
 * All functions return a pointer to r0 or NULL if there's an error
 */
EXPORTED rational_t *yices_int32(int32_t val) {
  q_set32(&r0, val);
  return &r0;
}

EXPORTED rational_t *yices_int64(int64_t val) {
  q_set64(&r0, val);
  return &r0;
}

/*
 * Rational constants
 * - den must be non-zero
 * - common factors are removed
 */
EXPORTED rational_t *yices_rat32(int32_t num, uint32_t den) {
  if (den == 0) return NULL;
  q_set_int32(&r0, num, den);
  return &r0;
}

EXPORTED rational_t *yices_rat64(int64_t num, uint64_t den) {
  if (den == 0) return NULL;
  q_set_int64(&r0, num, den);
  return &r0;
}

/*
 * Convert a string to a rational: string format is
 *   <optional_sign> <numerator>/<denominator>
 * where <optional_sign> is + or - or nothing
 *    <numerator> and <denominator> are digit sequences.
 */
EXPORTED rational_t *yices_string_rational(char *s) {
  if (q_set_from_string(&r0, s) == 0) {
    return &r0;
  } else {
    // wrong format
    return NULL;
  }
}

/*
 * Convert a string in floating point format to a rational
 * The string must be in one of the following formats:
 *   <optional sign> <integer part> . <fractional part>
 *   <optional sign> <integer part> <exp> <optional sign> <integer>
 *   <optional sign> <integer part> . <fractional part> <exp> <optional sign> <integer>
 * 
 * where <optional sign> is + or - or nothing
 *       <exp> is either 'e' or 'E'
 */
EXPORTED rational_t *yices_string_float(char *s) {
  if (q_set_from_float_string(&r0, s) == 0) {
    return &r0;
  } else {
    return NULL;
  }
}

/*
 * Constant initialized via GMP integers or rationals.
 * - q must be canonicalized
 */
EXPORTED rational_t *yices_mpz(mpz_t z) {
  q_set_mpz(&r0, z);
  q_normalize(&r0);
  return &r0;
}

EXPORTED rational_t *yices_mpq(mpq_t q) {
  q_set_mpq(&r0, q);
  q_normalize(&r0);
  return &r0;
}



/*****************
 *  POLYNOMIALS  *
 ****************/

/*
 * Reset: set buffer to zero.
 */
EXPORTED void yices_arith_reset(arith_buffer_t *b) {
  arith_buffer_reset(b);
}

/*
 * Linear operations
 */
EXPORTED void yices_arith_negate(arith_buffer_t *b) {
  arith_buffer_negate(b);
}

EXPORTED void yices_arith_add_const(arith_buffer_t *b, rational_t *a) {
  arith_buffer_add_const(b, a);
}

EXPORTED void yices_arith_sub_const(arith_buffer_t *b, rational_t *a) {
  arith_buffer_sub_const(b, a);
}

EXPORTED void yices_arith_mul_const(arith_buffer_t *b, rational_t *a) {
  if (q_is_zero(a)) {
    arith_buffer_reset(b);
  } else {
    arith_buffer_mul_const(b, a);
  }
}

EXPORTED void yices_arith_add_buffer(arith_buffer_t *b, arith_buffer_t *b1) {
  rational_t aux;

  if (b == b1) {
    q_init(&aux);
    q_set32(&aux, 2);
    arith_buffer_mul_const(b, &aux);
    q_clear(&aux);
  } else {
    arith_buffer_add_buffer(b, b1);
  }
}

EXPORTED void yices_arith_sub_buffer(arith_buffer_t *b, arith_buffer_t *b1) {
  if (b == b1) {
    arith_buffer_reset(b);
  } else {
    arith_buffer_sub_buffer(b, b1);
  }
}

EXPORTED void yices_arith_add_const_times_buffer(arith_buffer_t *b, rational_t *a, arith_buffer_t *b1) {
  rational_t aux;

  if (q_is_nonzero(a)) {
    if (b == b1) {
      q_init(&aux);
      q_set(&aux, a);
      q_add_one(&aux);
     yices_arith_mul_const(b, &aux);
      q_clear(&aux);
    } else {
      arith_buffer_add_const_times_buffer(b, b1, a);
    }
  }
}

EXPORTED void yices_arith_sub_const_times_buffer(arith_buffer_t *b, rational_t *a, arith_buffer_t *b1) {
  rational_t aux;

  if (q_is_nonzero(a)) {
    if (b == b1) {
      q_init(&aux);
      q_set(&aux, a);
      q_sub_one(&aux);
     yices_arith_mul_const(b, &aux);
      q_clear(&aux);
    } else {
      arith_buffer_sub_const_times_buffer(b, b1, a);
    }
  }
}




/*
 * Linear operations involving terms:
 * - return -1 if the term is not well defined or if it does not have
 *   integer or real type.
 * - return 0 if the operation succeeded
 *
 * Error report
 * if t is not well defined
 *   code = INVALID_TERM
 *   term1 = t
 *   index = -1
 * if t has a bad type
 *   code = ARITHTERM_REQUIRED
 *   term1 = t
 */
EXPORTED int32_t yices_arith_add_term(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (! check_good_term(&terms, t) ||
      ! check_arith_term(&terms, t)) {
    return -1;
  }

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_add_monarray(b, p->mono);
  } else {
    arith_buffer_add_var(b, get_arithmetic_variable(&terms, t));
  }

  return 0;
}

EXPORTED int32_t yices_arith_sub_term(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (! check_good_term(&terms, t) ||
      ! check_arith_term(&terms, t)) {
    return -1;
  }

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_sub_monarray(b, p->mono);
  } else {
    arith_buffer_sub_var(b, get_arithmetic_variable(&terms, t));
  }

  return 0;
}

EXPORTED int32_t yices_arith_add_const_times_term(arith_buffer_t *b, rational_t *a, term_t t) {
  polynomial_t *p;

  if (! check_good_term(&terms, t) ||
      ! check_arith_term(&terms, t)) {
    return -1;
  }

  if (q_is_zero(a)) return 0;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_add_const_times_monarray(b, p->mono, a);
  } else {
    arith_buffer_add_mono(b, get_arithmetic_variable(&terms, t), a);
  }

  return 0;
}

EXPORTED int32_t yices_arith_sub_const_times_term(arith_buffer_t *b, rational_t *a, term_t t) {
  polynomial_t *p;

  if (! check_good_term(&terms, t) ||
      ! check_arith_term(&terms, t)) {
    return -1;
  }

  if (q_is_zero(a)) return 0;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_sub_const_times_monarray(b, p->mono, a);
  } else {
    arith_buffer_sub_mono(b, get_arithmetic_variable(&terms, t), a);
  }

  return 0;
}



/*
 * Non-linear operations:
 * - return -1 if the operation failed (either because the terms are incorrect)
 *   or because there's an overflow in the polynomial degree.
 * - return 0 otherwise
 */
EXPORTED int32_t yices_arith_mul_buffer(arith_buffer_t *b, arith_buffer_t *b1) {
  if (! check_arith_buffer_degree(b) ||
      ! check_arith_buffer_degree(b1)) {
    return -1;
  }

  if (b == b1) {
    arith_buffer_square(b); 
    return 0;
  }

  arith_buffer_mul_buffer(b, b1);
  return 0;
}

EXPORTED int32_t yices_arith_mul_term(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (! check_good_term(&terms, t) || 
      ! check_arith_term(&terms, t) || 
      ! check_arith_buffer_degree(b)) {
    return -1;
  }

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    if (! check_poly_degree(p)) {
      return -1;
    }
    arith_buffer_mul_monarray(b, p->mono); 
  } else {
    arith_buffer_mul_var(b, get_arithmetic_variable(&terms, t));
  }
  return 0;
}

EXPORTED int32_t yices_arith_square(arith_buffer_t *b) {
  if (! check_arith_buffer_degree(b)) {
    return -1;
  }
  arith_buffer_square(b);
  return 0;
}



/**********************************************
 *  CONVERSION OF BUFFERS TO TERMS AND ATOMS  *
 *********************************************/

/*
 * Convert to term t if b is reduced to a variable x and theory_var[t] == x.
 */
EXPORTED term_t yices_arith_term(arith_buffer_t *b) {
  arith_var_t x;
  term_t t;

  arith_buffer_normalize(b);
  if (b->nterms == 1 && q_is_one(&b->list->next->coeff)) {
    x = b->list->next->var;
    if (polymanager_var_is_primitive(&arith_manager.pm, x)) {
      t = polymanager_var_index(&arith_manager.pm, x);
      assert(term_theory_var(&terms, t) == x);
      return t;
    }
  }
  return arith_term(&terms, b);
}


/*
 * Atom (b == 0).
 * simplify to true if b is the zero polynomial
 * simplify to false if b is constant and nonzero
 * rewrite to equality (t1 == t2) if possible
 *
 * TODO:
 * - more normalization on b (e.g., -b == 0 is equivalent to b == 0)
 * - detect some integer impossibilities (e.g., 3x + 3y - 2 = 0
 *   if x and y are integer)
 */
EXPORTED term_t yices_arith_eq0_atom(arith_buffer_t *b) {
  arith_var_t x, y;
  term_t left, right, aux;

  arith_buffer_normalize(b);

  if (arith_buffer_is_zero(b)) {
    return true_term(&terms);
  }

  if (arith_buffer_is_nonzero(b)) {
    return false_term(&terms);
  }

  x = null_theory_var;
  y = null_theory_var; // otherwise GCC complains

  if (arith_buffer_is_equality(b, &x, &y)) {
    // convert to left == right
    left = arithvar_manager_term_of_var(&arith_manager, x);
    right = arithvar_manager_term_of_var(&arith_manager, y);
    assert(is_arithmetic_term(&terms, left) && is_arithmetic_term(&terms, right));

    // normalize
    if (left > right) {
      aux = left; left = right; right = aux;
    }

    return arith_bineq_atom(&terms, left, right);

  } else {
    // build atom (b == 0)
    return arith_eq_atom(&terms, b);
  }
}

/*
 * Atom (b /= 0).
 * simplify to false if b is the zero polynomial
 * simplify to true  if b is constant and nonzero 
 * rewrite to (not (x == y)) if possible
 * otherwise return (not (b == 0))
 */
EXPORTED term_t yices_arith_neq0_atom(arith_buffer_t *b) {
  return mk_not(&terms, yices_arith_eq0_atom(b));
}

/*
 * Atom b >= 0
 * simplify to true if b is zero or a positive constant
 * simplify to false if b is a negative constant
 */
EXPORTED term_t yices_arith_geq0_atom(arith_buffer_t *b) {
  arith_buffer_normalize(b);

  if (arith_buffer_is_nonneg(b)) {
    return true_term(&terms);
  }

  if (arith_buffer_is_neg(b)) {
    return false_term(&terms);
  }
  
  return arith_geq_atom(&terms, b);
}

/*
 * Atom b <= 0, converted to (- b) >= 0
 */
EXPORTED term_t yices_arith_leq0_atom(arith_buffer_t *b) {
  arith_buffer_normalize(b);

  if (arith_buffer_is_nonpos(b)) {
    return true_term(&terms);
  }

  if (arith_buffer_is_pos(b)) {
    return false_term(&terms);
  }

  arith_buffer_negate(b); // still normalizedd
  return arith_geq_atom(&terms, b);
}


/*
 * Atom b>0, converted to (not ((- b) >= 0))
 */
EXPORTED term_t yices_arith_gt0_atom(arith_buffer_t *b) {
  arith_buffer_normalize(b);

  if (arith_buffer_is_nonpos(b)) {
    return false_term(&terms);
  }

  if (arith_buffer_is_pos(b)) {
    return true_term(&terms);
  }

  arith_buffer_negate(b);
  return not_term(&terms, arith_geq_atom(&terms, b));
}


/*
 * Atom b<0, converted to not (b >= 0)
 */
EXPORTED term_t yices_arith_lt0_atom(arith_buffer_t *b) {
  arith_buffer_normalize(b);

  if (arith_buffer_is_nonneg(b)) {
    return false_term(&terms);
  }

  if (arith_buffer_is_neg(b)) {
    return true_term(&terms);
  }

  return not_term(&terms, arith_geq_atom(&terms, b));
}




/********************************************
 * DIRECT CONSTRUCTION OF ARITHMETIC TERMS  *
 *******************************************/

/*
 * Convert rational a to a term
 * - a must be non-null
 */
EXPORTED term_t yices_arith_constant(rational_t *a) {
  arith_buffer_t *b;

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_buffer_add_const(b, a);

  return yices_arith_term(b);
}



/*
 * POLYNOMIALS
 */
static void arith_add(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_add_monarray(b, p->mono);
  } else {
    arith_buffer_add_var(b, get_arithmetic_variable(&terms, t));
  }  
}

static void arith_sub(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_sub_monarray(b, p->mono);
  } else {
    arith_buffer_sub_var(b, get_arithmetic_variable(&terms, t));
  } 
}

static void arith_mul(arith_buffer_t *b, term_t t) {
  polynomial_t *p;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_mul_monarray(b, p->mono);
  } else {
    arith_buffer_mul_var(b, get_arithmetic_variable(&terms, t));
  }
}

static void arith_set_square(arith_buffer_t *b, term_t t) {
  polynomial_t *p;
  int32_t v;

  if (term_kind(&terms, t) == ARITH_TERM) {
    p = arith_term_desc(&terms, t);
    arith_buffer_add_monarray(b, p->mono);
    arith_buffer_mul_monarray(b, p->mono);
  } else {
    v = get_arithmetic_variable(&terms, t);
    arith_buffer_add_var(b, v);
    arith_buffer_mul_var(b, v);
  }
}



/*
 * Add t1 and t2
 */
EXPORTED term_t yices_add(term_t t1, term_t t2) {
  arith_buffer_t *b;

  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_add(b, t1);
  arith_add(b, t2);

  return yices_arith_term(b);
}


/*
 * Subtract t2 from t1
 */
EXPORTED term_t yices_sub(term_t t1, term_t t2) {
  arith_buffer_t *b;

  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_add(b, t1);
  arith_sub(b, t2);

  return yices_arith_term(b);
}



/*
 * Negate t1
 */
EXPORTED term_t yices_neg(term_t t1) {
  arith_buffer_t *b;

  if (! check_good_term(&terms, t1) || 
      ! check_arith_term(&terms, t1)) {
    return NULL_TERM;
  }

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_sub(b, t1);

  return yices_arith_term(b);
}




/*
 * Multiply t1 and t2
 */
EXPORTED term_t yices_mul(term_t t1, term_t t2) {
  arith_buffer_t *b;

  if (! check_both_arith_terms(&terms, t1, t2) ||
      ! check_arith_term_degree(&terms, t1) || 
      ! check_arith_term_degree(&terms, t2)) {
    return NULL_TERM;
  }

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_add(b, t1);
  arith_mul(b, t2);

  return yices_arith_term(b);
}


/*
 * Compute the square of t1
 */
EXPORTED term_t yices_square(term_t t1) {
  arith_buffer_t *b;

  if (! check_good_term(&terms, t1) || 
      ! check_arith_term(&terms, t1) ||
      ! check_arith_term_degree(&terms, t1)) {
    return NULL_TERM;
  }

  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_set_square(b, t1);

  return yices_arith_term(b);  
}











/*********************************************
 *  DIRECT CONSTRUCTION OF ARITHMETIC ATOMS  *
 ********************************************/

/*
 * Store t1 - t2 in internal_arith_buffer.
 */
static void mk_arith_diff(term_t t1, term_t t2) {
  arith_buffer_t *b;
  
  b = get_internal_arith_buffer();
  arith_buffer_reset(b);
  arith_add(b, t1);
  arith_sub(b, t2);
}


/*
 * Build the term (ite c (aritheq t1 t2) (aritheq t3 t4))
 * - c is a boolean term
 * - t1, t2, t3, t4 are all arithmetic terms
 */
static term_t mk_lifted_aritheq(term_t c, term_t t1, term_t t2, term_t t3, term_t t4) {
  term_t left, right;

  mk_arith_diff(t1, t2);
  left = yices_arith_eq0_atom(internal_arith_buffer);
  mk_arith_diff(t3, t4);
  right = yices_arith_eq0_atom(internal_arith_buffer);

  return mk_bool_ite(&terms, c, left, right);
}

/*
 * Build the term (ite c (arithge t1 t2) (arithge t3 t4))
 * - c is a boolean term
 * - t1, t2, t3, t4 are all arithmetic terms
 */
static term_t mk_lifted_arithgeq(term_t c, term_t t1, term_t t2, term_t t3, term_t t4) {
  term_t left, right;

  mk_arith_diff(t1, t2);
  left = yices_arith_geq0_atom(internal_arith_buffer);
  mk_arith_diff(t3, t4);
  right = yices_arith_geq0_atom(internal_arith_buffer);

  return mk_bool_ite(&terms, c, left, right);
}



/*
 * Equality term (aritheq t1 t2)
 *
 * Apply the cheap lift-if rules
 *  (eq x (ite c y z))  ---> (ite c (eq x y) (eq x z)) provided x is not an if term
 *  (eq (ite c x y) z)) ---> (ite c (eq x z) (eq y z)) provided z is not an if term
 *  (eq (ite c x y) (ite c z u)) --> (ite c (eq x z) (eq y u))
 *
 */
static term_t mk_aritheq(term_t t1, term_t t2) {
  lift_result_t tmp;

  assert(is_arithmetic_term(&terms, t1) && is_arithmetic_term(&terms, t2));

  if (check_for_lift_if(&terms, t1, t2, &tmp)) {
    return mk_lifted_aritheq(tmp.cond, tmp.left1, tmp.left2, tmp.right1, tmp.right2);
  } 

  mk_arith_diff(t1, t2);
  return yices_arith_eq0_atom(internal_arith_buffer);
}

static term_t mk_arithgeq(term_t t1, term_t t2) {
  lift_result_t tmp;

  assert(is_arithmetic_term(&terms, t1) && is_arithmetic_term(&terms, t2));

  if (check_for_lift_if(&terms, t1, t2, &tmp)) {
    return mk_lifted_arithgeq(tmp.cond, tmp.left1, tmp.left2, tmp.right1, tmp.right2);
  } 

  mk_arith_diff(t1, t2);
  return yices_arith_geq0_atom(internal_arith_buffer);  
}

static term_t mk_arithneq(term_t t1, term_t t2) {
  return mk_not(&terms, mk_aritheq(t1, t2));
}




/*
 * API Calls
 */
// t1 == t2
EXPORTED term_t yices_aritheq_atom(term_t t1, term_t t2) {
  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_aritheq(t1, t2);
}

// (t1 != t2) is not (t1 == t2)
EXPORTED term_t yices_arithneq_atom(term_t t1, term_t t2) {
  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_not(&terms, mk_aritheq(t1, t2));
}

// build t1 >= t2
EXPORTED term_t yices_arithgeq_atom(term_t t1, term_t t2) {
  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_arithgeq(t1, t2);
}

// (t1 < t2) is (not (t1 >= t2))
EXPORTED term_t yices_arithlt_atom(term_t t1, term_t t2) {
  if (! check_both_arith_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_not(&terms, mk_arithgeq(t1, t2));
}

// (t1 > t2) is (t2 < t1)
EXPORTED term_t yices_arithgt_atom(term_t t1, term_t t2) {
  return yices_arithlt_atom(t2, t1);
}

// (t1 <= t2) is (t2 >= t1)
EXPORTED term_t yices_arithleq_atom(term_t t1, term_t t2) {
  return yices_arithgeq_atom(t2, t1);
}








/**************************
 *  BITVECTOR CONSTANTS   *
 *************************/

/*
 * All functions return and pointer to bv0
 * or NULL if there's an error (including if n <= 0)
 */

/*
 * Initialize bv0 from 32 or 64 bit integer x, or from a GMP integer
 * n = number of bits must be positive.
 * if n>32 or n>64, the highorder bits are 0
 */
EXPORTED bvconstant_t * yices_bvconst_uint32(int32_t n, uint32_t x) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set32(bv0.data, bv0.width, x);
  bvconst_normalize(bv0.data, n);  

  return &bv0;
}

EXPORTED bvconstant_t * yices_bvconst_uint64(int32_t n, uint64_t x) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set64(bv0.data, bv0.width, x);
  bvconst_normalize(bv0.data, n);  

  return &bv0;
}

EXPORTED bvconstant_t * yices_bvconst_mpz(int32_t n, mpz_t x) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set_mpz(bv0.data, bv0.width, x);
  bvconst_normalize(bv0.data, n);  

  return &bv0;  
}


/*
 * Parse a string of '0' and '1' and convert to a bit constant
 * - return NULL if there's a format error (including if s is the empty string)
 * - the number of bits is the length of s
 */
EXPORTED bvconstant_t * yices_bvconst_from_string(char *s) {
  uint32_t n;
  int code;

  n = strlen(s);
  if (n == 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  code = bvconst_set_from_string(bv0.data, n, s);
  bvconst_normalize(bv0.data, n);  

  return code == 0 ? &bv0 : NULL;
}


/*
 * Parse a string of hexa decimal digits and convert it to a bit constant
 * - return NULL if there's a format error 
 * - the number of bits is four times the length of s
 */
EXPORTED bvconstant_t * yices_bvconst_from_hexa_string(char *s) {
  uint32_t n;
  int code;

  n = strlen(s);
  if (n == 0) return NULL;

  bvconstant_set_bitsize(&bv0, 4 * n);
  code = bvconst_set_from_hexa_string(bv0.data, n, s);
  bvconst_normalize(bv0.data, 4 * n);

  return code == 0 ? &bv0 : NULL;
}



/*
 * Convert an integer array to a bit constant
 */
EXPORTED bvconstant_t * yices_bvconst_from_array(int32_t n, int32_t a[]) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set_array(bv0.data, a, n);
  bvconst_normalize(bv0.data, n);  

  return &bv0;
}



/*
 * bvconst_zero: set all bits to 0
 * bvconst_one: set low-order bit to 1, all the others to 0
 * bvconst_minus_one: set all bits to 1
 */
EXPORTED bvconstant_t * yices_bvconst_zero(int32_t n) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_clear(bv0.data, bv0.width);
  return &bv0;
}

EXPORTED bvconstant_t * yices_bvconst_one(int32_t n) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set_one(bv0.data, bv0.width);

  return &bv0;
}

EXPORTED bvconstant_t * yices_bvconst_minus_one(int32_t n) {
  if (n <= 0) return NULL;

  bvconstant_set_bitsize(&bv0, n);
  bvconst_set_minus_one(bv0.data, bv0.width);
  bvconst_normalize(bv0.data, n);  

  return &bv0;
}






/**********************************
 *  BITVECTOR ARITHMETIC BUFFERS  *
 *********************************/

/*
 * Reset buffer to 0b00...0 with n bits
 * No change if n <= 0.
 */
EXPORTED void yices_bvarith_reset(bvarith_buffer_t *b, int32_t n) {
  if (n > 0) {
    bvarith_buffer_prepare(b, n);
  }
}





/*
 * Assignment: copy a constant into b.
 * - set b's bitsize to that of c
 */
EXPORTED void yices_bvarith_set_const(bvarith_buffer_t *b, bvconstant_t *c) {
  uint32_t n;

  n = c->bitsize;
  assert(n > 0);
  bvarith_buffer_prepare(b, n);
  bvarith_buffer_add_const(b, c->data);
}

/*
 * Copy another buffer into b: b's size is adjusted.
 * no effect if b1 == b
 */
EXPORTED void yices_bvarith_set_buffer(bvarith_buffer_t *b, bvarith_buffer_t *b1) {
  uint32_t n;

  if (b1 != b) {
    n = b1->size;
    bvarith_buffer_prepare(b, n);
    bvarith_buffer_add_buffer(b, b1);
  }
}


/*
 * Assignment: copy term t into b.
 * Return -1 if there's an error, 0 otherwise
 *
 * Error report:
 * if t is invalid
 *    code = INVALID_TERM
 *    term1 = t
 *    index = -1
 * if t does not have bitvector type
 *    code = BITVECTOR_REQUIRED
 *    term1 = t
 */
EXPORTED int32_t yices_bvarith_set_term(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;
  int32_t n;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return -1;
  }

  n = term_bitsize(&terms, t);
  bvarith_buffer_prepare(b, n);
  
  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_add_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_add_const(b, c->bits);
    break;

  default:
    bvarith_buffer_add_var(b, get_bitvector_variable(&terms, t));
    break;
  }

  return 0;  
}





/*
 * Bitvector arithmetic operations
 *
 * All return -1 if there's an error, 0 otherwise.
 * Exception: bvnegate never fails.
 *
 * Error report:
 * if buffer and arguments do not have the same size
 *   code = INCOMPATIBLE_BVSIZES
 *   badval = size of argument
 * if term t is not well defined
 *   code = INVALID_TERM
 *   index = -1
 * if t is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if degree is too large (in mul operations)
 *   code = DEGREE_OVERFLOW
 */
EXPORTED void yices_bvarith_negate(bvarith_buffer_t *b) {
  bvarith_buffer_negate(b);  
}

EXPORTED int32_t yices_bvarith_add_const(bvarith_buffer_t *b, bvconstant_t *c) {
  if (b->size != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvarith_buffer_add_const(b, c->data);
  return 0;
}

EXPORTED int32_t yices_bvarith_sub_const(bvarith_buffer_t *b, bvconstant_t *c) {
  if (b->size != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvarith_buffer_sub_const(b, c->data);
  return 0;
}

EXPORTED int32_t yices_bvarith_mul_const(bvarith_buffer_t *b, bvconstant_t *c) {
  if (b->size != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  if (bvconst_is_zero(c->data, c->width)) {
    // reset to zero
    bvarith_buffer_prepare(b, b->size);
  } else {
    bvarith_buffer_mul_const(b, c->data);
  }

  return 0;
}


EXPORTED int32_t yices_bvarith_add_buffer(bvarith_buffer_t *b, bvarith_buffer_t *b1) {
  if (b->size != b1->size) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->size;
    return -1;
  }

  bvarith_buffer_add_buffer(b, b1);
  return 0;
}

EXPORTED int32_t yices_bvarith_sub_buffer(bvarith_buffer_t *b, bvarith_buffer_t *b1) {
  if (b->size != b1->size) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->size;
    return -1;
  }

  bvarith_buffer_sub_buffer(b, b1);
  return 0;
}

EXPORTED int32_t yices_bvarith_mul_buffer(bvarith_buffer_t *b, bvarith_buffer_t *b1) {
  if (b->size != b1->size) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->size;
    return -1;
  }

  if (! check_bvarith_buffer_degree(b) ||
      ! check_bvarith_buffer_degree(b1)) {
    return -1;
  }

  if (b == b1) {
    bvarith_buffer_square(b);
    return 0;
  }

  bvarith_buffer_mul_buffer(b, b1);
  return 0;
}


EXPORTED int32_t yices_bvarith_add_term(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->size)) {
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_add_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_add_const(b, c->bits);
    break;

  default:
    bvarith_buffer_add_var(b, get_bitvector_variable(&terms, t));
    break;
  }

  return 0;
}

EXPORTED int32_t yices_bvarith_sub_term(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->size)) {
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_sub_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_sub_const(b, c->bits);
    break;

  default:
    bvarith_buffer_sub_var(b, get_bitvector_variable(&terms, t));
    break;
  }  

  return 0;
}

EXPORTED int32_t yices_bvarith_mul_term(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->size) ||
      ! check_bvarith_buffer_degree(b)) {
    return -1;
  }
  
  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    if (! check_bvarith_expr_degree(p)) {
      return -1;
    }
    bvarith_buffer_mul_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    if (bvconst_is_zero(c->bits, c->nbits)) {
      bvarith_buffer_prepare(b, b->size); // reset to zero
    } else {
      bvarith_buffer_mul_const(b, c->bits);
    }
    break;

  default:
    bvarith_buffer_mul_var(b, get_bitvector_variable(&terms, t));
    break;
  }

  return 0;  
}

EXPORTED int32_t yices_bvarith_square(bvarith_buffer_t *b) {
  if (! check_bvarith_buffer_degree(b)) {
    return -1;
  }
  bvarith_buffer_square(b);
  return 0;
}





/*
 * CONVERSION FROM  BVARITH BUFFER TO BVLOGIC BUFFER
 *
 * TODO: Reorganize the code. This module should not
 * have to know the detailed internal structure of the 
 * bvarith buffers.
 */


/*
 * Convert b to a term: 
 * if b is reduced to a single variable x, return the term attached to x
 * if b is constant, build a BV_CONST_TERM
 * if b can be converted to a bit array (BV_LOGIC_TERM) return that
 * otherwise construct a BV_ARITH_TERM
 */
EXPORTED term_t yices_bvarith_term(bvarith_buffer_t *b) {
  bv_var_t x;
  term_t t;

  bvarith_buffer_normalize(b);

  if (bvarith_buffer_is_constant(b)) {
    bvarith_buffer_copy_constant(b, &bv1);
    return bvconst_term(&terms, bv1.bitsize, bv1.data);
  }

  if (bvarith_buffer_is_variable(b)) {
    x = bvarith_buffer_first_var(b);
    if (polymanager_var_is_primitive(&bv_manager.pm, x)) {
      t = polymanager_var_index(&bv_manager.pm, x);
      assert(term_theory_var(&terms, t) == x);
      return t;
    }
  }

  t = convert_bvarith_to_bvlogic_term(b);
  if (t != NULL_TERM) {
    return t;
  }

  return bvarith_term(&terms, b);
}




/********************************
 *   BITVECTOR LOGIC BUFFERS    *
 *******************************/

/*
 * Reset b to the empty vector
 */
EXPORTED void yices_bvlogic_reset(bvlogic_buffer_t *b) {
  bvlogic_buffer_clear(b);
}


/*
 * Copy constant c into b
 */
EXPORTED void yices_bvlogic_set_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  bvlogic_buffer_set_constant(b, c->bitsize, c->data);
}


/*
 * Copy buffer b1 into b
 */
EXPORTED void yices_bvlogic_set_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  if (b != b1) {
    bvlogic_buffer_set_bitarray(b, b1->nbits, b1->bit);
  }
}


/*
 * Copy term t into b
 */
EXPORTED int32_t yices_bvlogic_set_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_set_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_set_constant(b, c->nbits, c->bits);
    break;
    
  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_set_bitarray(b, n, bits);
    break;    
  }

  return 0;
}



/*
 * Bitwise not 
 */
EXPORTED void yices_bvlogic_not(bvlogic_buffer_t *b) {
  bvlogic_buffer_not(b);
}



/*
 * Bitwise and, or, xor
 */
EXPORTED int32_t yices_bvlogic_and_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  if (b->nbits != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvlogic_buffer_and_constant(b, c->bitsize, c->data);
  return 0;
}

EXPORTED int32_t yices_bvlogic_or_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  if (b->nbits != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvlogic_buffer_or_constant(b, c->bitsize, c->data);
  return 0;
}

EXPORTED int32_t yices_bvlogic_xor_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  if (b->nbits != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvlogic_buffer_xor_constant(b, c->bitsize, c->data);
  return 0;
}

// more 
EXPORTED int32_t yices_bvlogic_nand_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  int32_t code;

  code = yices_bvlogic_and_const(b, c);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_nor_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  int32_t code;

  code = yices_bvlogic_or_const(b, c);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_xnor_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  int32_t code;

  code = yices_bvlogic_xor_const(b, c);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}


/*
 * operation between buffer and another buffer b1
 */
EXPORTED int32_t yices_bvlogic_and_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  if (b->nbits != b1->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->nbits;
    return -1;
  }
  bvlogic_buffer_and_bitarray(b, b1->nbits, b1->bit);

  return 0;
}

EXPORTED int32_t yices_bvlogic_or_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  if (b->nbits != b1->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->nbits;
    return -1;
  }
  bvlogic_buffer_or_bitarray(b, b1->nbits, b1->bit);

  return 0;
}

EXPORTED int32_t yices_bvlogic_xor_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  if (b->nbits != b1->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->nbits;
    return -1;
  }
  bvlogic_buffer_xor_bitarray(b, b1->nbits, b1->bit);

  return 0;
}


EXPORTED int32_t yices_bvlogic_nand_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  int32_t code;

  code = yices_bvlogic_and_buffer(b, b1);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_nor_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  int32_t code;

  code = yices_bvlogic_or_buffer(b, b1);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_xnor_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  int32_t code;

  code = yices_bvlogic_xor_buffer(b, b1);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}



/*
 * Bitwise operations with a single term
 */
EXPORTED int32_t yices_bvlogic_and_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->nbits)) {    
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_and_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_and_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_and_bitarray(b, n, bits);
    break;
  }

  return 0;
}


EXPORTED int32_t yices_bvlogic_or_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->nbits)) {    
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_or_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_or_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_or_bitarray(b, n, bits);
    break;
  }

  return 0;
}


EXPORTED int32_t yices_bvlogic_xor_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->nbits)) {    
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_xor_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_xor_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_xor_bitarray(b, n, bits);
    break;
  }

  return 0;
}



EXPORTED int32_t yices_bvlogic_nand_term(bvlogic_buffer_t *b, term_t t) {
  int32_t code;

  code = yices_bvlogic_and_term(b, t);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_nor_term(bvlogic_buffer_t *b, term_t t) {
  int32_t code;

  code = yices_bvlogic_or_term(b, t);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_xnor_term(bvlogic_buffer_t *b, term_t t) {
  int32_t code;

  code = yices_bvlogic_xor_term(b, t);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}



/*
 * Bitwise operation with n terms arg[0] ... arg[n-1]
 */
EXPORTED int32_t yices_bvlogic_and_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {  
  int32_t i;
  term_t t;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_terms(&terms, n, arg) ||
      ! check_good_bitvectors(&terms, n, arg, b->nbits)) {
    return -1;
  }

  for (i=0; i<n; i++) {
    t = arg[i];
    switch (term_kind(&terms, t)) {
    case BV_LOGIC_TERM:
      e = bvlogic_term_desc(&terms, t);
      assert(e->nbits == b->nbits);
      bvlogic_buffer_and_bitarray(b, e->nbits, e->bit);
      break;

    case BV_CONST_TERM:
      c = bvconst_term_desc(&terms, t);
      assert(c->nbits == b->nbits);
      bvlogic_buffer_and_constant(b, c->nbits, c->bits);
      break;

    default:
      assert(term_bitsize(&terms, t) == b->nbits);
      x = get_bitvector_variable(&terms, arg[i]);
      bits = bv_var_manager_get_bit_array(&bv_manager, x);
      bvlogic_buffer_and_bitarray(b, b->nbits, bits);
      break;
    }
  }

  return 0;
}



EXPORTED int32_t yices_bvlogic_or_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {  
  int32_t i;
  term_t t;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_terms(&terms, n, arg) ||
      ! check_good_bitvectors(&terms, n, arg, b->nbits)) {
    return -1;
  }

  for (i=0; i<n; i++) {
    t = arg[i];
    switch (term_kind(&terms, t)) {
    case BV_LOGIC_TERM:
      e = bvlogic_term_desc(&terms, t);
      assert(e->nbits == b->nbits);
      bvlogic_buffer_or_bitarray(b, e->nbits, e->bit);
      break;

    case BV_CONST_TERM:
      c = bvconst_term_desc(&terms, t);
      assert(c->nbits == b->nbits);
      bvlogic_buffer_or_constant(b, c->nbits, c->bits);
      break;

    default:
      assert(term_bitsize(&terms, t) == b->nbits);
      x = get_bitvector_variable(&terms, arg[i]);
      bits = bv_var_manager_get_bit_array(&bv_manager, x);
      bvlogic_buffer_or_bitarray(b, b->nbits, bits);
      break;
    }
  }
    
  return 0;
}



EXPORTED int32_t yices_bvlogic_xor_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {  
  int32_t i;
  term_t t;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_terms(&terms, n, arg) ||
      ! check_good_bitvectors(&terms, n, arg, b->nbits)) {
    return -1;
  }

  for (i=0; i<n; i++) {
    t = arg[i];
    switch (term_kind(&terms, t)) {
    case BV_LOGIC_TERM:
      e = bvlogic_term_desc(&terms, t);
      assert(e->nbits == b->nbits);
      bvlogic_buffer_xor_bitarray(b, e->nbits, e->bit);
      break;

    case BV_CONST_TERM:
      c = bvconst_term_desc(&terms, t);
      assert(c->nbits == b->nbits);
      bvlogic_buffer_xor_constant(b, c->nbits, c->bits);
      break;

    default:
      assert(term_bitsize(&terms, t) == b->nbits);
      x = get_bitvector_variable(&terms, arg[i]);
      bits = bv_var_manager_get_bit_array(&bv_manager, x);
      bvlogic_buffer_xor_bitarray(b, b->nbits, bits);
      break;
    }
  }
    
  return 0;
}


EXPORTED int32_t yices_bvlogic_nand_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {
  int32_t code;

  code = yices_bvlogic_and_terms(b, n, arg);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_nor_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {
  int32_t code;

  code = yices_bvlogic_or_terms(b, n, arg);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

EXPORTED int32_t yices_bvlogic_xnor_terms(bvlogic_buffer_t *b, int32_t n, term_t arg[]) {
  int32_t code;

  code = yices_bvlogic_xor_terms(b, n, arg);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}



/*
 * Shift and rotate
 */
EXPORTED int32_t yices_bvlogic_shift_left0(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_shift_left0(b, n);
  return 0;
}

EXPORTED int32_t yices_bvlogic_shift_left1(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_shift_left1(b, n);
  return 0;
}

EXPORTED int32_t yices_bvlogic_shift_right0(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_shift_right0(b, n);
  return 0;
}

EXPORTED int32_t yices_bvlogic_shift_right1(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_shift_right1(b, n);
  return 0;
}

EXPORTED int32_t yices_bvlogic_ashift_right(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_ashift_right(b, n);
  return 0;
}

EXPORTED int32_t yices_bvlogic_rotate_left(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  if (n < b->nbits) {
    // bvlogic_buffer_rotate require n < b->nbits
    // if n == b->nbits, we do nothing
    bvlogic_buffer_rotate_left(b, n);
  }

  return 0;
}

EXPORTED int32_t yices_bvlogic_rotate_right(bvlogic_buffer_t *b, int32_t n) {
  if (! check_bitshift(n, b->nbits)) {
    return -1;
  }

  if (n < b->nbits) {
    // bvlogic_buffer_rotate require n < b->nbits
    // if n == b->nbits, nothing to do
    bvlogic_buffer_rotate_right(b, n);
  }

  return 0;
}



/*
 * Extract b[j ... i] from b[m-1 ... 0]
 */
EXPORTED int32_t yices_bvlogic_extract(bvlogic_buffer_t *b, int32_t i, int32_t j) {
  if (! check_bitextract(i, j, b->nbits)) {
    return -1;
  }

  bvlogic_buffer_extract_subvector(b, i, j);
  return 0;
}


/*
 * Concatenation:
 * if b is b[m-1] ... b[0] then concat left adds bits to the left of b[m-1]
 * concat right adds bits to the right of b[0].
 */
EXPORTED void yices_bvlogic_concat_left_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  bvlogic_buffer_concat_left_constant(b, c->bitsize, c->data);
}

EXPORTED void yices_bvlogic_concat_right_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  bvlogic_buffer_concat_right_constant(b, c->bitsize, c->data);
}

EXPORTED void yices_bvlogic_concat_left_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  bvlogic_buffer_concat_left_bitarray(b, b1->nbits, b1->bit);
}

EXPORTED void yices_bvlogic_concat_right_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  bvlogic_buffer_concat_right_bitarray(b, b1->nbits, b1->bit);
}


EXPORTED int32_t yices_bvlogic_concat_left_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_concat_left_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_concat_left_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_concat_left_bitarray(b, n, bits);
    break;
  }

  return 0;
}


EXPORTED int32_t yices_bvlogic_concat_right_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_concat_right_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_concat_right_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_concat_right_bitarray(b, n, bits);
    break;
  }

  return 0;
}


/*
 * Repeat concatenation: make n copies of b
 * - n must be positive
 */
EXPORTED int32_t yices_bvlogic_repeat(bvlogic_buffer_t *b, int32_t n) {
  if (! check_positive(n)) {
    return -1;
  }
  bvlogic_buffer_repeat_concat(b, n);
  return 0;
}

/*
 * Sign-extension:
 * if b is b[m-1] ... b[0] then copy the sign bit b[m-1] n times to the left of b[m-1] 
 * returns -1 if  m == 0 or n < 0
 */
EXPORTED int32_t yices_bvlogic_sign_extend(bvlogic_buffer_t *b, int32_t n) {
  if (b->nbits <= 0 || n < 0) {
    error.code = INVALID_BVSIGNEXTEND;
    return -1;
  }

  bvlogic_buffer_sign_extend(b, b->nbits + n);
  return 0;
}

/*
 * Zero-extension:
 * if b is b[m-1] ... b[0] then copy bit 0 n times to the left of b[m-1] 
 * returns -1 if m == 0 or n < 0
 */
EXPORTED int32_t yices_bvlogic_zero_extend(bvlogic_buffer_t *b, int32_t n) {
  if (b->nbits <= 0 || n < 0) {
    error.code = INVALID_BVZEROEXTEND;
    return -1;
  }

  bvlogic_buffer_zero_extend(b, b->nbits + n);
  return 0;
}




/*
 * AND reduction
 */
EXPORTED int32_t yices_bvlogic_redand(bvlogic_buffer_t *b) {
  if (b->nbits == 0) {
    error.code = EMPTY_BITVECTOR;
    return -1;
  }

  bvlogic_buffer_redand(b);
  return 0;
}


/*
 * OR reduction
 */
EXPORTED int32_t yices_bvlogic_redor(bvlogic_buffer_t *b) {
  if (b->nbits == 0) {
    error.code = EMPTY_BITVECTOR;
    return -1;
  }

  bvlogic_buffer_redor(b);
  return 0;
}



/*
 * BITWISE COMPARISON
 */
EXPORTED int32_t yices_bvlogic_comp_const(bvlogic_buffer_t *b, bvconstant_t *c) {
  if (b->nbits != c->bitsize) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = c->bitsize;
    return -1;
  }

  bvlogic_buffer_comp_constant(b, c->bitsize, c->data);
  return 0;
}


EXPORTED int32_t yices_bvlogic_comp_buffer(bvlogic_buffer_t *b, bvlogic_buffer_t *b1) {
  if (b->nbits != b1->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = b1->nbits;
    return -1;
  }

  bvlogic_buffer_comp_bitarray(b, b1->nbits, b1->bit);
  return 0;
}


EXPORTED int32_t yices_bvlogic_comp_term(bvlogic_buffer_t *b, term_t t) {
  int32_t n;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitsize(&terms, t, b->nbits)) {    
    return -1;
  }

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_comp_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_comp_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_comp_bitarray(b, n, bits);
    break;
  }

  return 0;
}





/*
 * Convert buffer to a term
 * return NULL_TERM is b is the empty vector
 *
 * if b is reduced to a single variable x, return the term attached to x
 * if b is constant, build a BV_CONST_TERM
 * otherwise construct a BV_LOGIC_TERM
 */
EXPORTED term_t yices_bvlogic_term(bvlogic_buffer_t *b) {
  bv_var_t x;
  term_t t;

  if (b->nbits == 0) {
    error.code = EMPTY_BITVECTOR;
    return NULL_TERM;
  }

  if (bvlogic_buffer_is_constant(b)) {
    bvlogic_buffer_copy_constant(b, &bv1);
    return bvconst_term(&terms, bv1.bitsize, bv1.data);
  }

  //check whether b is equal to a bv_variable x
  if (bvlogic_buffer_is_variable(b, &bv_manager)) {
    x = bvlogic_buffer_get_variable(b);
    if (polymanager_var_is_primitive(&bv_manager.pm, x)) {
      t = polymanager_var_index(&bv_manager.pm, x);
      assert(term_theory_var(&terms, t) == x);
      return t;
    }
  }

  return bvlogic_term(&terms, b);
}









/********************************************
 * DIRECT CONSTRUCTION OF BIT-VECTOR TERMS  *
 *******************************************/


/*
 * CONSTANTS
 */

/*
 * Direct construction of a bvconst term. Fail if n is zero.
 */
EXPORTED term_t yices_bvconst_term(uint32_t n, uint32_t *bv) {
  if (n == 0) {
    error.code = EMPTY_BITVECTOR;
    return NULL_TERM;
  }

  return bvconst_term(&terms, n, bv);
}

/*
 * Same thing from a bvconstant a
 */
EXPORTED term_t yices_bvconstant(bvconstant_t *a) {
  return yices_bvconst_term(a->bitsize, a->data);
}



/*
 * POLYNOMIALS
 */
static void bvarith_set(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;
  int32_t n;

  n = term_bitsize(&terms, t);
  bvarith_buffer_prepare(b, n);

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_add_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_add_const(b, c->bits);
    break;

  default:
    bvarith_buffer_add_var(b, get_bitvector_variable(&terms, t));
    break;
  }
}

static void bvarith_set_neg(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;
  int32_t n;

  n = term_bitsize(&terms, t);
  bvarith_buffer_prepare(b, n);

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_sub_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_sub_const(b, c->bits);
    break;

  default:
    bvarith_buffer_sub_var(b, get_bitvector_variable(&terms, t));
    break;
  }
}

static void bvarith_add(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_add_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_add_const(b, c->bits);
    break;

  default:
    bvarith_buffer_add_var(b, get_bitvector_variable(&terms, t));
    break;
  }
}

static void bvarith_sub(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_sub_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvarith_buffer_sub_const(b, c->bits);
    break;

  default:
    bvarith_buffer_sub_var(b, get_bitvector_variable(&terms, t));
    break;
  }
}

static void bvarith_mul(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_mul_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    if (bvconst_is_zero(c->bits, c->nbits)) {
      bvarith_buffer_prepare(b, b->size); // reset to zero
    } else {
      bvarith_buffer_mul_const(b, c->bits);
    }
    break;

  default:
    bvarith_buffer_mul_var(b, get_bitvector_variable(&terms, t));
    break;
  }
}


static void bvarith_set_square(bvarith_buffer_t *b, term_t t) {
  bvarith_expr_t *p;
  bvconst_term_t *c;
  int32_t n, v;

  n = term_bitsize(&terms, t);
  bvarith_buffer_prepare(b, n);

  switch (term_kind(&terms, t)) {
  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvarith_buffer_add_expr(b, p);
    bvarith_buffer_mul_expr(b, p);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    if (bvconst_is_nonzero(c->bits, c->nbits)) {
      bvarith_buffer_add_const(b, c->bits);
      bvarith_buffer_mul_const(b, c->bits);
    } // else b remains zero
    break;

  default:
    v = get_bitvector_variable(&terms, t);
    bvarith_buffer_add_var(b, v);
    bvarith_buffer_mul_var(b, v);
    break;
  }
}



EXPORTED term_t yices_bvadd(term_t t1, term_t t2) {
  bvarith_buffer_t *b;
  
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvarith_buffer();
  bvarith_set(b, t1);
  bvarith_add(b, t2);

  return yices_bvarith_term(b);
}

EXPORTED term_t yices_bvsub(term_t t1, term_t t2) {
  bvarith_buffer_t *b;
  
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  b = get_internal_bvarith_buffer();
  bvarith_set(b, t1);
  bvarith_sub(b, t2);

  return yices_bvarith_term(b);
}

EXPORTED term_t yices_bvneg(term_t t1) {
  bvarith_buffer_t *b;
  
  if (! check_good_term(&terms, t1) ||
      ! check_bitvector_term(&terms, t1)) {
    return NULL_TERM;
  }

  b = get_internal_bvarith_buffer();
  bvarith_set_neg(b, t1);

  return yices_bvarith_term(b);
}

EXPORTED term_t yices_bvmul(term_t t1, term_t t2) {
  bvarith_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2) || 
      ! check_bvterm_degree(&terms, t1) || 
      ! check_bvterm_degree(&terms, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvarith_buffer();
  bvarith_set(b, t1);
  bvarith_mul(b, t2);

  return yices_bvarith_term(b);
}

EXPORTED term_t yices_bvsquare(term_t t1) {
  bvarith_buffer_t *b;
  
  if (! check_good_term(&terms, t1) ||
      ! check_bitvector_term(&terms, t1) || 
      ! check_bvterm_degree(&terms, t1)) {
    return NULL_TERM;
  }

  b = get_internal_bvarith_buffer();
  bvarith_set_square(b, t1);

  return yices_bvarith_term(b);
}


/*
 * BITWISE LOGICAL OPERATIONS
 */
static void bvlogic_set(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  int32_t n;
  bv_var_t x;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_set_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_set_constant(b, c->nbits, c->bits);
    break;
    
  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_set_bitarray(b, n, bits);
    break;    
  }
}

static void bvlogic_and(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  int32_t n;
  bv_var_t x;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_and_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_and_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_and_bitarray(b, n, bits);
    break;
  }  
}

static void bvlogic_or(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  int32_t n;
  bv_var_t x;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_or_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_or_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_or_bitarray(b, n, bits);
    break;
  }  
}

static void bvlogic_xor(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  int32_t n;
  bv_var_t x;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_xor_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_xor_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_xor_bitarray(b, n, bits);
    break;
  }  
}

// concat: add t to the left of b (high-order bits are from t)
static void bvlogic_concat(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  bv_var_t x;
  int32_t n;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_concat_left_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_concat_left_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_concat_left_bitarray(b, n, bits);
    break;
  }
}

// bitwise comparion + reduction
static void bvlogic_comp(bvlogic_buffer_t *b, term_t t) {
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bit_t *bits;
  int32_t n;
  bv_var_t x;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    assert(e->nbits == b->nbits);
    bvlogic_buffer_comp_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    assert(c->nbits == b->nbits);
    bvlogic_buffer_comp_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    assert(n == b->nbits);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_comp_bitarray(b, n, bits);
    break;
  }
}


EXPORTED term_t yices_bvnot(term_t t1) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t1) ||
      ! check_bitvector_term(&terms, t1)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_buffer_not(b);

  return yices_bvlogic_term(b);
}


EXPORTED term_t yices_bvand(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_and(b, t2);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_bvor(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_or(b, t2);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_bvxor(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;
  
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_xor(b, t2);

  return yices_bvlogic_term(b);
}


EXPORTED term_t yices_bvnand(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_and(b, t2);
  bvlogic_buffer_not(b);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_bvnor(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_or(b, t2);
  bvlogic_buffer_not(b);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_bvxnor(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_xor(b, t2);
  bvlogic_buffer_not(b);

  return yices_bvlogic_term(b);
}




/*
 * SHIFT/ROTATION BY A CONSTANT
 */

/*
 * Shift or rotation by an integer constant n
 * - shift_left0 sets the low-order bits to zero
 * - shift_left1 sets the low-order bits to one
 * - shift_rigth0 sets the high-order bits to zero
 * - shift_right1 sets the high-order bits to one
 * - ashift_right is arithmetic shift, it copies the sign bit &
 * - rotate_left: circular rotation
 * - rotate_right: circular rotation 
 *
 * If t is a vector of m bits, then n must satisfy 0 <= n <= m.
 *
 * The functions return NULL_TERM (-1) if there's an error.
 *
 * Error reports:
 * if t is not valid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if n < 0
 *   code = NONNEG_INT_REQUIRED
 *   badval = n
 * if n > size of t
 *   code = INVALID_BITSHIFT
 *   badval = n
 */
EXPORTED term_t yices_shift_left0(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_shift_left0(b, n);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_shift_left1(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_shift_left1(b, n);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_shift_right0(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_shift_right0(b, n);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_shift_right1(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_shift_right1(b, n);

  return yices_bvlogic_term(b);
}


EXPORTED term_t yices_ashift_right(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_ashift_right(b, n);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_rotate_left(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  if (n < b->nbits) {
    bvlogic_buffer_rotate_left(b, n);
  }

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_rotate_right(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) || 
      ! check_bitshift(n, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }
  
  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  if (n < b->nbits) {
    bvlogic_buffer_rotate_right(b, n);
  }

  return yices_bvlogic_term(b);
}


/*
 * Extract a subvector of t
 * - t must be a bitvector term of size m
 * - i and j must satisfy 0 <= i <= j <= m-1
 * The result is the bits i to j of t.
 *
 * Return NULL_TERM (-1) if there's an error.
 *
 * Error reports:
 * if t is not valid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if 0 <= i <= j <= m-1 does not hold
 *   code = INVALID_BVEXTRACT
 */
EXPORTED term_t yices_bvextract(term_t t, int32_t i, int32_t j) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_bitextract(i, j, term_bitsize(&terms, t))) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_extract_subvector(b, i, j);

  return yices_bvlogic_term(b);
}


/*
 * Concatenation
 * - t1 and t2 must be bitvector terms
 *
 * Return NULL_TERM (-1) if there's an error.
 *
 * Error reports
 * if t1 or t2 is not a valid term
 *   code = INVALID_TERM
 *   term1 = t1 or t2
 * if t1 or t2 is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t1 or t2
 */
EXPORTED term_t yices_bvconcat(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t1) ||
      ! check_good_term(&terms, t2) ||
      ! check_bitvector_term(&terms, t1) ||
      ! check_bitvector_term(&terms, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t2);
  bvlogic_concat(b, t1);

  return yices_bvlogic_term(b);
}


/*
 * Repeated concatenation:
 * - make n copies of t and concatenate them
 * - n must be positive
 *
 * Return NULL_TERM (-1) if there's an error
 *
 * Error report:
 * if t is not valid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if n <= 0
 *   code = POSINT_REQUIRED
 *   badval = n
 */
EXPORTED term_t yices_bvrepeat(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_positive(n)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_repeat_concat(b, n);

  return yices_bvlogic_term(b);
}


/*
 * Sign extension
 * - add n copies of t's sign bit
 * - n must be non-negative
 *
 * Return NULL_TERM if there's an error.
 *
 * Error reports:
 * if t is invalid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if n < 0,
 *   code = NONNEG_INT_REQUIRED
 *   badval = n
 */
EXPORTED term_t yices_sign_extend(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_nonneg(n)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_sign_extend(b, b->nbits + n);

  return yices_bvlogic_term(b);
}


/*
 * Zero extension
 * - add n zeros to t
 * - n must be non-negative
 *
 * Return NULL_TERM if there's an error.
 *
 * Error reports:
 * if t is invalid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 * if n < 0,
 *   code = NONNEG_INT_REQUIRED
 *   badval = n
 */
EXPORTED term_t yices_zero_extend(term_t t, int32_t n) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t) ||
      ! check_nonneg(n)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_zero_extend(b, b->nbits + n);

  return yices_bvlogic_term(b);
}



/*
 * AND-reduction: 
 * if t is b[m-1] ... b[0], then the result is a bit-vector of 1 bit
 * equal to the conjunction of all bits of t (i.e., (and b[0] ... b[m-1])
 *
 * OR-reduction: compute (or b[0] ... b[m-1])
 *
 * Return NULL_TERM if there's an error
 *
 * Error reports:
 * if t is invalid
 *   code = INVALID_TERM
 *   term1 = t
 * if t is not a bitvector
 *   code = BITVECTOR_REQUIRED
 *   term1 = t
 */
EXPORTED term_t yices_redand(term_t t) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_redand(b);

  return yices_bvlogic_term(b);
}

EXPORTED term_t yices_redor(term_t t) {
  bvlogic_buffer_t *b;

  if (! check_good_term(&terms, t) ||
      ! check_bitvector_term(&terms, t)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t);
  bvlogic_buffer_redor(b);

  return yices_bvlogic_term(b);
}


/*
 * Bitwise equality comparison: if t1 and t2 are bitvectors of size n,
 * construct (bvand (bvxnor t1 t2))
 *
 * Return NULL_TERM if there's an error
 *
 * Error reports:
 * if t1 or t2 is not valid
 *   code = INVALID_TERM
 *   term1 = t1 or t2
 *   index = -1
 * if t1 or t2 is not a bitvector term
 *   code = BITVECTOR_REQUIRED
 *   term1 = t1 or t2
 * if t1 and t2 do not have the same bitvector type
 *   code = INCOMPATIBLE_TYPES
 *   term1 = t1
 *   type1 = type of t1
 *   term2 = t2
 *   type2 = type of t2
 */
EXPORTED term_t yices_redcomp(term_t t1, term_t t2) {
  bvlogic_buffer_t *b;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  b = get_internal_bvlogic_buffer();
  bvlogic_set(b, t1);
  bvlogic_comp(b, t2);

  return yices_bvlogic_term(b);
}






/*
 * VARIABLE SHIFTS
 */

/*
 * All shift operators takes two bit-vector arguments of the same size.
 * The first argument is shifted. The second argument is the shift amount.
 * - bvshl t1 t2: shift left, padding with 0
 * - bvlshr t1 t2: logical shift right (padding with 0)
 * - bvashr t1 t2: arithmetic shift right (copy the sign bit)
 */


/*
 * Convert bv's value (as a non-negative integer) into a shift amount. 
 * If bv's value is larger than n, then returns n
 */
static uint32_t get_shift_amount(uint32_t n, uint32_t *bv) {
  uint32_t k, i, s;

  k = (n + 31) >> 5; // number of words in bv
  s = bvconst_get32(bv); // low-order word = shift amount

  // if any of the higher order words in nonzero, return n
  for (i=1; i<k; i++) {
    if (bv[i] != 0) return n;
  }

  // truncate s if required
  return (n <= s) ? n : s;
}

/*
 * Copy t into the internal_bvlogic_buffer
 */
static void copy_term_to_internal_bvlogic_buffer(term_t t) {
  bvlogic_buffer_t *b;
  bvlogic_expr_t *e;
  bvconst_term_t *c;
  bv_var_t x;
  bit_t *bits;
  int32_t n;

  b = get_internal_bvlogic_buffer();

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bvlogic_buffer_set_bitarray(b, e->nbits, e->bit);
    break;

  case BV_CONST_TERM:
    c = bvconst_term_desc(&terms, t);
    bvlogic_buffer_set_constant(b, c->nbits, c->bits);
    break;

  default:
    n = term_bitsize(&terms, t);
    x = get_bitvector_variable(&terms, t);
    bits = bv_var_manager_get_bit_array(&bv_manager, x);
    bvlogic_buffer_set_bitarray(b, n, bits);
    break;
  }
  
}

EXPORTED term_t yices_bvshl(term_t t1, term_t t2) {
  bvlogic_buffer_t *log_buffer;  
  bvconst_term_t *c;
  uint32_t s;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (term_kind(&terms, t2) == BV_CONST_TERM) {
    // shift left by a constant/pagging with 0
    copy_term_to_internal_bvlogic_buffer(t1);
    c = bvconst_term_desc(&terms, t2);
    s = get_shift_amount(c->nbits, c->bits);
    log_buffer = internal_bvlogic_buffer;
    bvlogic_buffer_shift_left0(log_buffer, s);
    return yices_bvlogic_term(log_buffer);
  }

  return bvapply_term(&terms, BVOP_SHL, t1, t2);
}


EXPORTED term_t yices_bvlshr(term_t t1, term_t t2) {
  bvlogic_buffer_t *log_buffer;  
  bvconst_term_t *c;
  uint32_t s;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (term_kind(&terms, t2) == BV_CONST_TERM) {
    // shift right by a constant/padding with 0
    copy_term_to_internal_bvlogic_buffer(t1);
    c = bvconst_term_desc(&terms, t2);
    s = get_shift_amount(c->nbits, c->bits);
    log_buffer = internal_bvlogic_buffer;
    bvlogic_buffer_shift_right0(log_buffer, s);
    return yices_bvlogic_term(log_buffer);
  }

  return bvapply_term(&terms, BVOP_LSHR, t1, t2);
}


EXPORTED term_t yices_bvashr(term_t t1, term_t t2) {
  bvlogic_buffer_t *log_buffer;  
  bvconst_term_t *c;
  uint32_t s;

  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (term_kind(&terms, t2) == BV_CONST_TERM) {
    // arithmetic shift by a constant
    copy_term_to_internal_bvlogic_buffer(t1);
    c = bvconst_term_desc(&terms, t2);
    s = get_shift_amount(c->nbits, c->bits);
    log_buffer = internal_bvlogic_buffer;
    bvlogic_buffer_ashift_right(log_buffer, s);
    return yices_bvlogic_term(log_buffer);
  }

  return bvapply_term(&terms, BVOP_ASHR, t1, t2);
}







/*
 * BITVECTOR DIVISION OPERATORS
 */

/*
 * These are all new SMTLIB division and remainder operators.
 * All are binary operators with two bitvector arguments of the same size.
 * - bvdiv: quotient in unsigned division
 * - bvrem: remainder in unsigned division
 * - bvsdiv: quotient in signed division (rounding toward 0)
 * - bvsrem: remainder in signed division
 * - bvsmod: remainder in floor division (signed division, rounding toward -infinity)
 */
term_t yices_bvoperator(uint32_t op, term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return bvapply_term(&terms, op, t1, t2);
}

EXPORTED term_t yices_bvdiv(term_t t1, term_t t2) {
  return yices_bvoperator(BVOP_DIV, t1, t2);
}

EXPORTED term_t yices_bvrem(term_t t1, term_t t2) {
  return yices_bvoperator(BVOP_REM, t1, t2);
}

EXPORTED term_t yices_bvsdiv(term_t t1, term_t t2) {
  return yices_bvoperator(BVOP_SDIV, t1, t2);
}

EXPORTED term_t yices_bvsrem(term_t t1, term_t t2) {
  return yices_bvoperator(BVOP_SREM, t1, t2);
}

EXPORTED term_t yices_bvsmod(term_t t1, term_t t2) {
  return yices_bvoperator(BVOP_SMOD, t1, t2);
}






/*********************
 *  BITVECTOR ATOMS  *
 ********************/

/*
 * Equality
 */
static term_t mk_bveq(term_t t1, term_t t2) {
  term_t aux;

  if (t1 == t2) return true_term(&terms);
  if (disequal_bitvector_terms(&terms, t1, t2)) {
    return false_term(&terms);
  }

  // put smaller index on the left
  if (t1 > t2) {
    aux = t1; t1 = t2; t2 = aux;
  }

  return bveq_atom(&terms, t1, t2);
}

EXPORTED term_t yices_bveq_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_bveq(t1, t2);
}


/*
 * Disequality
 */
static term_t mk_bvneq(term_t t1, term_t t2) {
  term_t aux;

  if (t1 == t2) return false_term(&terms);
  if (disequal_bitvector_terms(&terms, t1, t2)) {
    return true_term(&terms);
  }

  // put smaller index on the left
  if (t1 > t2) {
    aux = t1; t1 = t2; t2 = aux;
  }

  return not_term(&terms, bveq_atom(&terms, t1, t2));
}

EXPORTED term_t yices_bvneq_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  return mk_bvneq(t1, t2);
}


/*
 * Upper/lower bound of a bitvector term t (unsigned)
 */
static void upper_bound_unsigned(term_t t, bvconstant_t *c) {
  bvlogic_expr_t *e;
  bvarith_expr_t *p;
  bvconst_term_t *a;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bitarray_upper_bound_unsigned(e->nbits, e->bit, c);
    break;

  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvconstant_set_all_one(c, p->size);
    break;

  case BV_CONST_TERM:
    a = bvconst_term_desc(&terms, t);
    bvconstant_copy(c, a->nbits, a->bits);
    break;

  default:
    assert(is_bitvector_term(&terms, t));
    bvconstant_set_all_one(c, term_bitsize(&terms, t));
    break;
  }
}

static void lower_bound_unsigned(term_t t, bvconstant_t *c) {
  bvlogic_expr_t *e;
  bvarith_expr_t *p;
  bvconst_term_t *a;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bitarray_lower_bound_unsigned(e->nbits, e->bit, c);
    break;

  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvconstant_set_all_zero(c, p->size);
    break;

  case BV_CONST_TERM:
    a = bvconst_term_desc(&terms, t);
    bvconstant_copy(c, a->nbits, a->bits);
    break;

  default:
    assert(is_bitvector_term(&terms, t));
    bvconstant_set_all_zero(c, term_bitsize(&terms, t));
    break;
  }
}


/*
 * Check whether t1 < t2 must hold
 */
static bool must_lt(term_t t1, term_t t2) {
  upper_bound_unsigned(t1, &bv1); // t1 <= bv1
  lower_bound_unsigned(t2, &bv2); // bv2 <= t2
  assert(bv1.bitsize == bv2.bitsize);

  return bvconst_lt(bv1.data, bv2.data, bv1.bitsize);
}


static bool must_le(term_t t1, term_t t2) {
  upper_bound_unsigned(t1, &bv1);
  lower_bound_unsigned(t2, &bv2);
  assert(bv1.bitsize == bv2.bitsize);

  return bvconst_le(bv1.data, bv2.data, bv1.bitsize);
}


/*
 * Unsigned inequalities
 */
 // t1 >= t2
EXPORTED term_t yices_bvge_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }
  
  if (t1 == t2 || must_le(t2, t1)) {
    return true_term(&terms);
  }

  if (must_lt(t1, t2)) {
    return false_term(&terms);
  }
  
  return bvge_atom(&terms, t1, t2);
}

// t1 > t2
EXPORTED term_t yices_bvgt_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (t1 == t2 || must_le(t1, t2)) {
    return false_term(&terms);
  }

  if (must_lt(t2, t1)) {
    return true_term(&terms);
  }
  
  return not_term(&terms, bvge_atom(&terms, t2, t1));
}


// t1 <= t2
EXPORTED term_t yices_bvle_atom(term_t t1, term_t t2) {
  return yices_bvge_atom(t2, t1);
}

// t1 < t2
EXPORTED term_t yices_bvlt_atom(term_t t1, term_t t2) {
  return yices_bvgt_atom(t2, t1);
}




/*
 * Upper/lower bounds on t, interpreted as a signed integer
 */
static void upper_bound_signed(term_t t, bvconstant_t *c) {
  bvlogic_expr_t *e;
  bvarith_expr_t *p;
  bvconst_term_t *a;
  int32_t n;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bitarray_upper_bound_signed(e->nbits, e->bit, c);
    break;

  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvconstant_set_all_one(c, p->size);
    bvconst_clr_bit(c->data, p->size - 1); // set sign bit to 0
    break;

  case BV_CONST_TERM:
    a = bvconst_term_desc(&terms, t);
    bvconstant_copy(c, a->nbits, a->bits);
    break;

  default:
    assert(is_bitvector_term(&terms, t));
    n = term_bitsize(&terms, t);
    bvconstant_set_all_one(c, n);
    bvconst_clr_bit(c->data, n - 1); // set sign bit to 0    
    break;
  }
}

static void lower_bound_signed(term_t t, bvconstant_t *c) {
  bvlogic_expr_t *e;
  bvarith_expr_t *p;
  bvconst_term_t *a;
  int32_t n;

  switch (term_kind(&terms, t)) {
  case BV_LOGIC_TERM:
    e = bvlogic_term_desc(&terms, t);
    bitarray_lower_bound_signed(e->nbits, e->bit, c);
    break;

  case BV_ARITH_TERM:
    p = bvarith_term_desc(&terms, t);
    bvconstant_set_all_zero(c, p->size);
    bvconst_set_bit(c->data, p->size - 1); // set sign bit to 1
    break;

  case BV_CONST_TERM:
    a = bvconst_term_desc(&terms, t);
    bvconstant_copy(c, a->nbits, a->bits);
    break;

  default:
    assert(is_bitvector_term(&terms, t));
    n = term_bitsize(&terms, t);
    bvconstant_set_all_zero(c, n);
    bvconst_set_bit(c->data, n - 1); // set sign bit to 1
    break;
  }
}

/*
 * Check whether t1 < t2 must hold (signed comparison)
 */
static bool must_slt(term_t t1, term_t t2) {
  upper_bound_signed(t1, &bv1);
  lower_bound_signed(t2, &bv2);
  assert(bv1.bitsize == bv2.bitsize);

  return bvconst_slt(bv1.data, bv2.data, bv1.bitsize);
}


/*
 * Check whether t1 <= t2 must hold (signed comparison)
 */
static bool must_sle(term_t t1, term_t t2) {
  upper_bound_signed(t1, &bv1);
  lower_bound_signed(t2, &bv2);
  assert(bv1.bitsize == bv2.bitsize);

  return bvconst_sle(bv1.data, bv2.data, bv1.bitsize);
}




/*
 * Signed bitvector inequalities 
 */
// t1 >= t2
EXPORTED term_t yices_bvsge_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (t1 == t2 || must_sle(t2, t1)) {
    return true_term(&terms);
  }

  if (must_slt(t1, t2)) {
    return false_term(&terms);
  }
  
  return bvsge_atom(&terms, t1, t2);
}

// t1 > t2
EXPORTED term_t yices_bvsgt_atom(term_t t1, term_t t2) {
  if (! check_compatible_bv_terms(&terms, t1, t2)) {
    return NULL_TERM;
  }

  if (t1 == t2 || must_sle(t1, t2)) {
    return false_term(&terms);
  }

  if (must_slt(t2, t1)) {
    return true_term(&terms);
  }
  
  return not_term(&terms, bvsge_atom(&terms, t2, t1));
}

// t1 <= t2
EXPORTED term_t yices_bvsle_atom(term_t t1, term_t t2) {
  return yices_bvsge_atom(t2, t1);
}

// t1 < t2
EXPORTED term_t yices_bvslt_atom(term_t t1, term_t t2) {
  return yices_bvsgt_atom(t2, t1);
}








/***************************************
 * EXTENSIONS FOR BITVECTOR CONSTANTS  *
 **************************************/

/*
 * These functions are similar to the bvarith_xxx_const and bvlogic_xxx_const 
 * but the  bitvector constant is represented directly as a byte array.
 * For all operations, 
 * - n = bitsize of the constant,
 * - bv = byte array storing the constant. bv must be normalized.
 *
 * Error codes are set as in the other bitvector functions.  Only the
 * functions that may generate error codes are implemented here.  For
 * other functions, direct call of the bvarith_expr and bvlogic_expr
 * functions can be used.
 */
int32_t yices_bvarith_add_bvconst(bvarith_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->size != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvarith_buffer_add_const(b, bv);
  return 0;
}

int32_t yices_bvarith_sub_bvconst(bvarith_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->size != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvarith_buffer_sub_const(b, bv);
  return 0;
}

int32_t yices_bvarith_mul_bvconst(bvarith_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->size != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  if (bvconst_is_zero(bv, b->width)) {
    // reset to zero
    bvarith_buffer_prepare(b, n);
  } else {
    bvarith_buffer_mul_const(b, bv);
  }

  return 0;
}

int32_t yices_bvlogic_and_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->nbits != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvlogic_buffer_and_constant(b, n, bv);
  return 0;
}

int32_t yices_bvlogic_or_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->nbits != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvlogic_buffer_or_constant(b, n, bv);
  return 0;
}

int32_t yices_bvlogic_xor_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->nbits != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvlogic_buffer_xor_constant(b, n, bv);
  return 0;
}

// more 
int32_t yices_bvlogic_nand_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  int32_t code;

  code = yices_bvlogic_and_bvconst(b, n, bv);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

int32_t yices_bvlogic_nor_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  int32_t code;

  code = yices_bvlogic_or_bvconst(b, n, bv);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}

int32_t yices_bvlogic_xnor_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  int32_t code;

  code = yices_bvlogic_xor_bvconst(b, n, bv);
  if (code == 0) bvlogic_buffer_not(b);
  return code;
}


// Bitwise comparison for bitvector constant
int32_t yices_bvlogic_comp_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  if (b->size != n) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }

  bvlogic_buffer_comp_constant(b, n, bv);
  return 0;
}




/*
 * Support for SMT-LIB shift operators:
 *  (shl <bv1> <bv2>) 
 *  (lshr <bv1> <bv2>)
 *  (ashr <bv1> <bv2>)
 *
 * This deals with the common case where <bv2> is a constant.
 */
int32_t yices_bvlogic_shl_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  uint32_t s;

  if (n != b->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }
  
  s = get_shift_amount(n, bv);
  bvlogic_buffer_shift_left0(b, s);

  return 0;
}

int32_t yices_bvlogic_lshr_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  uint32_t s;

  if (n != b->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }
  
  s = get_shift_amount(n, bv);
  bvlogic_buffer_shift_right0(b, s);

  return 0;
}

int32_t yices_bvlogic_ashr_bvconst(bvlogic_buffer_t *b, uint32_t n, uint32_t *bv) {
  uint32_t s;

  if (n != b->nbits) {
    error.code = INCOMPATIBLE_BVSIZES;
    error.badval = n;
    return -1;
  }
  
  s = get_shift_amount(n, bv);
  bvlogic_buffer_ashift_right(b, s);

  return 0;
}

#endif


/**************************
 *  SOME CHECKS ON TERMS  *
 *************************/

/*
 * Get the type of term t
 * return NULL_TYPE if t is not a valid term
 * and set the error report:
 *   code = INVALID_TERM
 *   term1 = t
 *   index = -1
 */
type_t yices_type_of_term(term_t t) {
  if (! check_good_term(&terms, t)) {
    return NULL_TYPE;
  }
  return term_type(&terms, t);
}


/*
 * Check the type of a term t:
 * - term_is_arithmetic check whether t's type is either int or real
 * - term_is_real check whether t's type is real (return false if t's type is int)
 * - term_is_int check whether t's type is int 
 * If t is not a valid term, the check functions return false
 * and set the error report as above.
 */
bool yices_term_is_bool(term_t t) {
  return check_good_term(&terms, t) && is_boolean_term(&terms, t);
}

bool yices_term_is_int(term_t t) {
  return check_good_term(&terms, t) && is_integer_term(&terms, t);
}

bool yices_term_is_real(term_t t) {
  return check_good_term(&terms, t) && is_real_term(&terms, t);
}

bool yices_term_is_arithmetic(term_t t) {
  return check_good_term(&terms, t) && is_arithmetic_term(&terms, t);
}

bool yices_term_is_bitvector(term_t t) {
  return check_good_term(&terms, t) && is_bitvector_term(&terms, t);
}

bool yices_term_is_tuple(term_t t) {
  return check_good_term(&terms, t) && is_tuple_term(&terms, t);
}

bool yices_term_is_function(term_t t) {
  return check_good_term(&terms, t) && is_function_term(&terms, t);
}



/*
 * Size of bitvector term t. 
 * return -1 if t is not a bitvector
 */
uint32_t yices_term_bitsize(term_t t) {
  if (! check_bitvector_term(&terms, t)) {
    return 0;
  }
  return term_bitsize(&terms, t);
}





/************
 *  NAMES   *
 ***********/

/*
 * Create mapping (name -> tau) in the type table.
 * If a previous mapping (name -> tau') is in the table, then 
 * it is hidden. 
 *
 * return -1 if tau is invalid and set error report
 * return 0 otherwise.
 */
EXPORTED int32_t yices_set_type_name(type_t tau, char *name) {
  char *clone;

  if (! check_good_type(&types, tau)) {
    return -1;
  }

  // make a copy of name
  clone = clone_string(name);
  set_type_name(&types, tau, clone);

  return 0;
}


/*
 * Create mapping (name -> t) in the term table.
 * If a previous mapping (name -> t') is in the table, then 
 * it is hidden. 
 *
 * return -1 if  is invalid and set error report
 * return 0 otherwise.
 */
EXPORTED int32_t yices_set_term_name(term_t t, char *name) {
  char *clone;

  if (! check_good_term(&terms, t)) {
    return -1;
  }

  // make a copy of name
  clone = clone_string(name);
  set_term_name(&terms, t, clone);

  return 0;
}


/*
 * Remove name from the type table.
 */
EXPORTED void yices_remove_type_name(char *name) {
  remove_type_name(&types, name);
}


/*
 * Remove name from the term table.
 */
EXPORTED void yices_remove_term_name(char *name) {
  remove_term_name(&terms, name);
}


/*
 * Get type of the given name or return NULL_TYPE (-1)
 */
EXPORTED type_t yices_get_type_by_name(char *name) {
  return get_type_by_name(&types, name);
}


/*
 * Get term of the given name or return NULL_TERM
 */
EXPORTED term_t yices_get_term_by_name(char *name) {
  return get_term_by_name(&terms, name);
}


