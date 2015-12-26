/*
 *  Written by Leo Ponomarev 2009-2015
 */

#include <assert.h>
#include <nchan_module.h>

#include <subscribers/longpoll.h>
#include <subscribers/intervalpoll.h>
#include <subscribers/eventsource.h>
#include <subscribers/http-chunked.h>
#include <subscribers/http-multipart-mixed.h>
#include <subscribers/websocket.h>
#include <store/memory/store.h>
#include <store/redis/store.h>
#include <nchan_setup.c>
#include <store/memory/ipc.h>
#include <store/memory/shmem.h>
//#include <store/memory/store-private.h> //for debugging
#include <util/nchan_output.h>
#include <nchan_websocket_publisher.h>

ngx_int_t           nchan_worker_processes;
ngx_pool_t         *nchan_pool;
ngx_module_t        nchan_module;

//#define DEBUG_LEVEL NGX_LOG_WARN
#define DEBUG_LEVEL NGX_LOG_DEBUG

#define DBG(fmt, args...) ngx_log_error(DEBUG_LEVEL, ngx_cycle->log, 0, "NCHAN:" fmt, ##args)
#define ERR(fmt, args...) ngx_log_error(NGX_LOG_ERR, ngx_cycle->log, 0, "NCHAN:" fmt, ##args)

ngx_int_t nchan_maybe_send_channel_event_message(ngx_http_request_t *r, channel_event_type_t event_type) {
  static nchan_loc_conf_t            evcf_data;
  static nchan_loc_conf_t           *evcf = NULL;
  
  static ngx_str_t group =           ngx_string("meta");
  
  static ngx_str_t evt_sub_enqueue = ngx_string("subscriber_enqueue");
  static ngx_str_t evt_sub_dequeue = ngx_string("subscriber_dequeue");
  static ngx_str_t evt_sub_recvmsg = ngx_string("subscriber_receive_message");
  static ngx_str_t evt_sub_recvsts = ngx_string("subscriber_receive_status");
  static ngx_str_t evt_chan_publish= ngx_string("channel_publish");
  static ngx_str_t evt_chan_delete = ngx_string("channel_delete");

  struct timeval             tv;
  
  nchan_loc_conf_t          *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  ngx_http_complex_value_t  *cv = cf->channel_events_channel_id;
  if(cv==NULL) {
    //nothing to send
    return NGX_OK;
  }
  
  nchan_request_ctx_t       *ctx = ngx_http_get_module_ctx(r, nchan_module);
  ngx_str_t                  tmpid;
  size_t                     sz;
  ngx_str_t                 *id;
  u_char                    *cur;
  ngx_str_t                  evstr;
  ngx_buf_t                  buf;
  nchan_msg_t                msg;
  
  switch(event_type) {
    case SUB_ENQUEUE:
      ctx->channel_event_name = &evt_sub_enqueue;
      break;
    case SUB_DEQUEUE:
      ctx->channel_event_name = &evt_sub_dequeue;
      break;
    case SUB_RECEIVE_MESSAGE:
      ctx->channel_event_name = &evt_sub_recvmsg;
      break;
    case SUB_RECEIVE_STATUS:
      ctx->channel_event_name = &evt_sub_recvsts;
      break;
    case CHAN_PUBLISH:
      ctx->channel_event_name = &evt_chan_publish;
      break;
    case CHAN_DELETE:
      ctx->channel_event_name = &evt_chan_delete;
      break;
  }
  
  //the id
  ngx_http_complex_value(r, cv, &tmpid); 
  sz = group.len + 1 + tmpid.len;
  if((id = ngx_palloc(r->pool, sizeof(*id) + sz)) == NULL) {
    ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: can't allocate space for legacy channel id");
    return NGX_ERROR;
  }
  id->len = sz;
  id->data = (u_char *)&id[1];
  cur = id->data;  
  ngx_memcpy(cur, group.data, group.len);
  cur += group.len;
  cur[0]='/';
  cur++;
  ngx_memcpy(cur, tmpid.data, tmpid.len);
  
  
  //the event message
  ngx_http_complex_value(r, cf->channel_event_string, &evstr);
  ngx_memzero(&buf, sizeof(buf)); //do we really need this?...
  buf.temporary = 1;
  buf.memory = 1;
  buf.last_buf = 1;
  buf.pos = evstr.data;
  buf.last = evstr.data + evstr.len;
  buf.start = buf.pos;
  buf.end = buf.last;
  
  ngx_memzero(&msg, sizeof(msg));
  ngx_gettimeofday(&tv);
  msg.id.time = tv.tv_sec;
  msg.id.tagcount = 1;
  msg.buf = &buf;
  
  
  if(evcf == NULL) {
    evcf = &evcf_data;
    ngx_memzero(evcf, sizeof(*evcf));
    evcf->buffer_timeout = 10;
    evcf->max_messages = NGX_MAX_INT_T_VALUE;
    evcf->subscriber_start_at_oldest_message = 0;
    evcf->channel_timeout = 30;
  }
  evcf->storage_engine = cf->storage_engine;
  evcf->use_redis = cf->use_redis;
  
  evcf->storage_engine->publish(id, &msg, evcf, NULL, NULL);
  
  return NGX_OK;
}

