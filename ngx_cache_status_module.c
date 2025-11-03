/*
 * Copyright (C) Viktor Suprun 2016-2025
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_atomic.h"
#include "ngx_string.h"

#define TOTAL_REQUESTS_SLOT 0
#define UNCACHED_REQUESTS_SLOT 1
#define NGX_HTTP_CACHE_MISS_SLOT 2
#define NGX_HTTP_CACHE_BYPASS_SLOT 3
#define NGX_HTTP_CACHE_EXPIRED_SLOT 4
#define NGX_HTTP_CACHE_STALE_SLOT 5
#define NGX_HTTP_CACHE_UPDATING_SLOT 6
#define NGX_HTTP_CACHE_REVALIDATED_SLOT 7
#define NGX_HTTP_CACHE_HIT_SLOT 8
#define MISC_SLOT 9

ngx_str_t shm_name = ngx_string("cache_status");

typedef struct {
    ngx_atomic_t total;
    ngx_atomic_t uncached;
    ngx_atomic_t miss;
    ngx_atomic_t bypass;
    ngx_atomic_t expired;
    ngx_atomic_t stale;
    ngx_atomic_t updating;
    ngx_atomic_t revalidated;
    ngx_atomic_t hit;
    ngx_atomic_t misc;
} ngx_cache_status_counters;

typedef struct {
    ngx_atomic_uint_t data;
    ngx_shm_zone_t   *shm;
} ngx_cache_status_conf_t;

static ngx_http_output_header_filter_pt ngx_original_filter_ptr;

static char     *ngx_cache_status_default(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_cache_status_filter(ngx_http_request_t *r);
static ngx_int_t ngx_cache_status_filter_init(ngx_conf_t *cf);
static void     *ngx_cache_status_create_conf(ngx_conf_t *cf);

static ngx_command_t ngx_status_commands[] = {

    { ngx_string("cache_status"),
      NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_NOARGS | NGX_CONF_TAKE1,
      ngx_cache_status_default,
      0,
      0,
      NULL },

    ngx_null_command
};

static ngx_http_module_t ngx_cache_status_module_ctx = {
    NULL,                         /* preconfiguration */
    ngx_cache_status_filter_init, /* postconfiguration */

    ngx_cache_status_create_conf, /* create main configuration */
    NULL,                         /* init main configuration */

    NULL, /* create server configuration */
    NULL, /* merge server configuration */

    NULL, /* create location configuration */
    NULL  /* merge location configuration */
};

ngx_module_t ngx_cache_status_module = {

    NGX_MODULE_V1,
    &ngx_cache_status_module_ctx, /* module context */
    ngx_status_commands,          /* module directives */
    NGX_HTTP_MODULE,              /* module type */
    NULL,                         /* init master */
    NULL,                         /* init module */
    NULL,                         /* init process */
    NULL,                         /* init thread */
    NULL,                         /* exit thread */
    NULL,                         /* exit process */
    NULL,                         /* exit master */
    NGX_MODULE_V1_PADDING
};

