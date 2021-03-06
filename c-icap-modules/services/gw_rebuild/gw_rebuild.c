#include <wchar.h>
#include "c_icap/c-icap.h"
#include "c_icap/service.h"
#include "c_icap/header.h"
#include "c_icap/simple_api.h"
#include "c_icap/debug.h"
#include "c_icap/cfg_param.h"
#include "c_icap/filetype.h"
#include "c_icap/ci_threads.h"
#include "c_icap/mem.h"
#include "c_icap/commands.h"
#include "c_icap/txt_format.h"
#include "c_icap/txtTemplate.h"
#include "c_icap/stats.h"
#include "gw_rebuild.h"
#include "gw_proxy_api.h"
#include "gw_guid.h"
#include "gw_env_var.h"

#include "md5.h"
#include "common.h"
#include <assert.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <sys/stat.h>
static void generate_403_headers(ci_request_t *req);
static void generate_error_page(gw_rebuild_req_data_t *data, ci_request_t *req);
static void rebuild_content_length(ci_request_t *req, gw_body_data_t *body);
static int file_exists (char *filename);
/***********************************************************************************/
/* Module definitions                                                              */

static int ALLOW204 = 1;
static ci_off_t MAX_OBJECT_SIZE = 5*1024*1024;
static int DATA_CLEANUP = 1;
static const int GW_ENABLE_FILE_ID_REPORTING  = 1;
static const int GW_DISABLE_FILE_ID_REPORTING = 0;
#define GW_VERSION_SIZE 15
#define GW_BT_FILE_PATH_SIZE 150
#define STATS_BUFFER 1024

enum rebuild_request_body_return {REBUILD_UNPROCESSED=0, REBUILD_REBUILT=1, REBUILD_FAILED=2, REBUILD_ERROR=9};

char *PROXY_APP_LOCATION = NULL;

char *REBUILD_VERSION = "2.1.1";

/*Statistic  Ids*/
static int GW_SCAN_REQS = -1;
static int GW_SCAN_BYTES = -1;
static int GW_REBUILD_FAILURES = -1;
static int GW_REBUILD_ERRORS = -1;
static int GW_REBUILD_SUCCESSES = -1;
static int GW_NOT_PROCESSED = -1;
static int GW_UNPROCESSABLE = -1;

/*********************/
/* Formating table   */
static int fmt_gw_rebuild_fileid(ci_request_t *req, char *buf, int len, const char *param);

struct ci_fmt_entry gw_rebuild_report_format_table [] = {
    {"%GI", "The file id blocked is", fmt_gw_rebuild_fileid},
    { NULL, NULL, NULL}
};

static ci_service_xdata_t *gw_rebuild_xdata = NULL;

static int GWREQDATA_POOL = -1;

static char *ENABLE_FILE_ID_REPORTING_VARIABLE = "EnableFileId";
static int REPORT_FILE_ID;

static int gw_rebuild_init_service(ci_service_xdata_t *srv_xdata,
                           struct ci_server_conf *server_conf);
static int gw_rebuild_post_init_service(ci_service_xdata_t *srv_xdata,
                           struct ci_server_conf *server_conf);
static void gw_rebuild_close_service();
static int gw_rebuild_check_preview_handler(char *preview_data, int preview_data_len,
                                    ci_request_t *);
static int gw_rebuild_end_of_data_handler(ci_request_t *);
static void *gw_rebuild_init_request_data(ci_request_t *req);
static void gw_rebuild_release_request_data(void *srv_data);
static int gw_rebuild_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof,
                 ci_request_t *req);

/*Arguments parse*/
static void gw_rebuild_parse_args(gw_rebuild_req_data_t *data, char *args);

/*General functions*/
static void set_istag(ci_service_xdata_t *srv_xdata);
static void cmd_reload_istag(const char *name, int type, void *data);
static int init_body_data(ci_request_t *req);

/*Configuration Table .....*/
static struct ci_conf_entry conf_variables[] = {
    {"MaxObjectSize", &MAX_OBJECT_SIZE, ci_cfg_size_off, NULL},
    {"Allow204Responses", &ALLOW204, ci_cfg_onoff, NULL},
    {"DataCleanup", &DATA_CLEANUP, ci_cfg_onoff, NULL},
    {"ProxyAppLocation", &PROXY_APP_LOCATION, ci_cfg_set_str, NULL},
};