static void memstore_sub_debug_start() {
#if FAKESHARD  
  #ifdef SUB_FAKE_WORKER
  memstore_fakeprocess_push(SUB_FAKE_WORKER);
  #else
  memstore_fakeprocess_push_random();
  #endif
#endif   
}
static void memstore_sub_debug_end() {
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

static void memstore_pub_debug_start() {
#if FAKESHARD
  #ifdef PUB_FAKE_WORKER
  memstore_fakeprocess_push(PUB_FAKE_WORKER);
  #else
  memstore_fakeprocess_push_random();
  #endif
#endif
}
static void memstore_pub_debug_end() {
#if FAKESHARD
  memstore_fakeprocess_pop();
#endif
}

static void nchan_publisher_body_handler(ngx_http_request_t *r);

static ngx_int_t nchan_http_publisher_handler(ngx_http_request_t * r) {
  ngx_int_t                       rc;
  nchan_request_ctx_t            *ctx = ngx_http_get_module_ctx(r, nchan_module);
  
  static ngx_str_t                publisher_name = ngx_string("http");
  
  if(ctx) ctx->publisher_type = &publisher_name;
  
  /* Instruct ngx_http_read_subscriber_request_body to store the request
     body entirely in a memory buffer or in a file */
  r->request_body_in_single_buf = 1;
  r->request_body_in_persistent_file = 1;
  r->request_body_in_clean_file = 0;
  r->request_body_file_log_level = 0;
  
  //don't buffer the request body --send it right on through
  //r->request_body_no_buffering = 1;

  rc = ngx_http_read_client_request_body(r, nchan_publisher_body_handler);
  if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
    return rc;
  }
  return NGX_DONE;
}

