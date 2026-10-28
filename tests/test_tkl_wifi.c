#include "tkl_wifi.h"
#include "vw553_device.h"
#include "tuya_error_code.h"
#include "task.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static vw553_device_event_handler_t s_event_handler;
static void *s_event_context;
static const char *s_response;
static const char *s_station_ip_response = "+CIPSTA: \"0.0.0.0\"\r\n+CIPSTA: \"0.0.0.0\"\r\n+CIPSTA: \"0.0.0.0\"\r\nOK\r\n";
static char s_last_command[256];
static char s_ap_command[256];
static unsigned s_execute_count;
static unsigned s_cwjap_count;
static unsigned s_ps_off_count;
static unsigned s_ps_on_count;
static unsigned s_last_ps_off_index;
static unsigned s_last_ps_on_index;
static unsigned s_last_cwjap_index;
static int32_t s_execute_status = OPRT_OK;
static vw553_at_result_t s_execute_result = VW553_AT_RESULT_OK;
static unsigned s_ip_queries;
static unsigned s_ip_valid_after = UINT32_MAX;
static TickType_t s_ticks;
static bool s_saw_ap_config;
static bool s_saw_cwqap;
static bool s_saw_ap_after_cwqap;
static bool s_saw_ap_mode;
static bool s_saw_ap_ip_config;
static WF_EVENT_E s_last_event;
static unsigned s_event_count;
static bool s_ap_teardown_flag;
static unsigned s_ap_teardown_true_count;

int32_t vw553_device_subscribe(vw553_device_event_handler_t handler, void *context)
{
    s_event_handler = handler;
    s_event_context = context;
    return OPRT_OK;
}

bool vw553_socket_backend_has_active_socket(void) { return false; }
void vw553_socket_ap_teardown_set(bool active)
{
    s_ap_teardown_flag = active;
    if (active) ++s_ap_teardown_true_count;
}
void vw553_socket_ap_listeners_invalidate(bool invalid) { (void)invalid; }
void vw553_socket_wifi_join_set(bool active) { (void)active; }
void vw553_socket_dns_cache_flush(void) {}

int32_t vw553_device_negotiate(uint32_t timeout_ms)
{
    (void)timeout_ms;
    return OPRT_OK;
}

int32_t vw553_device_execute(const vw553_uart_request_t *request, vw553_at_result_t *result)
{
    assert(request && request->command && request->reply && result);
    snprintf(s_last_command, sizeof(s_last_command), "%s", request->command);
    ++s_execute_count;
    if (strncmp(request->command, "AT+TY553XPS=", sizeof("AT+TY553XPS=") - 1U) == 0) {
        if (strcmp(request->command, "AT+TY553XPS=0,0") == 0) {
            ++s_ps_off_count;
            s_last_ps_off_index = s_execute_count;
        } else if (strcmp(request->command, "AT+TY553XPS=2,3") == 0 ||
                   strcmp(request->command, "AT+TY553XPS=2,4") == 0) {
            ++s_ps_on_count;
            s_last_ps_on_index = s_execute_count;
        }
        const char ok[] = "OK\r\n";
        memcpy(request->reply, ok, sizeof(ok));
        if (request->reply_length) *request->reply_length = sizeof(ok) - 1U;
        *result = VW553_AT_RESULT_OK;
        return OPRT_OK;
    }
    if (strncmp(request->command, "AT+CWJAP_CUR=", sizeof("AT+CWJAP_CUR=") - 1U) == 0) {
        ++s_cwjap_count;
        s_last_cwjap_index = s_execute_count;
    }
    if (s_execute_status != OPRT_OK) {
        *result = s_execute_result;
        return s_execute_status;
    }
    if (strcmp(request->command, "AT+CIPSTA?") == 0) {
        ++s_ip_queries;
        s_response = s_ip_queries >= s_ip_valid_after
                         ? "+CIPSTA: \"192.168.1.40\"\r\n+CIPSTA: \"255.255.255.0\"\r\n+CIPSTA: \"192.168.1.1\"\r\nOK\r\n"
                         : s_station_ip_response;
    }
    if (strncmp(request->command, "AT+CWSAP_CUR=", sizeof("AT+CWSAP_CUR=") - 1U) == 0) {
        s_saw_ap_config = true;
        if (s_saw_cwqap) s_saw_ap_after_cwqap = true;
        snprintf(s_ap_command, sizeof(s_ap_command), "%s", request->command);
    }
    if (strcmp(request->command, "AT+CWQAP") == 0) s_saw_cwqap = true;
    if (strcmp(request->command, "AT+CWMODE_CUR=2") == 0) s_saw_ap_mode = true;
    if (strncmp(request->command, "AT+CIPAP=", sizeof("AT+CIPAP=") - 1U) == 0) s_saw_ap_ip_config = true;
    const char *response = s_response ? s_response : "OK\r\n";
    size_t length = strlen(response);
    if (length + 1U > request->reply_capacity) return OPRT_BUFFER_NOT_ENOUGH;
    memcpy(request->reply, response, length + 1U);
    if (request->reply_length) *request->reply_length = length;
    *result = VW553_AT_RESULT_OK;
    if (strcmp(request->command, "AT+CWJAP_CUR=\"router\",\"12345678\"") == 0 && s_event_handler) {
        static const char connected[] = "WIFI CONNECTED";
        const vw553_device_event_t event = {.kind = VW553_DEVICE_EVENT_LINE,
                                             .value.line = {connected, sizeof(connected) - 1U}};
        s_event_handler(s_event_context, &event);
    }
    return OPRT_OK;
}