static ngx_int_t ngx_status_handler(ngx_http_request_t *r)
{
    ngx_int_t   rc;
    ngx_buf_t  *b;
    ngx_chain_t out;

    ngx_cache_status_conf_t   *conf;
    ngx_cache_status_counters *cnt;

    if (r->method != NGX_HTTP_GET && r->method != NGX_HTTP_HEAD) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    rc = ngx_http_discard_request_body(r);

    if (rc != NGX_OK) {
        return rc;
    }

    r->headers_out.content_type_len = sizeof("text/plain") - 1;
    ngx_str_set(&r->headers_out.content_type, "text/plain");
    r->headers_out.content_type_lowcase = NULL;

    if (r->method == NGX_HTTP_HEAD) {
        r->headers_out.status = NGX_HTTP_OK;

        rc = ngx_http_send_header(r);

        if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
            return rc;
        }
    }

    conf = ngx_http_get_module_main_conf(r, ngx_cache_status_module);
    if (conf == NULL || conf->shm == NULL || conf->shm->data == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cnt = conf->shm->data;

    b = ngx_create_temp_buf(r->pool, ngx_pagesize);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    out.buf  = b;
    out.next = NULL;

    b->last = ngx_cpymem(b->last, "Cache statistics:\n", sizeof("Cache statistics:\n") - 1);
    b->last = ngx_sprintf(b->last, "Requests: %uA\n", cnt->total);
    b->last = ngx_sprintf(b->last, "Uncached: %uA\n", cnt->uncached);
    b->last = ngx_sprintf(b->last, "Miss: %uA\n", cnt->miss);
    b->last = ngx_sprintf(b->last, "Bypass: %uA\n", cnt->bypass);
    b->last = ngx_sprintf(b->last, "Expired: %uA\n", cnt->expired);
    b->last = ngx_sprintf(b->last, "Stale: %uA\n", cnt->stale);
    b->last = ngx_sprintf(b->last, "Updating: %uA\n", cnt->updating);
    b->last = ngx_sprintf(b->last, "Revalidated: %uA\n", cnt->revalidated);
    b->last = ngx_sprintf(b->last, "Hit: %uA\n", cnt->hit);
    b->last = ngx_sprintf(b->last, "Misc: %uA", cnt->misc);

    r->headers_out.status           = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf      = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static ngx_int_t ngx_status_prom_handler(ngx_http_request_t *r)
{
    ngx_chain_t                out;
    ngx_buf_t                 *b;
    ngx_int_t                  rc;
    ngx_cache_status_conf_t   *conf;
    ngx_cache_status_counters *cnt;

    conf = ngx_http_get_module_main_conf(r, ngx_cache_status_module);
    if (conf == NULL || conf->shm == NULL || conf->shm->data == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    cnt = conf->shm->data;

    
    ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "cache_status: ngx_status_prom_handler %p", cnt);

    r->headers_out.content_type_len  = sizeof("text/plain") - 1;
    r->headers_out.content_type.len  = sizeof("text/plain") - 1;
    r->headers_out.content_type.data = (u_char *) "text/plain";

    b = ngx_create_temp_buf(r->pool, ngx_pagesize);

    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }
    out.buf  = b;
    out.next = NULL;

    b->last = ngx_sprintf(b->last, "# HELP nginx_cache_status nginx cache status\n");
    b->last = ngx_sprintf(b->last, "# TYPE nginx_cache_status counter\n");
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"total\"} %d\n", cnt->total);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"uncached\"} %d\n", cnt->uncached);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"miss\"} %d\n", cnt->miss);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"bypass\"} %d\n", cnt->bypass);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"expired\"} %d\n", cnt->expired);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"stale\"} %d\n", cnt->stale);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"updating\"} %d\n", cnt->updating);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"revalidated\"} %d\n", cnt->revalidated);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"hit\"} %d\n", cnt->hit);
    b->last = ngx_sprintf(b->last, "nginx_cache_status{status=\"misc\"} %d\n", cnt->misc);

    r->headers_out.status           = NGX_HTTP_OK;
    r->headers_out.content_length_n = b->last - b->pos;

    b->last_buf      = (r == r->main) ? 1 : 0;
    b->last_in_chain = 1;

    rc = ngx_http_send_header(r);

    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    return ngx_http_output_filter(r, &out);
}

static char *ngx_cache_status_default(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_core_loc_conf_t *clcf;
    ngx_str_t                *value;
    value = cf->args->elts;
    clcf  = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    if (ngx_strncmp(value[1].data, "prom", 4) == 0) {
        clcf->handler = ngx_status_prom_handler;
    } else {
        clcf->handler = ngx_status_handler;
    }

    return NGX_CONF_OK;
}

static ngx_int_t ngx_cache_status_init_shm_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    if (data) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shm_zone->data = ngx_slab_alloc((ngx_slab_pool_t *) (shm_zone->shm.addr), sizeof(ngx_cache_status_counters));
    if (shm_zone->data == NULL) {
        ngx_log_error(NGX_LOG_ALERT, ngx_cycle->log, 0, "cache_status: slab alloc failed");
        return NGX_ERROR;
    }

    return NGX_OK;
}