ngx_int_t nchan_pubsub_handler(ngx_http_request_t *r) {
  nchan_loc_conf_t       *cf = ngx_http_get_module_loc_conf(r, nchan_module);
  ngx_str_t              *channel_id;
  subscriber_t           *sub;
  nchan_msg_id_t         *msg_id;
  ngx_int_t               rc = NGX_DONE;
  nchan_request_ctx_t    *ctx;
  ngx_str_t              *origin_header;
  
#if NCHAN_BENCHMARK
  struct timeval          tv;
  ngx_gettimeofday(&tv);
#endif
  
  if((ctx = ngx_pcalloc(r->pool, sizeof(nchan_request_ctx_t))) == NULL) {
    return NGX_HTTP_INTERNAL_SERVER_ERROR;
  }
  ngx_http_set_ctx(r, ctx, nchan_module);

#if NCHAN_BENCHMARK
  ctx->start_tv = tv;
#endif
  
  if((origin_header = nchan_get_header_value(r, NCHAN_HEADER_ORIGIN)) != NULL) {
    ctx->request_origin_header = *origin_header;
    if(!(cf->allow_origin.len == 1 && cf->allow_origin.data[0] == '*')) {
      if(!(origin_header->len == cf->allow_origin.len && ngx_strnstr(origin_header->data, (char *)cf->allow_origin.data, origin_header->len) != NULL)) {
        //CORS origin match failed! return a 403 forbidden
        goto forbidden;
      }
    }
  }
  else {
    ctx->request_origin_header.len=0;
    ctx->request_origin_header.data=NULL;
  }
  
  if((channel_id = nchan_get_channel_id(r, SUB, 1)) == NULL) {
    //just get the subscriber_channel_id for now. the publisher one is handled elsewhere
    return r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR;
  }

  if(nchan_detect_websocket_request(r)) {
    //want websocket?
    if(cf->sub.websocket) {
      //we prefer to subscribe
      memstore_sub_debug_start();
      msg_id = nchan_subscriber_get_msg_id(r);
      if((sub = websocket_subscriber_create(r, msg_id)) == NULL) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unable to create websocket subscriber");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
      }
      sub->fn->subscribe(sub, channel_id);
      
      memstore_sub_debug_end();
    }
    else if(cf->pub.websocket) {
      //no need to subscribe, but keep a connection open for publishing
      //not yet implemented
      nchan_create_websocket_publisher(r);
    }
    else goto forbidden;
    return NGX_DONE;
  }
  else {
    subscriber_t *(*sub_create)(ngx_http_request_t *r, nchan_msg_id_t *msg_id) = NULL;
    
    switch(r->method) {
      case NGX_HTTP_GET:
        if(cf->sub.eventsource && nchan_detect_eventsource_request(r)) {
          sub_create = eventsource_subscriber_create;
        }
        else if(cf->sub.http_chunked && nchan_detect_chunked_subscriber_request(r)) {
          sub_create = http_chunked_subscriber_create;
        }
        else if(cf->sub.http_multipart && nchan_detect_multipart_subscriber_request(r)) {
          sub_create = http_multipart_subscriber_create;
        }
        else if(cf->sub.poll) {
          sub_create = intervalpoll_subscriber_create;
        }
        else if(cf->sub.longpoll) {
          sub_create = longpoll_subscriber_create;
        }
        else if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else {
          goto forbidden;
        }
        
        if(sub_create) {
          memstore_sub_debug_start();
          
          msg_id = nchan_subscriber_get_msg_id(r);
          if((sub = sub_create(r, msg_id)) == NULL) {
            ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "unable to create subscriber");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
          }
          
          sub->fn->subscribe(sub, channel_id);
          
          memstore_sub_debug_end();
        }
        
        break;
      
      case NGX_HTTP_POST:
      case NGX_HTTP_PUT:
        if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else goto forbidden;
        break;
      
      case NGX_HTTP_DELETE:
        if(cf->pub.http) {
          nchan_http_publisher_handler(r);
        }
        else goto forbidden;
        break;
      
      case NGX_HTTP_OPTIONS:
        if(cf->pub.http) {
          nchan_OPTIONS_respond(r, &cf->allow_origin, &NCHAN_ACCESS_CONTROL_ALLOWED_PUBLISHER_HEADERS, &NCHAN_ALLOW_GET_POST_PUT_DELETE_OPTIONS);
        }
        else if(cf->sub.poll || cf->sub.longpoll || cf->sub.eventsource || cf->sub.websocket) {
          nchan_OPTIONS_respond(r, &cf->allow_origin, &NCHAN_ACCESS_CONTROL_ALLOWED_SUBSCRIBER_HEADERS, &NCHAN_ALLOW_GET_OPTIONS);
        }
        else goto forbidden;
        break;
    }
  }
  
  return rc;
  
forbidden:
  nchan_respond_status(r, NGX_HTTP_FORBIDDEN, NULL, 0);
  return NGX_OK;
}

static ngx_int_t channel_info_callback(ngx_int_t status, void *rptr, ngx_http_request_t *r) {
  ngx_http_finalize_request(r, nchan_response_channel_ptr_info( (nchan_channel_t *)rptr, r, 0));
  return NGX_OK;
}

static ngx_int_t publish_callback(ngx_int_t status, void *rptr, ngx_http_request_t *r) {
  nchan_channel_t       *ch = rptr;
  nchan_request_ctx_t   *ctx = ngx_http_get_module_ctx(r, nchan_module);
  static nchan_msg_id_t  empty_msgid = NCHAN_ZERO_MSGID;
  //DBG("publish_callback %V owner %i status %i", ch_id, memstore_channel_owner(ch_id), status);
  switch(status) {
    case NCHAN_MESSAGE_QUEUED:
      //message was queued successfully, but there were no subscribers to receive it.
      ctx->prev_msg_id = ctx->msg_id;
      ctx->msg_id = ch != NULL ? ch->last_published_msg_id : empty_msgid;
      
      nchan_maybe_send_channel_event_message(r, CHAN_PUBLISH);
      ngx_http_finalize_request(r, nchan_response_channel_ptr_info(ch, r, NGX_HTTP_ACCEPTED));
      return NGX_OK;
      
    case NCHAN_MESSAGE_RECEIVED:
      //message was queued successfully, and it was already sent to at least one subscriber
      ctx->prev_msg_id = ctx->msg_id;
      ctx->msg_id = ch != NULL ? ch->last_published_msg_id : empty_msgid;
      
      nchan_maybe_send_channel_event_message(r, CHAN_PUBLISH);
      ngx_http_finalize_request(r, nchan_response_channel_ptr_info(ch, r, NGX_HTTP_CREATED));
      return NGX_OK;
      
    case NGX_ERROR:
    case NGX_HTTP_INTERNAL_SERVER_ERROR:
      //WTF?
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: error publishing message");
      ctx->prev_msg_id = empty_msgid;;
      ctx->msg_id = empty_msgid;
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return NGX_ERROR;
      
    default:
      //for debugging, mostly. I don't expect this branch to behit during regular operation
      ctx->prev_msg_id = empty_msgid;;
      ctx->msg_id = empty_msgid;
      ngx_log_error(NGX_LOG_ERR, r->connection->log, 0, "nchan: TOTALLY UNEXPECTED error publishing message, status code %i", status);
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return NGX_ERROR;
  }
}


