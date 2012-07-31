/*
 * author Zhang Songfu
 */

#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <wand/MagickWand.h>


static char*	   ngx_http_imagemagick					 (ngx_conf_t *cf, ngx_command_t *cmd, void *conf);
static ngx_int_t   ngx_http_imagemagick_handler			 (ngx_http_request_t *r);
static ngx_str_t*  ngx_http_imagemagick_get_command		 (ngx_http_request_t *r);
static void		   ngx_http_imagemagick_command_handler	 (ngx_http_request_t *r);
static ngx_int_t   ngx_http_imagemagick_tokenize_command (ngx_http_request_t *r, ngx_str_t *cmd, ngx_array_t *a);
static u_char*     ngx_http_imagemagickd_map_uri_to_path (ngx_http_request_t *r, ngx_str_t *uri, ngx_str_t *path, size_t *root_length, size_t reserved);

static MagickBooleanType ngx_http_imagemagick_convert	 (char **argv, size_t argc);

#define ngx_http_imagemagick_server_error(r) ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR)

static ngx_command_t  ngx_http_imagemagick_commands[] = {
	{ ngx_string("imagemagick"),
	  NGX_HTTP_LOC_CONF|NGX_CONF_NOARGS,
	  ngx_http_imagemagick,
	  NGX_HTTP_LOC_CONF_OFFSET,
	  0,
	  NULL },

	ngx_null_command
};


static ngx_http_module_t  ngx_http_imagemagick_module_ctx = {
	NULL,						   /* preconfiguration */
	NULL,						   /* postconfiguration */

	NULL,						   /* create main configuration */
	NULL,						   /* init main configuration */

	NULL,						   /* create server configuration */
	NULL,						   /* merge server configuration */

	NULL,						   /* create location configuration */
	NULL						   /* merge location configuration */
};


ngx_module_t  ngx_http_imagemagick_module = {
	NGX_MODULE_V1,
	&ngx_http_imagemagick_module_ctx, /* module context */
	ngx_http_imagemagick_commands,	/* module directives */
	NGX_HTTP_MODULE,			   /* module type */
	NULL,						   /* init master */
	NULL,						   /* init module */
	NULL,						   /* init process */
	NULL,						   /* init thread */
	NULL,						   /* exit thread */
	NULL,						   /* exit process */
	NULL,						   /* exit master */
	NGX_MODULE_V1_PADDING
};


static char *
ngx_http_imagemagick(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
	ngx_http_core_loc_conf_t  *clcf;

	clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
	clcf->handler = ngx_http_imagemagick_handler;

	return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_imagemagick_handler(ngx_http_request_t *r)
{
	ngx_int_t	  rc;

	if (!(r->method & (NGX_HTTP_GET|NGX_HTTP_POST))) {
		return NGX_HTTP_NOT_ALLOWED;
	}
	
	if (r->method & NGX_HTTP_GET) {
		// handle get request, the convert source file is on local
		rc = ngx_http_discard_request_body(r);

		if (rc != NGX_OK) {
			return rc;
		}
	
		ngx_http_imagemagick_command_handler(r);
	} else {
		// handle post request, the convert source file is in request body
		r->request_body_in_file_only = 1;
		r->request_body_in_persistent_file = 1;
		r->request_body_in_clean_file = 1;
		r->request_body_file_group_access = 1;
		r->request_body_file_log_level = 0;
	
		rc = ngx_http_read_client_request_body(r, ngx_http_imagemagick_command_handler);
	
		if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
			return rc;
		}
	}
	
	return NGX_DONE;
}