static void *ngx_cache_status_create_conf(ngx_conf_t *cf)
{
    ngx_shm_zone_t          *shm_zone;
    ngx_cache_status_conf_t *conf;
    shm_zone = ngx_shared_memory_add(cf, &shm_name, 2 * ngx_pagesize, &ngx_cache_status_module);
    if (shm_zone == NULL) {
        ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, "cache_status: shm alloc failed");
        return NULL;
    }
    conf     = ngx_palloc(cf->pool, sizeof(ngx_cache_status_conf_t));
    if (conf == NULL) {
        ngx_conf_log_error(NGX_LOG_ALERT, cf, 0, "cache_status: mem alloc failed");
        return NULL;
    }
    conf->shm       = shm_zone;
    conf->shm->init = ngx_cache_status_init_shm_zone;
    return conf;
}

static ngx_int_t ngx_cache_status_filter_init(ngx_conf_t *cf)
{
    ngx_original_filter_ptr    = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_cache_status_filter;

    return NGX_OK;
}

void ngx_cache_status_atomic_inc(ngx_http_request_t *r, int type)
{
    ngx_cache_status_conf_t   *conf;
    ngx_cache_status_counters *cnt;
    conf = ngx_http_get_module_main_conf(r, ngx_cache_status_module);
    if (conf == NULL || conf->shm == NULL || conf->shm->data == NULL) {
        return;
    }
    cnt = conf->shm->data;
    switch (type) {
        case TOTAL_REQUESTS_SLOT:
            ngx_atomic_fetch_add(&cnt->total, 1);
            break;
        case UNCACHED_REQUESTS_SLOT:
            ngx_atomic_fetch_add(&cnt->uncached, 1);
            break;
        case NGX_HTTP_CACHE_MISS_SLOT:
            ngx_atomic_fetch_add(&cnt->miss, 1);
            break;
        case NGX_HTTP_CACHE_BYPASS_SLOT:
            ngx_atomic_fetch_add(&cnt->bypass, 1);
            break;
        case NGX_HTTP_CACHE_EXPIRED_SLOT:
            ngx_atomic_fetch_add(&cnt->expired, 1);
            break;
        case NGX_HTTP_CACHE_STALE_SLOT:
            ngx_atomic_fetch_add(&cnt->stale, 1);
            break;
        case NGX_HTTP_CACHE_UPDATING_SLOT:
            ngx_atomic_fetch_add(&cnt->updating, 1);
            break;
        case NGX_HTTP_CACHE_REVALIDATED_SLOT:
            ngx_atomic_fetch_add(&cnt->revalidated, 1);
            break;
        case NGX_HTTP_CACHE_HIT_SLOT:
            ngx_atomic_fetch_add(&cnt->hit, 1);
            break;
        case MISC_SLOT:
            ngx_atomic_fetch_add(&cnt->misc, 1);
            break;
        default:
            break;
    }
}

static ngx_int_t ngx_cache_status_filter(ngx_http_request_t *r)
{
    ngx_cache_status_atomic_inc(r, TOTAL_REQUESTS_SLOT);

#if (NGX_HTTP_CACHE)
    if (r->upstream == NULL || r->upstream->cache_status == 0) {
        ngx_cache_status_atomic_inc(r, UNCACHED_REQUESTS_SLOT);
        return ngx_original_filter_ptr(r);
    }

    if (r->upstream->cache_status > 7) { // greater than NGX_HTTP_CACHE_HIT
        ngx_cache_status_atomic_inc(r, MISC_SLOT);
    } else {
        ngx_cache_status_atomic_inc(r, r->upstream->cache_status + 1);
    }
#else
    ngx_cache_status_atomic_inc(r, UNCACHED_REQUESTS_SLOT);
#endif

    return ngx_original_filter_ptr(r);
}