static void nchan_publisher_body_handler_continued(ngx_http_request_t *r, ngx_str_t *channel_id, nchan_loc_conf_t *cf) {
  ngx_buf_t                      *buf;
  size_t                          content_type_len;
  nchan_msg_t                    *msg;
  struct timeval                  tv;
  
  switch(r->method) {
    case NGX_HTTP_GET:
      cf->storage_engine->find_channel(channel_id, (callback_pt) &channel_info_callback, (void *)r);
      break;
    
    case NGX_HTTP_PUT:
    case NGX_HTTP_POST:
      memstore_pub_debug_start();

      if((msg = ngx_pcalloc(r->pool, sizeof(*msg))) == NULL) {
        ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, "nchan: can't allocate msg in request pool");
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return; 
      }
      msg->shared = 0;
      
      //content type
      content_type_len = (r->headers_in.content_type!=NULL ? r->headers_in.content_type->value.len : 0);
      if(content_type_len > 0) {
        msg->content_type.len = content_type_len;
        msg->content_type.data = r->headers_in.content_type->value.data;
      }
      
      if(r->headers_in.content_length_n == -1 || r->headers_in.content_length_n == 0) {
        buf = ngx_create_temp_buf(r->pool, 0);
      }
      else if(r->request_body->bufs!=NULL) {
        buf = nchan_request_body_to_single_buffer(r);
      }
      else {
        ngx_log_error(NGX_LOG_ERR, (r)->connection->log, 0, "nchan: unexpected publisher message request body buffer location. please report this to the nchan developers.");
        ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
      }
      
      ngx_gettimeofday(&tv);
      msg->id.time = tv.tv_sec;
      msg->id.tag.fixed[0] = 0;
      msg->id.tagactive = 0;
      msg->id.tagcount = 1;
      
      msg->buf = buf;
#if NCHAN_MSG_LEAK_DEBUG
      msg->lbl = r->uri;
#endif
#if NCHAN_BENCHMARK
      nchan_request_ctx_t            *ctx = ngx_http_get_module_ctx(r, nchan_module);
      msg->start_tv = ctx->start_tv;
#endif
      
      cf->storage_engine->publish(channel_id, msg, cf, (callback_pt) &publish_callback, r);
      
      memstore_pub_debug_end();
      break;
      
    case NGX_HTTP_DELETE:
      cf->storage_engine->delete_channel(channel_id, (callback_pt) &channel_info_callback, (void *)r);
      nchan_maybe_send_channel_event_message(r, CHAN_DELETE);
      break;
      
    default: 
      nchan_respond_status(r, NGX_HTTP_FORBIDDEN, NULL, 0);
  }
  
}

typedef struct {
  ngx_str_t       *ch_id;
} nchan_pub_subrequest_data_t;

typedef struct {
  ngx_http_post_subrequest_t    psr;
  nchan_pub_subrequest_data_t   psr_data;
} nchan_pub_subrequest_stuff_t;


static ngx_int_t nchan_publisher_body_authorize_handler(ngx_http_request_t *r, void *data, ngx_int_t rc) {
  nchan_pub_subrequest_data_t  *d = data;
  
  if(rc == NGX_OK) {
    nchan_loc_conf_t    *cf = ngx_http_get_module_loc_conf(r->main, nchan_module);
    ngx_int_t            code = r->headers_out.status;
    if(code >= 200 && code <299) {
      //authorized. proceed as planned
      nchan_publisher_body_handler_continued(r->main, d->ch_id, cf);
    }
    else { //anything else means forbidden
      ngx_http_finalize_request(r->main, NGX_HTTP_FORBIDDEN);
    }
  }
  else {
    ngx_http_finalize_request(r->main, NGX_HTTP_INTERNAL_SERVER_ERROR);
  }
  return NGX_OK;
}