CI_DECLARE_MOD_DATA ci_service_module_t service = {
    "gw_rebuild",              /*Module name */
    "Glasswall Rebuild service",        /*Module short description */
    ICAP_RESPMOD | ICAP_REQMOD,        /*Service type response or request modification */
    gw_rebuild_init_service,    /*init_service. */
    gw_rebuild_post_init_service,   /*post_init_service. */
    gw_rebuild_close_service,   /*close_service */
    gw_rebuild_init_request_data,       /*init_request_data. */
    gw_rebuild_release_request_data,    /*release request data */
    gw_rebuild_check_preview_handler,
    gw_rebuild_end_of_data_handler,
    gw_rebuild_io,
    conf_variables,
    NULL
};

static char* concat(char* output, const char* s1, const char* s2);
static int gw_rebuild_init_service(ci_service_xdata_t *srv_xdata,
                           struct ci_server_conf *server_conf)
{   
    gw_rebuild_xdata = srv_xdata;

    ci_service_set_preview(srv_xdata, 1024);
    ci_service_enable_204(srv_xdata);
    ci_service_set_transfer_preview(srv_xdata, "*");

    /*Initialize object pools*/
    GWREQDATA_POOL = ci_object_pool_register("gw_rebuild_req_data_t", sizeof(gw_rebuild_req_data_t));

    if(GWREQDATA_POOL < 0) {
        ci_debug_printf(1, "gw_rebuild_init_service: error registering object_pool gw_rebuild_req_data_t\n");
        return CI_ERROR;
    }

    /*initialize statistic counters*/
    /* TODO:convert to const after fix ci_stat_* api*/
    char template_buf[STATS_BUFFER];
    char buf[STATS_BUFFER];
    buf[STATS_BUFFER-1] = '\0';
    char *stats_label = "Service gw_rebuild";
    concat(template_buf, stats_label, " %s");
    snprintf(buf, STATS_BUFFER-1, template_buf, "REQUESTS SCANNED");
    GW_SCAN_REQS = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "BODY BYTES SCANNED");
    GW_SCAN_BYTES = ci_stat_entry_register(buf, STAT_KBS_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "REBUILD FAILURES");    
    GW_REBUILD_FAILURES = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "REBUILD ERRORS");
    GW_REBUILD_ERRORS = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "SCAN REBUILT");
    GW_REBUILD_SUCCESSES = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "UNPROCESSED");
    GW_NOT_PROCESSED = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);
    snprintf(buf, STATS_BUFFER-1, template_buf, "UNPROCESSABLE");
    GW_UNPROCESSABLE = ci_stat_entry_register(buf, STAT_INT64_T, stats_label);   

    int set_result;
    set_result = set_from_environment_variable_bool(ENABLE_FILE_ID_REPORTING_VARIABLE, &REPORT_FILE_ID, GW_DISABLE_FILE_ID_REPORTING);
    if (set_result == -1){
        ci_debug_printf(1, "gw_rebuild_init_service: File Id Reporting configuration error, default to 'false'\n");  
        REPORT_FILE_ID = GW_DISABLE_FILE_ID_REPORTING;
    }
     
    if (set_result == 0)
        ci_debug_printf(5, "gw_rebuild_init_service: File Id Reporting set to default value\n");    

    ci_debug_printf(5, "gw_rebuild_init_service: File Id Reporting = %s\n", REPORT_FILE_ID == GW_ENABLE_FILE_ID_REPORTING? "true" : "false");    
    return CI_OK;
}


static int gw_rebuild_post_init_service(ci_service_xdata_t *srv_xdata,
                           struct ci_server_conf *server_conf)
{   
    if (!PROXY_APP_LOCATION){
       ci_debug_printf(1, "Proxy App location not specified\n");
       return CI_ERROR;
    }
    
    if (!file_exists(PROXY_APP_LOCATION)){
       ci_debug_printf(1, "Proxy App not found at %s\n", PROXY_APP_LOCATION);
       return CI_ERROR;   
    }    
    
    set_istag(gw_rebuild_xdata);
    register_command_extend(GW_RELOAD_ISTAG, ONDEMAND_CMD, NULL, cmd_reload_istag);

    ci_debug_printf(1, "Using Proxy App at %s\n", PROXY_APP_LOCATION);    
    return CI_OK;
}

