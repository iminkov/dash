/**
 * \file dart_pmem.c
 *
 * Implementation of dart persistent memory
 */
#include <stdio.h>
#include <string.h>
#include <linux/limits.h>
#include <sys/stat.h>

#include <dash/dart/if/dart_team_group.h>
#include <dash/dart/if/dart_pmem.h>

#include <dash/dart/base/logging.h>
#include <dash/dart/base/assert.h>



int _dart_pmem_list_new(PMEMobjpool * pop, void * ptr, void * arg)

{
  int ret = 0;
  TOID(struct dart_pmem_bucket_list) * list = ptr;
  struct dart_pmem_slist_constr_args * args = arg;

  TX_BEGIN(pop) {
    if (TOID_IS_NULL(*list)) {
      abort();
    }

    DART_PMEM_SLIST_INIT(&D_RW(*list)->head);
    TX_MEMCPY(D_RW(*list)->name, args->name, strlen(args->name) + 1);
  }
  TX_ONABORT {
    DART_LOG_ERROR("%s: transaction aborted: %s\n", __func__, pmemobj_errormsg());
    ret = -1;

  } TX_END

  return ret;
}


static char * _tempname(const char * layout, int myid)
{
  char suffix[3];
  snprintf(suffix, sizeof(suffix), ".%d", myid);

  char * prefix = malloc(strlen(layout) + sizeof(suffix));
  DART_ASSERT(prefix);

  strcpy(prefix, layout);
  strcat(prefix, suffix);

  //char const * dir = '\0';

  //char * ret = tempnam(dir, prefix);
  //free(prefix);
  //return ret;
  return prefix;
}

#define DART_PMEM_ALL_FLAGS\
  (DART_PMEM_FILE_CREATE)

dart_ret_t dart__pmem__open(
  dart_team_t   team,
  const char  * name,
  int           flags,
  mode_t        mode,
  dart_pmem_pool_t * poolp)
{
  DART_ASSERT(poolp);
  DART_ASSERT(name);

  if (flags & ~(DART_PMEM_ALL_FLAGS)) {
    DART_LOG_ERROR("invalid flag specified: %d", flags);
    return DART_ERR_INVAL;
  }

  if (DART_TEAM_NULL == team) {
    DART_LOG_ERROR("invalid team specified: %d", team);
    return DART_ERR_INVAL;
  }

  if (strlen(name) >= DART_NVM_POOL_NAME ) {
    DART_LOG_ERROR("invalid pool name: %s", name);
    return DART_ERR_INVAL;
  }

  int myid;
  DART_ASSERT_RETURNS(dart_team_myid(team, &myid), DART_OK);

  PMEMobjpool * pop;

  char * full_path = _tempname(name, myid);

  if ((flags & DART_PMEM_FILE_CREATE) && access(full_path, F_OK) != 0) {

    if ((pop = pmemobj_create(full_path, name, DART_PMEM_MIN_POOL,
                              mode)) == NULL) {
      DART_LOG_ERROR("failed to create pmem pool: %s", name);
      return DART_ERR_INVAL;
    }

    struct dart_pmem_slist_constr_args args = {
      .name = name
    };

    PMEMoid root = pmemobj_root_construct(pop,
                                          sizeof(struct dart_pmem_bucket_list),
                                          _dart_pmem_list_new, &args);

    DART_ASSERT(!(OID_IS_NULL(root)));
  } else {
    if ((pop = pmemobj_open(full_path, name)) == NULL) {
      DART_LOG_ERROR("failed to open pmem pool: %s", name);
      return DART_ERR_INVAL;
    }

    DART_ASSERT(pmemobj_root_size(pop));
  }

  //TODO: record actual size
  poolp->size = 0;
  poolp->path = full_path;
  poolp->layout = strdup(name);
  poolp->pop = pop;
  poolp->teamid = team;

  return DART_OK;
}

char * dart_pmem_bucket_alloc(PMEMobjpool * pop,
                              TOID(struct dart_pmem_bucket_list) list,
                              struct dart_pmem_bucket_alloc_args args)
{
  if (TOID_IS_NULL(list)) {
    return NULL;
  }

  char * ret = NULL;

  struct dart_pmem_list_head * head = &D_RW(list)->head;

  TOID(struct dart_pmem_bucket) node;
  TX_BEGIN(pop) {
    node = TX_NEW(struct dart_pmem_bucket);
    if (TOID_IS_NULL(node)) {
      abort();
    }
    D_RW(node)->element_size = args.element_size;
    D_RW(node)->length = args.nelements;
    D_RW(node)->data = pmemobj_tx_zalloc(args.element_size * args.nelements, TYPE_NUM_BYTE);
    if (OID_IS_NULL(D_RO(node)->data)) {
      abort();
    }
    DART_PMEM_SLIST_INSERT_HEAD(head, node, next);
  }
  TX_ONCOMMIT {
    ret = pmemobj_direct(D_RW(node)->data);
  }
  TX_ONABORT {
    fprintf(stderr, "%s: transaction aborted: %s\n", __func__, pmemobj_errormsg());
    ret = NULL;
  } TX_END

  return ret;
}

dart_ret_t  dart__pmem__alloc(
  dart_team_t         teamid,
  dart_pmem_pool_t    pool,
  size_t              nbytes,
  dart_gptr_t    *    gptr)
{
  if (NULL == pool.pop) {
    DART_LOG_ERROR("invalid pmem pool");
    return DART_ERR_INVAL;
  }

  if (teamid != pool.teamid) {
    DART_LOG_ERROR("invalid teamid for pool %s", pool.layout);
    return DART_ERR_INVAL;
  }

  size_t root_size = pmemobj_root_size(pool.pop);

  if (root_size < sizeof(struct dart_pmem_bucket_list)) {
    //TODO: apply better consistency check
    DART_LOG_ERROR("improperly initialized pool");
    return DART_ERR_INVAL;
  }

  TOID(struct dart_pmem_bucket_list) list = POBJ_ROOT(pool.pop, struct dart_pmem_bucket_list);

  struct dart_pmem_bucket_alloc_args args = {
    .element_size = sizeof(char),
    .nelements = nbytes,
  };


  char * mem = dart_pmem_bucket_alloc(pool.pop, list, args);

  if (NULL == mem) {
    DART_LOG_ERROR("could not allocate persistent memory");
    return DART_ERR_OTHER;
  }

  return dart_team_memregister(teamid, nbytes, mem, gptr);
}

dart_ret_t dart__pmem__close(
  dart_pmem_pool_t * pool)
{
  pmemobj_close(pool->pop);
  free((char *) pool->path);
  free((char *) pool->layout);

  return DART_OK;
}