void *tal_malloc(size_t size) { return malloc(size); }
void tal_free(void *ptr) { free(ptr); }
BaseType_t xTaskCreate(void (*entry)(void *), const char *name, unsigned int stack, void *argument,
                       unsigned int priority, TaskHandle_t *handle)
{
    (void)entry; (void)name; (void)stack; (void)argument; (void)priority; (void)handle;
    return pdPASS;
}
void vTaskDelay(TickType_t ticks) { s_ticks += ticks; }
TickType_t xTaskGetTickCount(void) { return s_ticks; }

static void wifi_event(WF_EVENT_E event, void *arg)
{
    (void)arg;
    s_last_event = event;
    ++s_event_count;
}

int main(void)
{
    assert(tkl_wifi_init(wifi_event) == OPRT_OK);
    assert(strcmp(s_last_command, "AT+TY553XPS=0,0") == 0);
    assert(s_ps_off_count == 1U);
    assert(tkl_wifi_set_lp_mode(TRUE, 0) == OPRT_INVALID_PARM);
    assert(tkl_wifi_set_lp_mode(TRUE, 11) == OPRT_INVALID_PARM);
    unsigned before_policy = s_execute_count;
    assert(tkl_wifi_set_lp_mode(TRUE, 3) == OPRT_OK);
    assert(s_execute_count == before_policy);

    /* netconn_wifi_open() treats any error as fatal, so the no-op country
     * code path must report success or the saved netinfo is never loaded. */
    assert(tkl_wifi_set_country_code(COUNTRY_CODE_CN) == OPRT_OK);
    assert(tkl_wifi_set_country_code(COUNTRY_CODE_EU) == OPRT_OK);
    assert(s_execute_count == before_policy);

    NW_MAC_S mac = {0};
    s_response = "+CIFSR:STAMAC,02:11:22:33:44:55\r\nOK\r\n";
    assert(tkl_wifi_get_mac(WF_AP, &mac) == OPRT_OK);
    assert(mac.mac[0] == 0x02 && mac.mac[5] == 0x55);

    WF_AP_CFG_IF_S ap = {0};
    memcpy(ap.ssid, "Tuya-setup", sizeof("Tuya-setup") - 1U);
    ap.s_len = (uint8_t)(sizeof("Tuya-setup") - 1U);
    ap.md = WAAM_OPEN;
    ap.chan = 6;
    ap.ip.dhcpen = TRUE;
    strcpy(ap.ip.ip, "192.168.176.1");
    strcpy(ap.ip.gw, "192.168.176.1");
    strcpy(ap.ip.mask, "255.255.255.0");
    s_response = "OK\r\n";
    assert(tkl_wifi_start_ap(&ap) == OPRT_OK);
    assert(!s_ap_teardown_flag);
    /* Match the board-verified AP sequence: CWSAP_CUR must run directly.
     * Sending CWMODE_CUR=2 first makes this VW553 image reject CWSAP_CUR. */
    assert(!s_saw_ap_mode && s_saw_ap_config && s_saw_ap_ip_config);
    assert(strcmp(s_ap_command, "AT+CWSAP_CUR=\"Tuya-setup\",\"\",6,0") == 0);
    assert(strcmp(s_last_command, "AT+CIPAP=\"192.168.176.1\",\"192.168.176.1\",\"255.255.255.0\"") == 0);

    /* The vendor's CIPAP? can transiently return ERROR after SoftAP starts.
     * The IP we successfully configured above is authoritative for WF_AP. */
    NW_IP_S cached_ap_ip = {0};
    unsigned before_cached_get = s_execute_count;
    s_execute_status = OPRT_COM_ERROR;
    s_execute_result = VW553_AT_RESULT_ERROR;
    assert(tkl_wifi_get_ip(WF_AP, &cached_ap_ip) == OPRT_OK);
    assert(s_execute_count == before_cached_get);
    assert(strcmp(cached_ap_ip.ip, "192.168.176.1") == 0);
    assert(strcmp(cached_ap_ip.gw, "192.168.176.1") == 0);
    assert(strcmp(cached_ap_ip.mask, "255.255.255.0") == 0);
    assert(cached_ap_ip.dhcpen == TRUE);

    /* A VW553 reset invalidates the cache, so the next read must query again. */
    const vw553_device_event_t reset_event = {.kind = VW553_DEVICE_EVENT_RESET};
    s_event_handler(s_event_context, &reset_event);
    s_execute_status = OPRT_OK;
    s_execute_result = VW553_AT_RESULT_OK;
    s_response = "+CIPAP:ip:\"192.168.176.1\"\r\n"
                 "+CIPAP:gateway:\"192.168.176.1\"\r\n"
                 "+CIPAP:netmask:\"255.255.255.0\"\r\nOK\r\n";
    unsigned before_query_get = s_execute_count;
    memset(&cached_ap_ip, 0, sizeof(cached_ap_ip));
    assert(tkl_wifi_get_ip(WF_AP, &cached_ap_ip) == OPRT_OK);
    assert(s_execute_count == before_query_get + 1U);
    assert(strcmp(cached_ap_ip.ip, "192.168.176.1") == 0);

    NW_IP_S static_ip = {0};
    strcpy(static_ip.ip, "192.168.1.9");
    strcpy(static_ip.mask, "255.255.255.0");
    strcpy(static_ip.gw, "192.168.1.1");
    assert(tkl_wifi_set_ip(WF_STATION, &static_ip) == OPRT_OK);
    assert(strcmp(s_last_command, "AT+CIPSTA=\"192.168.1.9\",\"255.255.255.0\",\"192.168.1.1\"") == 0);
    assert(tkl_wifi_set_ip(WF_AP, &static_ip) == OPRT_OK);
    assert(strcmp(s_last_command, "AT+CIPAP=\"192.168.1.9\",\"192.168.1.1\",\"255.255.255.0\"") == 0);

    uint8_t channel = 0U;
    s_response = "+CWSTATUS: STA, connected, Cafe, North, 11, 02:11:22:33:44:66\r\nOK\r\n";
    assert(tkl_wifi_get_cur_channel(&channel) == OPRT_OK && channel == 11U);
    s_response = "+CWSTATUS: SoftAP, Tuya setup, private pass, 6\r\nOK\r\n";
    assert(tkl_wifi_get_cur_channel(&channel) == OPRT_OK && channel == 6U);

    AP_IF_S *aps = NULL;
    uint32_t count = 0;
    s_response = "+CWLAP: Cafe,North, -55, 02:11:22:33:44:66, 6, WPA2 AES\r\nOK\r\n";
    assert(tkl_wifi_scan_ap(NULL, &aps, &count) == OPRT_OK);
    assert(count == 1U && strcmp((const char *)aps[0].ssid, "Cafe,North") == 0);
    assert(aps[0].rssi == -55 && aps[0].channel == 6U && aps[0].security == WAAM_WPA2_PSK);
    assert(tkl_wifi_release_ap(aps) == OPRT_OK);

    unsigned before = s_execute_count;
    assert(tkl_wifi_station_connect((const int8_t *)"comma,ssid", (const int8_t *)"12345678") == OPRT_INVALID_PARM);
    assert(tkl_wifi_station_connect((const int8_t *)"router", (const int8_t *)"comma,password") == OPRT_INVALID_PARM);
    assert(tkl_wifi_scan_ap((const int8_t *)"comma,ssid", &aps, &count) == OPRT_INVALID_PARM);
    WF_AP_CFG_IF_S comma_ap = ap;
    memcpy(comma_ap.ssid, "comma,ssid", sizeof("comma,ssid") - 1U);
    comma_ap.s_len = (uint8_t)(sizeof("comma,ssid") - 1U);
    assert(tkl_wifi_start_ap(&comma_ap) == OPRT_INVALID_PARM);
    WF_AP_CFG_IF_S comma_password_ap = ap;
    memcpy(comma_password_ap.passwd, "comma,pass", sizeof("comma,pass") - 1U);
    comma_password_ap.p_len = (uint8_t)(sizeof("comma,pass") - 1U);
    comma_password_ap.md = WAAM_WPA_WPA3_SAE;
    assert(tkl_wifi_start_ap(&comma_password_ap) == OPRT_INVALID_PARM);
    assert(tkl_wifi_station_connect((const int8_t *)"router\"\rAT+ERASE", (const int8_t *)"12345678") ==
           OPRT_INVALID_PARM);
    assert(s_execute_count == before);

    s_event_count = 0U;
    s_cwjap_count = 0U;
    s_execute_status = OPRT_COM_ERROR;
    s_execute_result = VW553_AT_RESULT_ERROR;
    assert(tkl_wifi_station_connect((const int8_t *)"router", (const int8_t *)"12345678") == OPRT_COM_ERROR);
    assert(s_cwjap_count == 1U);
    assert(s_last_ps_off_index < s_last_cwjap_index);
    assert(s_event_count == 1U && s_last_event == WFE_CONNECT_FAILED);
    s_execute_status = OPRT_OK;
    s_execute_result = VW553_AT_RESULT_OK;

    s_event_count = 0U;
    s_ip_queries = 0U;
    s_ip_valid_after = 3U;
    s_response = "WIFI CONNECTED\r\nOK\r\n";
    assert(tkl_wifi_station_connect((const int8_t *)"router", (const int8_t *)"12345678") == OPRT_OK);
    assert(s_ap_teardown_true_count > 0U && !s_ap_teardown_flag);
    assert(s_event_count == 1U && s_last_event == WFE_CONNECTED);
    assert(s_last_ps_off_index < s_last_cwjap_index && s_last_cwjap_index < s_last_ps_on_index);
    assert(strcmp(s_last_command, "AT+TY553XPS=2,3") == 0);
    assert(s_ps_on_count == 1U);

    before_policy = s_execute_count;
    assert(tkl_wifi_set_lp_mode(TRUE, 4) == OPRT_OK);
    assert(s_execute_count == before_policy + 1U);
    assert(strcmp(s_last_command, "AT+TY553XPS=2,4") == 0);
    assert(s_ps_on_count == 2U);
    assert(tkl_wifi_set_lp_mode(FALSE, 0) == OPRT_OK);
    assert(strcmp(s_last_command, "AT+TY553XPS=0,0") == 0);

    s_event_count = 0U;
    s_ip_queries = 0U;
    s_ip_valid_after = UINT32_MAX;
    assert(tkl_wifi_station_connect((const int8_t *)"router", (const int8_t *)"12345678") == OPRT_TIMEOUT);
    assert(s_event_count == 1U && s_last_event == WFE_CONNECT_FAILED);

    s_response = "+CWLAP: malformed\r\nOK\r\n";
    assert(tkl_wifi_scan_ap(NULL, &aps, &count) == OPRT_COM_ERROR);
    assert(aps == NULL && count == 0U);

    /* A reset can restart AP provisioning right after CWJAP succeeded.  VW553
     * refuses CWSAP_CUR while the station is associated, so disconnect first. */
    s_event_count = 0U;
    s_ip_queries = 0U;
    s_ip_valid_after = 0U;
    s_response = "WIFI CONNECTED\r\nOK\r\n";
    assert(tkl_wifi_station_connect((const int8_t *)"router", (const int8_t *)"12345678") == OPRT_OK);
    s_saw_cwqap = false;
    s_saw_ap_after_cwqap = false;
    s_response = "OK\r\n";
    assert(tkl_wifi_start_ap(&ap) == OPRT_OK);
    assert(s_saw_cwqap && s_saw_ap_after_cwqap);
    puts("VW553 TKL Wi-Fi host tests passed");
    return 0;
}