static void gw_rebuild_close_service()
{
    ci_debug_printf(3, "gw_rebuild_close_service......\n");
    ci_object_pool_unregister(GWREQDATA_POOL);
}

static void *gw_rebuild_init_request_data(ci_request_t *req)
{
    int preview_size;
    gw_rebuild_req_data_t *data;

    ci_debug_printf(3, "gw_rebuild_init_request_data......\n");

     preview_size = ci_req_preview_size(req);

    if (req->args[0] != '\0') {
        ci_debug_printf(5, "service arguments:%s\n", req->args);
    }
    if (ci_req_hasbody(req)) {
        ci_debug_printf(5, "Request type: %d. Preview size:%d\n", req->type, preview_size);
        if (!(data = ci_object_pool_alloc(GWREQDATA_POOL))) {
            ci_debug_printf(1, "Error allocation memory for service data!!!!!!!\n");
            return NULL;
        }
        memset(&data->body,0, sizeof(gw_body_data_t));
        data->error_page = NULL;
        data->url_log[0] = '\0';
        data->gw_status = GW_STATUS_UNDEFINED;
        data->gw_processing = GW_PROCESSING_UNDEFINED;
        if (ALLOW204)
            data->args.enable204 = 1;
        else
            data->args.enable204 = 0;
        data->args.sizelimit = 1;
        data->args.mode = 0;

        if (req->args[0] != '\0') {
            ci_debug_printf(5, "service arguments:%s\n", req->args);
            gw_rebuild_parse_args(data, req->args);
        }
        if (data->args.enable204 && ci_allow204(req))
            data->allow204 = 1;
        else
            data->allow204 = 0;
        data->req = req;
        
        generate_random_guid(data->file_id);
        
        
        ci_debug_printf(3, "gw_rebuild_init_request_data:FileId:%s\n", data->file_id);

        return data;
    }
    return NULL;
}

static void gw_rebuild_release_request_data(void *data)
{
    if (data) {

        gw_rebuild_req_data_t *requestData = (gw_rebuild_req_data_t *) data;
        ci_debug_printf(3, "Releasing gw_rebuild data:FileId:%s\n", requestData->file_id);
        if (DATA_CLEANUP)
        {            
            gw_body_data_destroy(&requestData->body);
        }
        else
        {
            ci_debug_printf(3, "Leaving gw_rebuild data body:FileId:%s\n", requestData->file_id);
        }

        if (((gw_rebuild_req_data_t *) data)->error_page)
            ci_membuf_free(((gw_rebuild_req_data_t *) data)->error_page);

        ci_object_pool_free(data);
     }
}

static int gw_rebuild_check_preview_handler(char *preview_data, int preview_data_len,
                                    ci_request_t *req)
{
     ci_off_t content_size = 0;

     gw_rebuild_req_data_t *data = ci_service_data(req);

     ci_debug_printf(3, "gw_rebuild_check_preview_handler:FileId:%s, preview data size is %d\n", data->file_id, preview_data_len);

     if (!data || !ci_req_hasbody(req)){
        ci_debug_printf(6, "No body data, allow 204:FileId:%s\n", data->file_id);
        ci_stat_uint64_inc(GW_UNPROCESSABLE, 1); 
        return CI_MOD_ALLOW204;
     }

    data->max_object_size = MAX_OBJECT_SIZE;

    /*Compute the expected size, will be used by must_scanned*/
    content_size = ci_http_content_length(req);
    data->expected_size = content_size;
    ci_debug_printf(6, "gw_rebuild_check_preview_handler:FileId:%s, expected_size is %ld\n", data->file_id, content_size);

    /*log objects url*/
    if (!ci_http_request_url(req, data->url_log, LOG_URL_SIZE)) {
        ci_debug_printf(2, "Failed to retrieve HTTP request URL:FileId:%s\n", data->file_id);
    }

    if (init_body_data(req) == CI_ERROR){
        ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1);         
        return CI_ERROR;
    }
    
    if (preview_data_len == 0) {
        return CI_MOD_CONTINUE;
    }
    
    if (preview_data_len && 
        gw_body_data_write(&data->body, preview_data, preview_data_len, ci_req_hasalldata(req)) == CI_ERROR){
            ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1);                        
            return CI_ERROR;
    }
    ci_debug_printf(6, "gw_rebuild_check_preview_handler:FileId:%s, gw_body_data_write data_len %d\n", data->file_id, preview_data_len);

    return CI_MOD_CONTINUE;
}

