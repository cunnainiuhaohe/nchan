ngx_int_t ngx_http_complex_value_noalloc(ngx_http_request_t *r, ngx_http_complex_value_t *val, ngx_str_t *value, size_t maxlen);
u_char *nchan_strsplit(u_char **s1, ngx_str_t *sub, u_char *last_char);
ngx_str_t * nchan_get_header_value(ngx_http_request_t * r, ngx_str_t header_name);
ngx_buf_t * nchan_request_body_to_single_buffer(ngx_http_request_t *r);