static void 
ngx_http_imagemagick_command_handler(ngx_http_request_t *r)
{
	ngx_str_t				 *source;
	ngx_str_t				 *dest;
	ngx_str_t				 *ai;
	ngx_str_t				 *cmd;
	ngx_str_t                *uri;
	ngx_array_t				 *tokens;
	ngx_int_t				  rc;
	ngx_uint_t				  i;
	ngx_log_t				 *log;
	ngx_buf_t				 *b;
	ngx_chain_t				  out;
	
	ngx_fd_t				  fd;
	ngx_open_file_info_t	  of;
	ngx_http_core_loc_conf_t *clcf;
	
	size_t					  argc;
	char					**argv;
	
	u_char					 *cp;
	u_char					 *last;
	size_t					  root;
	ngx_temp_file_t			 *tf;
	
	MagickBooleanType		  status;
	
	
	log = r->connection->log;
	
	tokens = ngx_array_create(r->pool, 10, sizeof(ngx_str_t));
	if (tokens == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	ai = ngx_array_push(tokens);
	if (ai == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	ai->data = (u_char *) "gm convert";
	ai->len = 7;
	
	// get command from HTTP headers or queryString
	cmd = ngx_http_imagemagick_get_command(r);
	
	if (cmd == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	ngx_log_error(NGX_LOG_ERR, log, 0, "imagemagick convert command: \"%s\"", cmd->data);
	
	//ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
	//			   "imagemagick convert command: \"%s\"", cmd->data);
	
	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
	
	if (r->method & NGX_HTTP_POST) {
		source = dest = &r->request_body->temp_file->file.name;
	} else {
		uri = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
		source = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
		
		cp = cmd->data;
		while (cp < cmd->data + cmd->len) {
			if (*cp == ' ') {
				uri->data = cmd->data;
				uri->len = cp - cmd->data;
				
				cmd->data = cp + 1;
				cmd->len = cmd->len - uri->len - 1;
				break;
			}
			cp++;
		}
		
		if (uri->len == 0) {
			ngx_http_imagemagick_server_error(r);
			return;
		}
		
		last = ngx_http_imagemagickd_map_uri_to_path(r, uri, source, &root, 0);
		
		if (last == NULL) {
			ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
			return;
		}
		source->len = last - source->data;
		
		tf = ngx_pcalloc(r->pool, sizeof(ngx_temp_file_t));
		if (tf == NULL) {
			ngx_http_finalize_request(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
			return;
		}

		tf->file.fd = NGX_INVALID_FILE;
		tf->file.log = r->connection->log;
		tf->path = clcf->client_body_temp_path;
		tf->pool = r->pool;
		tf->log_level = r->request_body_file_log_level;
		tf->persistent = r->request_body_in_persistent_file;
		tf->clean = 1;

		if (r->request_body_file_group_access) {
			tf->access = 0660;
		}

		if (ngx_create_temp_file(&tf->file, tf->path, tf->pool,
								 tf->persistent, tf->clean, tf->access)
			!= NGX_OK)
		{
			ngx_http_imagemagick_server_error(r);
			return;
		}
		
		dest = &tf->file.name;
	}
	
	// push source file name into tokens
	ai = ngx_array_push(tokens);
	if (ai == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	*ai = *source;

	// tokenize command, and push them into tokens array
	rc = ngx_http_imagemagick_tokenize_command(r, cmd, tokens);
	if (rc == NGX_ERROR) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	ai = ngx_array_push_n(tokens, 2);
	if (ai == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	ai->data = (u_char *) "-compress";
	ai->len = 9;
	ai++;
	ai->data = (u_char *) "JPEG";
	ai->len = 4;
	
	// push dest filename into tokens again, to save generated thumbnail into dest file
	ai = ngx_array_push(tokens);
	if (ai == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	*ai = *dest;
	
	// OK, prepare convert args
	argc = tokens->nelts;
	
	argv = ngx_palloc(r->pool, argc * sizeof(char*));
	if (argv == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	ai = tokens->elts;
	for (i = 0; i < argc; i++) {
		argv[i] = (char *) ai[i].data;
		ngx_log_error(NGX_LOG_ERR, log, 0, "current[%d]: %s", i, argv[i]);
	}
	
	ngx_array_destroy(tokens);
	
	// DO ImageMagick converting
	status = ngx_http_imagemagick_convert(argv, argc);
	
	if (status == MagickFalse) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	// Done, write response

	of.test_dir = 0;
	//of.retest = clcf->open_file_cache_retest;
	of.errors = clcf->open_file_cache_errors;
	of.events = clcf->open_file_cache_events;

	rc = ngx_open_cached_file(clcf->open_file_cache, dest, &of, r->pool);

	if (rc == NGX_ERROR) {
		ngx_log_error(NGX_LOG_ERR, log, of.err,
						  "failed to open file \"%s\"", dest->data);
		ngx_http_imagemagick_server_error(r);
		return;
	}

	fd = of.fd;
	
	log->action = "sending response to client";
	
	r->headers_out.status = NGX_HTTP_OK;
	r->headers_out.content_type.len = sizeof("image/jpeg") - 1;
	r->headers_out.content_type.data = (u_char *) "image/jpeg";
	
	r->headers_out.content_length_n = of.size;
	r->headers_out.last_modified_time = of.mtime;
	
	if (r != r->main && of.size == 0) {
		rc = ngx_http_send_header(r);
		ngx_http_finalize_request(r, rc);
		return;
	}
	
	b = ngx_pcalloc(r->pool, sizeof(ngx_buf_t));
	if (b == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	b->file = ngx_pcalloc(r->pool, sizeof(ngx_file_t));
	if (b->file == NULL) {
		ngx_http_imagemagick_server_error(r);
		return;
	}
	
	rc = ngx_http_send_header(r);
	
	b->file_pos = 0;
	b->file_last = of.size;

	b->in_file = b->file_last ? 1: 0;
	b->last_buf = (r == r->main) ? 1: 0;
	b->last_in_chain = 1;

	b->file->fd = fd;
	b->file->name = *dest;
	b->file->log = log;

	out.buf = b;
	out.next = NULL;

	rc = ngx_http_output_filter(r, &out);
	ngx_http_finalize_request(r, rc);
	return;
}

static u_char * 
ngx_http_imagemagickd_map_uri_to_path(ngx_http_request_t *r, ngx_str_t *uri, ngx_str_t *path,
	size_t *root_length, size_t reserved)
{
	u_char					         *last;
	size_t					          alias;
	ngx_http_core_loc_conf_t         *clcf;

	clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);

	alias = clcf->alias ? clcf->name.len : 0;

	reserved += uri->len - alias + 1;

	if (clcf->root_lengths == NULL) {

		*root_length = clcf->root.len;

		path->len = clcf->root.len + reserved;

		path->data = ngx_palloc(r->pool, path->len);
		if (path->data == NULL) {
			return NULL;
		}

		last = ngx_copy(path->data, clcf->root.data, clcf->root.len);

	} else {
		path->len = reserved;
		path->data = ngx_palloc(r->pool, reserved);
		if (path->data == NULL) {
			return NULL;
		}
		
		if (ngx_conf_full_name((ngx_cycle_t *) ngx_cycle, path, 0) == NGX_ERROR) {
			return NULL;
		}
		
		*root_length = path->len - reserved;
		last = path->data + *root_length;
	}

	last = ngx_cpystrn(last, uri->data + alias, uri->len - alias + 1);

	return last;
}

static MagickBooleanType
ngx_http_imagemagick_convert(char **argv, size_t argc)
{
	// ImageMgick stuff
	ngx_uint_t				   i;
	char					  *option;
	ExceptionInfo			  *exception;
	ImageInfo				  *image_info;
	MagickBooleanType		   regard_warnings, status;
	
	// DO ImageMagick converting
	exception = AcquireExceptionInfo();
	regard_warnings = MagickFalse;
	for (i = 1; i < argc; i++) {
		option = argv[i];
		
		if ((ngx_strlen(option) == 1) || ((*option != '-') && (*option != '+'))) {
			continue;
		}
		if (LocaleCompare("debug", option + 1) == 0) {
			(void) SetLogEventMask(argv[++i]);
		}
		if (LocaleCompare("regard-warnings",option+1) == 0) {
			regard_warnings = MagickTrue;
		}
	}
	image_info = AcquireImageInfo();
	
	status = ConvertImageCommand(image_info, argc, argv, NULL, exception);
	
	if ((status == MagickFalse) || (exception->severity != UndefinedException)) {
		if ((exception->severity < ErrorException) && (regard_warnings == MagickFalse)) {
			status = MagickTrue;
		}
		CatchException(exception);
	}
	image_info = DestroyImageInfo(image_info);
	exception = DestroyExceptionInfo(exception);
	
	return status;
}

static ngx_str_t ngx_http_imagemagick_command_header = ngx_string("X-ImageMagick-Convert");

static ngx_str_t*
ngx_http_imagemagick_get_command(ngx_http_request_t *r)
{
	u_char							*dst, *src;
	size_t							 len;
	u_char							*p, *start_p;
	ngx_uint_t						 i;
	ngx_list_part_t					*part;
	ngx_table_elt_t					*header;
	ngx_str_t						*ret;

	part = &r->headers_in.headers.part;
	header = part->elts;

	for (i = 0; /* void */ ; i++) {

		if (i >= part->nelts) {
			if (part->next == NULL) {
				break;
			}

			part = part->next;
			header = part->elts;
			i = 0;
		}

		if (header[i].key.len == ngx_http_imagemagick_command_header.len
			&& ngx_strncmp(header[i].key.data, ngx_http_imagemagick_command_header.data,
						   header[i].key.len) == 0) {
			ret = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
			ret->data = header[i].value.data;
			ret->len = header[i].value.len;
			return ret;
		}
	}

	/* not found, check as a reaquest arg */
	if (r->args.len) {
		p = (u_char *) ngx_strstr(r->args.data, "X-ImageMagick-Convert=");

		if (p) {
			start_p = p += 22;
			while (p < r->args.data + r->args.len) {
				if (*p++ != '&') {
					continue;
				}
			}

			ret = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
			ret->data = start_p;
			ret->len = p - start_p;
			
			dst = ret->data;
			src = ret->data;

			ngx_unescape_uri(&dst, &src, ret->len, NGX_UNESCAPE_URI);
			
			len = (ret->data + ret->len) - src;
			if (len) {
				dst = ngx_copy(dst, src, len);
			}

			ret->len = dst - ret->data;
		
			return ret;
		}
	}

	return NULL;
}

static ngx_int_t
ngx_http_imagemagick_tokenize_command(ngx_http_request_t *r, ngx_str_t *cmd, ngx_array_t *dst) 
{
	u_char		 *pos;
	u_char		 *start_pos;
	u_char		 *data;
	ngx_str_t	 *token;
	ngx_str_t	 *ai;
	ngx_int_t	  ntokens;
	ngx_int_t	  len;

	ntokens = 0;
	start_pos = pos = cmd->data;
	
	while (pos < cmd->data + cmd->len) {
		if (*pos == ' ') {
			len = pos - start_pos;
			// If we've accumulated a token, this is the end of it. 
			if (len > 0) {
				data = ngx_pcalloc(r->pool, len + 1);
				ngx_memcpy(data, start_pos, len);
				*(data + len) = '\0';
				
				token = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
				token->data = data;
				token->len = len;

				ai = ngx_array_push(dst);
				if (ai == NULL) {
					return NGX_ERROR;
				}
				*ai = *token;
				ntokens++;
			}
			start_pos = pos + 1;
		}
		pos++;
	}
	
	len = pos - start_pos;
	if (len > 0) {
		data = ngx_pcalloc(r->pool, len + 1);
		ngx_memcpy(data, start_pos, len);
		*(data + len) = '\0';
		
		token = ngx_pcalloc(r->pool, sizeof(ngx_str_t));
		token->data = data;
		token->len = len;

		ai = ngx_array_push(dst);
		if (ai == NULL) {
			return NGX_ERROR;
		}
		*ai = *token;
		ntokens++;
	}
	
	return ntokens;
}