int gw_rebuild_write_to_net(char *buf, int len, ci_request_t *req)
{
    int bytes;
    gw_rebuild_req_data_t *data = ci_service_data(req);
     ci_debug_printf(9, "gw_rebuild_write_to_net:FileId:%s, buf len is %d\n", data->file_id, len);
    if (!data)
        return CI_ERROR;

    bytes = gw_body_data_read(&data->body, buf, len);

    ci_debug_printf(9, "gw_rebuild_write_to_net:FileId:%s, write bytes is %d\n", data->file_id, bytes);

    return bytes;
}

int gw_rebuild_read_from_net(char *buf, int len, int iseof, ci_request_t *req)
{
    gw_rebuild_req_data_t *data = ci_service_data(req);
    ci_debug_printf(9, "gw_rebuild_read_from_net:FileId:%s, buf len is %d, iseof is %d\n", data->file_id, len, iseof);
    if (!data)
        return CI_ERROR;

    if (data->args.sizelimit
        && gw_body_data_size(&data->body) >= data->max_object_size) {
        ci_debug_printf(2, "Object bigger than max scanable file:FileId:%s\n", data->file_id);

    /*TODO: Raise an error report rather than just raise an error */
    return CI_ERROR;
    } 
    ci_debug_printf(9, "gw_rebuild_read_from_net:FileId:%s, Writing to data->body, %d bytes \n", data->file_id, len);

    return gw_body_data_write(&data->body, buf, len, iseof);
}

static int gw_rebuild_io(char *wbuf, int *wlen, char *rbuf, int *rlen, int iseof, ci_request_t *req)
{
    char printBuffer[100];
    char tempBuffer[20];
    printBuffer[0] = '\0';
    strcat(printBuffer, "gw_rebuild_io, ");

    if (wlen) {
        snprintf(tempBuffer, sizeof(tempBuffer), "wlen=%d, ", *wlen);
        strcat(printBuffer, tempBuffer);
    }
    if (rlen) {
        snprintf(tempBuffer, sizeof(tempBuffer), "rlen=%d, ", *rlen);
        strcat(printBuffer, tempBuffer);
    }
    snprintf(tempBuffer, sizeof(tempBuffer), "iseof=%d\n", iseof);
    strcat(printBuffer, tempBuffer);
    ci_debug_printf(9, "%s", printBuffer);

     if (rbuf && rlen) {
        *rlen = gw_rebuild_read_from_net(rbuf, *rlen, iseof, req);
        if (*rlen == CI_ERROR)
            return CI_ERROR;
            /*else if (*rlen < 0) ignore*/
     }
     else if (iseof && gw_rebuild_read_from_net(NULL, 0, iseof, req) == CI_ERROR){
         return CI_ERROR;
     }

     if (wbuf && wlen) {
          *wlen = gw_rebuild_write_to_net(wbuf, *wlen, req);
     }
     return CI_OK;
}