static void nchan_publisher_body_handler(ngx_http_request_t *r) {
  ngx_str_t                      *channel_id;
  nchan_loc_conf_t               *cf = ngx_http_get_module_loc_conf(r, nchan_module);

  ngx_http_complex_value_t       *authorize_request_url_ccv = cf->authorize_request_url;
  
  if((channel_id = nchan_get_channel_id(r, PUB, 1))==NULL) {
    ngx_http_finalize_request(r, r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR);
    return;
  }
  
  if(!authorize_request_url_ccv) {
    nchan_publisher_body_handler_continued(r, channel_id, cf);
  }
  else {
    nchan_pub_subrequest_stuff_t   *psr_stuff;
    
    if((psr_stuff = ngx_palloc(r->pool, sizeof(*psr_stuff))) == NULL) {
      ERR("can't allocate memory for publisher auth subrequest");
      ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
      return;
    }
    
    ngx_http_post_subrequest_t    *psr = &psr_stuff->psr;
    nchan_pub_subrequest_data_t   *psrd = &psr_stuff->psr_data;
    ngx_http_request_t            *sr;
    ngx_str_t                      auth_request_url;
    
    ngx_http_complex_value(r, authorize_request_url_ccv, &auth_request_url);
    
    psr->handler = nchan_publisher_body_authorize_handler;
    psr->data = psrd;
    
    psrd->ch_id = channel_id;
    
    ngx_http_subrequest(r, &auth_request_url, NULL, &sr, psr, 0);
    sr->method = r->method;
    sr->method_name = r->method_name;
    
    if((sr->request_body = ngx_pcalloc(r->pool, sizeof(ngx_http_request_body_t))) == NULL) {
      ERR("can't allocate memory for publisher auth subrequest body");
      ngx_http_finalize_request(r, r->headers_out.status ? NGX_OK : NGX_HTTP_INTERNAL_SERVER_ERROR);
      return;
    }
    sr->header_only = 1;
  }
}

#if NCHAN_BENCHMARK
int nchan_timeval_subtract(struct timeval *result, struct timeval *x, struct timeval *y) {
  /* Perform the carry for the later subtraction by updating y. */
  if (x->tv_usec < y->tv_usec) {
    int nsec = (y->tv_usec - x->tv_usec) / 1000000 + 1;
    y->tv_usec -= 1000000 * nsec;
    y->tv_sec += nsec;
  }
  if (x->tv_usec - y->tv_usec > 1000000) {
    int nsec = (x->tv_usec - y->tv_usec) / 1000000;
    y->tv_usec += 1000000 * nsec;
    y->tv_sec -= nsec;
  }

  /* Compute the time remaining to wait.
     tv_usec is certainly positive. */
  result->tv_sec = x->tv_sec - y->tv_sec;
  result->tv_usec = x->tv_usec - y->tv_usec;

  /* Return 1 if result is negative. */
  return x->tv_sec < y->tv_sec;
}
#endif


#if NCHAN_SUBSCRIBER_LEAK_DEBUG

subscriber_t *subdebug_head = NULL;

void subscriber_debug_add(subscriber_t *sub) {
  if(subdebug_head == NULL) {
    sub->dbg_next = NULL;
    sub->dbg_prev = NULL;
  }
  else {
    sub->dbg_next = subdebug_head;
    sub->dbg_prev = NULL;
    assert(subdebug_head->dbg_prev == NULL);
    subdebug_head->dbg_prev = sub;
  }
  subdebug_head = sub;
}
void subscriber_debug_remove(subscriber_t *sub) {
  subscriber_t *prev, *next;
  prev = sub->dbg_prev;
  next = sub->dbg_next;
  if(subdebug_head == sub) {
    assert(sub->dbg_prev == NULL);
    if(next) {
      next->dbg_prev = NULL;
    }
    subdebug_head = next;
  }
  else {
    if(prev) {
      prev->dbg_next = next;
    }
    if(next) {
      next->dbg_prev = prev;
    }
  }
  
  sub->dbg_next = NULL;
  sub->dbg_prev = NULL;
}
void subscriber_debug_assert_isempty(void) {
  assert(subdebug_head == NULL);
}
#endif