static int rebuild_request_body(ci_request_t *req, gw_rebuild_req_data_t* data, ci_simple_file_t* input, ci_simple_file_t* output);
static void add_file_id_header(ci_request_t *req, const char* header_key, unsigned char* file_id);
static int gw_rebuild_end_of_data_handler(ci_request_t *req)
{
    gw_rebuild_req_data_t *data = ci_service_data(req);
    ci_debug_printf(3, "gw_rebuild_end_of_data_handler:FileId:%s\n", data->file_id);
    
    if (!data){
        data->gw_processing = GW_PROCESSING_NONE;
        ci_stat_uint64_inc(GW_UNPROCESSABLE, 1);                 
        return CI_MOD_DONE;
    }

    int rebuild_status = REBUILD_ERROR;
    rebuild_status = rebuild_request_body(req, data, data->body.store, data->body.rebuild);

    if (rebuild_status == REBUILD_FAILED){
        ci_debug_printf(3, "gw_rebuild_end_of_data_handler:FileId:%s, REBUILD_FAILED\n", data->file_id);
        generate_403_headers(req);
    } else if (rebuild_status == REBUILD_ERROR){
        int error_report_size;
        ci_debug_printf(3, "gw_rebuild_end_of_data_handler:FileId:%s, REBUILD_ERROR\n", data->file_id);
        generate_403_headers(req);
        generate_error_page(data, req);               
        error_report_size = ci_membuf_size(data->error_page);
   
        gw_body_data_destroy(&data->body);
        gw_body_data_new(&data->body, error_report_size);
        gw_body_data_write(&data->body, data->error_page->buf, error_report_size, 1);
        rebuild_content_length(req, &data->body);
    }
    
    if (REPORT_FILE_ID == GW_ENABLE_FILE_ID_REPORTING){
      add_file_id_header(req, "X-Adaptation-File-Id", data->file_id);
    }

    ci_debug_printf(3, "gw_rebuild_end_of_data_handler:FileId:%s, allow204(%d)\n", data->file_id, data->allow204);
    if (data->allow204 && rebuild_status == REBUILD_UNPROCESSED){
        ci_debug_printf(3, "gw_rebuild_end_of_data_handler:FileId:%s, returning %d\n",  data->file_id, rebuild_status);
        return CI_MOD_ALLOW204;
    }
 
    ci_req_unlock_data(req);
    gw_body_data_unlock_all(&data->body);

    return CI_MOD_DONE;
}

static int call_proxy_application(const unsigned char* file_id, const ci_simple_file_t* input, const ci_simple_file_t* output);
static int process_output_file(ci_request_t *req, gw_rebuild_req_data_t* data, ci_simple_file_t* output);
static int replace_request_body(gw_rebuild_req_data_t* data, ci_simple_file_t* rebuild);
static int refresh_externally_updated_file(ci_simple_file_t* updated_file);
/* Return value:  */
/* REBUILD_UNPROCESSED - to continue to unchanged content */
/* REBUILD_REBUILT - to continue to rebuilt content */
/* REBUILD_FAILED - to report error and use supplied error report */
/* REBUILD_ERROR - to report  processing error */
int rebuild_request_body(ci_request_t *req, gw_rebuild_req_data_t* data, ci_simple_file_t* input, ci_simple_file_t* output)
{
    ci_stat_uint64_inc(GW_SCAN_REQS, 1);    
    ci_stat_kbs_inc(GW_SCAN_BYTES, (int)gw_body_data_size(&data->body));
    int gw_proxy_api_return = call_proxy_application(data->file_id, input, output);
    
    /* Store the return status for inclusion in any error report */
    data->gw_status = gw_proxy_api_return;
    
    int rebuild_status = REBUILD_ERROR;
    int outfile_status = CI_ERROR;
    switch (gw_proxy_api_return)
    {
        case GW_FAILED:
            ci_debug_printf(3, "rebuild_request_body GW_FAILED:FileId:%s\n", data->file_id);
            outfile_status = process_output_file(req, data, output);

            if (outfile_status == CI_OK){
                ci_stat_uint64_inc(GW_REBUILD_FAILURES, 1); 
                rebuild_status =  REBUILD_FAILED;
            } else {
                ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
            }
            break;
        case GW_ERROR:
            ci_debug_printf(3, "rebuild_request_body GW_ERROR:FileId:%s\n", data->file_id);
            ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
            break;
        case GW_UNPROCESSED:
            ci_debug_printf(3, "rebuild_request_body GW_UNPROCESSED:FileId:%s\n", data->file_id);
            ci_stat_uint64_inc(GW_NOT_PROCESSED, 1); 
            rebuild_status = REBUILD_UNPROCESSED;
            break;
        case GW_REBUILT:
            ci_debug_printf(3, "rebuild_request_body GW_REBUILT:FileId:%s\n", data->file_id);
            outfile_status = process_output_file(req, data, output);

            if (outfile_status == CI_OK){
                ci_stat_uint64_inc(GW_REBUILD_SUCCESSES, 1); 
                rebuild_status = REBUILD_REBUILT;    
            } else {
                ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
            }
            break;  
        default:
            ci_debug_printf(3, "Unrecognised Proxy API return value (%d):FileId:%s\n", gw_proxy_api_return, data->file_id);
            ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
    }
    return rebuild_status;    
}

static int process_output_file(ci_request_t *req, gw_rebuild_req_data_t* data, ci_simple_file_t* output)
{
    if (refresh_externally_updated_file(output) == CI_ERROR){
        ci_debug_printf(3, "Problem sizing replacement content:FileId:%s\n", data->file_id);
        ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
        return CI_ERROR;
    } 
    if (ci_simple_file_size(output) == 0){
        ci_debug_printf(3, "No replacement content available:FileId:%s\n", data->file_id);
        ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
        return CI_ERROR;
    }
    if (!replace_request_body(data, output)){
        ci_debug_printf(3, "Error replacing request body:FileId:%s\n", data->file_id);
        ci_stat_uint64_inc(GW_REBUILD_ERRORS, 1); 
        return CI_ERROR;
    }      
    rebuild_content_length(req, &data->body);  
    return CI_OK;
 }

int replace_request_body(gw_rebuild_req_data_t* data, ci_simple_file_t* rebuild)
{
    ci_simple_file_destroy(data->body.store);
    data->body.store = rebuild;        
    return CI_OK;
}

/*******************************************************************************/
/* Other  functions                                                            */

static void cmd_reload_istag(const char *name, int type, void *data)
{
    if (gw_rebuild_xdata)
        set_istag(gw_rebuild_xdata);
}

void set_istag(ci_service_xdata_t *srv_xdata)
{
    ci_debug_printf(9, "Updating istag %s with %s\n", srv_xdata->ISTag, REBUILD_VERSION);
    char istag[SERVICE_ISTAG_SIZE + 1];
    
    istag[0] = '-';
    strncpy(istag + 1, REBUILD_VERSION, strlen(REBUILD_VERSION));
    ci_service_set_istag(srv_xdata, istag);
}

static int init_body_data(ci_request_t *req)
{
    gw_rebuild_req_data_t *data = ci_service_data(req);
    assert(data);

    gw_body_data_new(&(data->body), data->args.sizelimit==0 ? 0 : data->max_object_size);
        /*Icap server can not send data at the begining.
        The following call does not needed because the c-icap
        does not send any data if the ci_req_unlock_data is not called:*/
        /* ci_req_lock_data(req);*/

        /* Let ci_simple_file api to control the percentage of data.
         For now no data can send */
    gw_body_data_lock_all(&(data->body));

    return CI_OK;
}

static void generate_403_headers(ci_request_t *req)
{
    if ( ci_http_response_headers(req))
         ci_http_response_reset_headers(req);
    else
         ci_http_response_create(req, 1, 1);
    ci_http_response_add_header(req, "HTTP/1.0 403 Forbidden");
    ci_http_response_add_header(req, "Server: C-ICAP");
    ci_http_response_add_header(req, "Connection: close");
    ci_http_response_add_header(req, "Content-Type: text/html");
}

static void generate_error_page(gw_rebuild_req_data_t *data, ci_request_t *req)
{
    ci_membuf_t *error_page;
    char buf[1024];
    const char *lang;

    error_page = ci_txt_template_build_content(req, "gw_rebuild", "POLICY_ISSUE",
                           gw_rebuild_report_format_table);

    lang = ci_membuf_attr_get(error_page, "lang");
    if (lang) {
        snprintf(buf, sizeof(buf), "Content-Language: %s", lang);
        buf[sizeof(buf)-1] = '\0';
        ci_http_response_add_header(req, buf);
    }
    else
        ci_http_response_add_header(req, "Content-Language: en");

    data->error_page = error_page;
}

/***************************************************************************************/
/* Parse arguments function -
   Current arguments: allow204=on|off, sizelimit=off
*/
void gw_rebuild_parse_args(gw_rebuild_req_data_t *data, char *args)
{
     char *str;
     if ((str = strstr(args, "allow204="))) {
          if (strncmp(str + 9, "on", 2) == 0)
               data->args.enable204 = 1;
          else if (strncmp(str + 9, "off", 3) == 0)
               data->args.enable204 = 0;
     }

     if ((str = strstr(args, "sizelimit="))) {
          if (strncmp(str + 10, "off", 3) == 0)
               data->args.sizelimit = 0;
     }

}

static int exec_prog(const char **argv);
/* Return value: exit status from executed application (gw_proxy_api_return), or GW_ERROR */
static int call_proxy_application(const unsigned char* file_id, const ci_simple_file_t* input, const ci_simple_file_t* output)
{     
    const char* args[8] = {PROXY_APP_LOCATION, 
                           "-f", file_id,
                           "-i", input->filename, 
                           "-o", output->filename, 
                           NULL};
    return exec_prog(args);  
}

/* First array item is path to executable, last array item is null. Program arguments are intermediate array elements*/
/* Return value: exit status from executed application (gw_proxy_api_return), or GW_ERROR */
static int exec_prog(const char **argv)
{
    pid_t   my_pid;
    int     status, timeout;

    if (0 == (my_pid = fork())) {
        if (-1 == execvp(argv[0], (char **)argv)) {
            ci_debug_printf(1, "child process execve failed for %s (%d)", argv[0], my_pid);
            return GW_ERROR;
        }
    }
    timeout = 1000;

    while (0 == waitpid(my_pid , &status , WNOHANG)) {
        if ( --timeout < 0 ) {
            ci_debug_printf(1, "Unexpected timeout running Proxy application (%d)\n", my_pid);
            return GW_ERROR;
        }
        sleep(1);
    }

    ci_debug_printf(8, "%s PID %d WEXITSTATUS %d WIFEXITED %d [status %d]\n",
            argv[0], my_pid, WEXITSTATUS(status), WIFEXITED(status), status);
            
    if (WIFEXITED(status) ==0)
    {
        ci_debug_printf(1, "Unexpected error running Proxy application (%d)\n", status);
        return GW_ERROR;
    }

    return WEXITSTATUS(status);
}

void rebuild_content_length(ci_request_t *req, gw_body_data_t *bd)
{
    ci_off_t new_file_size = 0;
    char buf[256];
    ci_simple_file_t *body = NULL;

    body = bd->store;
    assert(body->readpos == 0);
    new_file_size = body->endpos;

    ci_debug_printf(5, "Body data size changed to new size %"  PRINTF_OFF_T "\n",
                    (CAST_OFF_T)new_file_size);

    snprintf(buf, sizeof(buf), "Content-Length: %" PRINTF_OFF_T, (CAST_OFF_T)new_file_size);
    int remove_status = 0;
    if (req->type == ICAP_REQMOD){
        remove_status = ci_http_request_remove_header(req, "Content-Length");
        ci_http_request_add_header(req, buf);
        ci_debug_printf(5, "Request Header updated(%d), %s\n", remove_status, buf);        
    }
    else if (req->type == ICAP_RESPMOD){
        remove_status = ci_http_response_remove_header(req, "Content-Length");
        ci_http_response_add_header(req, buf);
        ci_debug_printf(5, "Response Header updated(%d), %s\n", remove_status, buf);        
    }   
}

static int file_size(int fd)
{
   struct stat s;
   if (fstat(fd, &s) == -1) {
      return(-1);
   }
   return(s.st_size);
}

static int refresh_externally_updated_file(ci_simple_file_t* updated_file)
{
    ci_off_t new_size;
    ci_simple_file_write(updated_file, NULL, 0, 1);  /* to close of the file have been modified externally */
       
    new_size = file_size(updated_file->fd);
    if (new_size < 0)
        return CI_ERROR;
    
    updated_file->endpos= new_size;
    updated_file->readpos=0;
    return CI_OK;
}

static void add_file_id_header(ci_request_t *req, const char* header_key, unsigned char* file_id)
{
  char buf[256];
  snprintf(buf, sizeof(buf), "%s: %s", header_key, file_id);
  if (req->type == ICAP_REQMOD){
    ci_http_request_add_header(req, buf);
    ci_debug_printf(5, "Request Header updated, %s\n", buf);        
  }
  else if (req->type == ICAP_RESPMOD){
     ci_http_response_add_header(req, buf);
     ci_debug_printf(5, "Response Header updated, %s\n", buf);        
  }
}

/**************************************************************/
/* gw_rebuild templates  formating table                         */

static int fmt_gw_rebuild_fileid(ci_request_t *req, char *buf, int len, const char *param)
{
    gw_rebuild_req_data_t *data = ci_service_data(req);
    return snprintf(buf, len, "%s", data->file_id);
}

char* concat(char* output, const char* s1, const char* s2)
{
    strcpy(output, s1);
    strcat(output, s2);
    return output;
}

static int file_exists (char *filename) {
  struct stat   buffer;   
  return (stat (filename, &buffer) == 0);
}