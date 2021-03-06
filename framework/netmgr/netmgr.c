/*
 * Copyright (C) 2015-2017 Alibaba Group Holding Limited
 */

#include <stdio.h>
#include <string.h>
#include <stdbool.h>
#include <aos/aos.h>
#include <aos/network.h>
#include <hal/hal.h>

#include "netmgr.h"

#ifdef CONFIG_AOS_MESH
#include "umesh.h"
#endif

#define TAG "netmgr"

#ifndef WIFI_SSID
#define DEMO_AP_SSID "cisco-15A7"
#define DEMO_AP_PASSWORD "12345678"
#else
#define DEMO_AP_SSID WIFI_SSID
#define DEMO_AP_PASSWORD WIFI_PWD
#endif

#define MAX_RETRY_CONNECT 120
#define RETRY_INTERVAL_MS 500

typedef struct {
    char ssid[33];
    char pwd[65];
#ifdef STM32L475xx
    char security[MAX_SECURITY_SIZE + 1];
#endif
} wifi_conf_t;

typedef struct {
    wifi_conf_t                saved_conf;
    netmgr_ap_config_t         ap_config;
    hal_wifi_module_t          *wifi_hal_mod;
    autoconfig_plugin_t        *autoconfig_chain;
    int32_t                    ipv4_owned;
    int8_t                     disconnected_times;
    bool                       doing_smartconfig;
    bool                       ip_available;
    netmgr_wifi_scan_result_cb_t cb;
    bool                       wifi_scan_complete_cb_finished;
} netmgr_cxt_t;

extern autoconfig_plugin_t g_alink_smartconfig;

static netmgr_cxt_t        g_netmgr_cxt;
static autoconfig_plugin_t g_def_smartconfig;

static void netmgr_wifi_config_start(void);
static void add_autoconfig_plugin(autoconfig_plugin_t *plugin);
static int32_t has_valid_ap(void);

static void format_ip(uint32_t ip, char *buf)
{
    int i = 0;

    unsigned char octet[4]  = { 0, 0, 0, 0 };

    for (i = 0; i < 4; i++) {
        octet[i] = ( ip >> ((3 - i) * 8) ) & 0xFF;
    }

    sprintf(buf, "%d.%d.%d.%d", octet[3], octet[2], octet[1], octet[0]);
}

static void netmgr_connect_fail_event(hal_wifi_module_t *m, int err, void *arg)
{

}

#ifdef CONFIG_AOS_MESH
static void mesh_delayed_action(void *arg)
{
    umesh_set_mode((node_mode_t)arg);
    umesh_stop();
    umesh_start();
}
#endif

static void start_mesh(bool is_leader)
{
#ifdef CONFIG_AOS_MESH
    node_mode_t mode;

    mode = umesh_get_mode() & (~MODE_LEADER);
    if (is_leader) {
        mode |= MODE_LEADER;
    }

    aos_post_delayed_action(1000, mesh_delayed_action, (void *)(long)mode);
#endif
}

static void stop_mesh(void)
{
#ifdef CONFIG_AOS_MESH
    umesh_stop();
#endif
}

static int32_t translate_addr(char *str)
{
    int32_t a, b, c, d;
    int32_t address = 0;
    sscanf(str, "%d.%d.%d.%d", &a, &b, &c, &d);
    address |= d << 24;
    address |= c << 16;
    address |= b << 8;
    address |= a;

    return address;
}

static void netmgr_ip_got_event(hal_wifi_module_t *m,
                                hal_wifi_ip_stat_t *pnet, void *arg)
{
    LOGI(TAG, "Got ip : %s, gw : %s, mask : %s", pnet->ip, pnet->gate, pnet->mask);

    g_netmgr_cxt.ipv4_owned = translate_addr(pnet->ip);
    g_netmgr_cxt.ip_available = true;
    aos_post_event(EV_WIFI, CODE_WIFI_ON_PRE_GOT_IP, 0u);
    start_mesh(true);
}

static void netmgr_stat_chg_event(hal_wifi_module_t *m, hal_wifi_event_t stat,
                                  void *arg)
{
    switch (stat) {
        case NOTIFY_STATION_UP:
            aos_post_event(EV_WIFI, CODE_WIFI_ON_CONNECTED,
                           (unsigned long)g_netmgr_cxt.ap_config.ssid);
            break;
        case NOTIFY_STATION_DOWN:
            aos_post_event(EV_WIFI, CODE_WIFI_ON_DISCONNECT, 0u);
            break;
        case NOTIFY_AP_UP:
            break;
        case NOTIFY_AP_DOWN:
            break;
        default:
            break;
    }
}

static void get_bssid(uint8_t *to_fill, int size)
{
    memset(to_fill, 0, size);
}

static void netmgr_scan_completed_event(hal_wifi_module_t *m,
                                        hal_wifi_scan_result_t *result,
                                        void *arg)
{
    netmgr_wifi_scan_result_cb_t cb = g_netmgr_cxt.cb;
    int i, last_ap = 0;
    uint8_t bssid[ETH_ALEN];

    if (g_netmgr_cxt.cb) {
        for (i = 0; i < (result->ap_num); i++) {
            LOGD("netmgr", "AP to add: %s", result->ap_list[i].ssid);
            if (i == (result->ap_num - 1)) {
                last_ap = 1;
            }
            get_bssid(bssid, ETH_ALEN);
            cb(result->ap_list[i].ssid, bssid, NETMGR_AWSS_AUTH_TYPE_WPA2PSK,
               NETMGR_AWSS_ENC_TYPE_NONE, 0, 0, last_ap);
        }
        g_netmgr_cxt.wifi_scan_complete_cb_finished = true;
    }
}

static void netmgr_scan_adv_completed_event(hal_wifi_module_t *m,
                                            hal_wifi_scan_result_adv_t *result,
                                            void *arg)
{
    netmgr_wifi_scan_result_cb_t cb = g_netmgr_cxt.cb;
    int i, last_ap = 0;

    if (g_netmgr_cxt.cb) {
        for (i = 0; i < (result->ap_num); i++) {
            LOGD("netmgr", "AP to add: %s", result->ap_list[i].ssid);
            if (i == (result->ap_num - 1)) {
                last_ap = 1;
            }
            cb(result->ap_list[i].ssid, (const uint8_t *)result->ap_list[i].bssid,
               result->ap_list[i].security, NETMGR_AWSS_ENC_TYPE_NONE,
               result->ap_list[i].channel, result->ap_list[i].ap_power, last_ap);
        }
        g_netmgr_cxt.wifi_scan_complete_cb_finished = true;
    }
}

static void netmgr_para_chg_event(hal_wifi_module_t *m,
                                  hal_wifi_ap_info_adv_t *ap_info,
                                  char *key, int key_len, void *arg)
{
}

static void netmgr_fatal_err_event(hal_wifi_module_t *m, void *arg)
{
}

static const hal_wifi_event_cb_t g_wifi_hal_event = {
    .connect_fail        = netmgr_connect_fail_event,
    .ip_got              = netmgr_ip_got_event,
    .stat_chg            = netmgr_stat_chg_event,
    .scan_compeleted     = netmgr_scan_completed_event,
    .scan_adv_compeleted = netmgr_scan_adv_completed_event,
    .para_chg            = netmgr_para_chg_event,
    .fatal_err           = netmgr_fatal_err_event,
};

#ifdef STM32L475xx
static void set_access_security(hal_wifi_init_type_t *wifi_type, char *security)
{
    if ( strcmp( security, "open" ) == 0 ) {
        wifi_type->access_sec = WIFI_ECN_OPEN;
    } else if ( strcmp( security, "wep" ) == 0 ) {
        wifi_type->access_sec = WIFI_ECN_WEP;
    } else if ( strcmp( security, "wpa" ) == 0 ) {
        wifi_type->access_sec = WIFI_ECN_WPA_PSK;
    } else if ( strcmp( security, "wpa2" ) == 0 ) {
        wifi_type->access_sec = WIFI_ECN_WPA2_PSK;
    } else {
        // set WIFI_ECN_WPA2_PSK as default
        wifi_type->access_sec = WIFI_ECN_WPA2_PSK;
        LOGE("netmgr", "Invalid access security settings! Only support open|wep|wpa|wpa2");
    }
}

static bool valid_access_security(char *security)
{
    bool ret = false;
    if ( strcmp( security, "open" ) == 0
         || strcmp( security, "wep" ) == 0
         || strcmp( security, "wpa" ) == 0
         || strcmp( security, "wpa2" ) == 0 ) {
        ret = true;
    }
    return ret;
}
#endif

static void reconnect_wifi(void *arg)
{
    hal_wifi_module_t    *module;
    hal_wifi_init_type_t type;
    netmgr_ap_config_t   *ap_config = &(g_netmgr_cxt.ap_config);

    module = hal_wifi_get_default_module();

    bzero(&type, sizeof(type));
    type.wifi_mode = STATION;
    type.dhcp_mode = DHCP_CLIENT;
    strncpy(type.wifi_ssid, ap_config->ssid, sizeof(type.wifi_ssid) - 1);
    strncpy(type.wifi_key, ap_config->pwd, sizeof(type.wifi_key) - 1);
#ifdef STM32L475xx
    set_access_security(&type, ap_config->security);
#endif
    hal_wifi_start(module, &type);
}

void netmgr_reconnect_wifi()
{
    g_netmgr_cxt.ip_available = false;
    reconnect_wifi(NULL);
}

static void get_wifi_ssid(void)
{
    memset(g_netmgr_cxt.ap_config.ssid, 0, sizeof(g_netmgr_cxt.ap_config.ssid));
    strncpy(g_netmgr_cxt.ap_config.ssid, g_netmgr_cxt.saved_conf.ssid,
            sizeof(g_netmgr_cxt.ap_config.ssid) - 1);

    memset(g_netmgr_cxt.ap_config.pwd, 0, sizeof(g_netmgr_cxt.ap_config.pwd));
    strncpy(g_netmgr_cxt.ap_config.pwd, g_netmgr_cxt.saved_conf.pwd,
            sizeof(g_netmgr_cxt.ap_config.pwd) - 1);
#ifdef STM32L475xx
    memset(g_netmgr_cxt.ap_config.security, 0, sizeof(g_netmgr_cxt.ap_config.security));
    strncpy(g_netmgr_cxt.ap_config.security, g_netmgr_cxt.saved_conf.security,
            sizeof(g_netmgr_cxt.ap_config.security) - 1);
#endif
}

static int clear_wifi_ssid(void)
{
    int ret = 0;

    memset(g_netmgr_cxt.ap_config.ssid, 0, sizeof(g_netmgr_cxt.ap_config.ssid));
    memset(g_netmgr_cxt.ap_config.pwd, 0, sizeof(g_netmgr_cxt.ap_config.pwd));

    memset(&g_netmgr_cxt.saved_conf, 0, sizeof(wifi_conf_t));
    ret = aos_kv_del(NETMGR_WIFI_KEY); // use kv_del instead of kv_set in case kv is full

    return ret;
}

static int set_wifi_ssid(void)
{
    int ret = 0;

    memset(&g_netmgr_cxt.saved_conf, 0,
           sizeof(wifi_conf_t));
    strncpy(g_netmgr_cxt.saved_conf.ssid,
            g_netmgr_cxt.ap_config.ssid,
            sizeof(g_netmgr_cxt.saved_conf.ssid) - 1);
    strncpy(g_netmgr_cxt.saved_conf.pwd,
            g_netmgr_cxt.ap_config.pwd,
            sizeof(g_netmgr_cxt.saved_conf.pwd) - 1);
#ifdef STM32L475xx
    strncpy(g_netmgr_cxt.saved_conf.security,
            g_netmgr_cxt.ap_config.security,
            sizeof(g_netmgr_cxt.saved_conf.security) - 1);
#endif
    ret = aos_kv_set(NETMGR_WIFI_KEY, (unsigned char *)&g_netmgr_cxt.saved_conf,
                     sizeof(wifi_conf_t), 1);

    return ret;
}

static void handle_wifi_disconnect(void)
{
    g_netmgr_cxt.disconnected_times++;

    stop_mesh();

#if 0 // low level handle disconnect
    if (has_valid_ap() == 1 && g_netmgr_cxt.disconnected_times < MAX_RETRY_CONNECT) {
        aos_post_delayed_action(RETRY_INTERVAL_MS, reconnect_wifi, NULL);
    } else {
        clear_wifi_ssid();
        netmgr_wifi_config_start();
    }
#endif
}

static void netmgr_events_executor(input_event_t *eventinfo, void *priv_data)
{
    if (eventinfo->type != EV_WIFI) {
        return;
    }

    switch (eventinfo->code) {
        case CODE_WIFI_ON_CONNECTED:
            g_netmgr_cxt.disconnected_times = 0;
            break;
        case CODE_WIFI_ON_DISCONNECT:
            handle_wifi_disconnect();
            g_netmgr_cxt.ip_available = false;
            break;
        case CODE_WIFI_ON_PRE_GOT_IP:
            if (g_netmgr_cxt.doing_smartconfig) {
                g_netmgr_cxt.autoconfig_chain->config_result_cb(
                    0, g_netmgr_cxt.ipv4_owned);
                g_netmgr_cxt.autoconfig_chain->autoconfig_stop();
            } else {
                aos_post_event(EV_WIFI, CODE_WIFI_ON_GOT_IP,
                               (unsigned long)(&g_netmgr_cxt.ipv4_owned));
            }
            break;
        case CODE_WIFI_ON_GOT_IP:
            if (g_netmgr_cxt.doing_smartconfig) {
                set_wifi_ssid();
                g_netmgr_cxt.doing_smartconfig = false;
            }
            break;
        case CODE_WIFI_CMD_RECONNECT:
            g_netmgr_cxt.disconnected_times = 0;
            g_netmgr_cxt.ip_available = false;
            LOGD("netmgr", "reconnect wifi - %s, %s",
                 g_netmgr_cxt.ap_config.ssid, g_netmgr_cxt.ap_config.pwd);
            reconnect_wifi(NULL);
            break;
        default :
            break;
    }
}

void wifi_get_ip(char ips[16])
{
    format_ip(g_netmgr_cxt.ipv4_owned, ips);
}

void netmgr_register_wifi_scan_result_callback(netmgr_wifi_scan_result_cb_t cb)
{
    g_netmgr_cxt.cb = cb;
    g_netmgr_cxt.wifi_scan_complete_cb_finished = false;
}

static void netmgr_wifi_config_start(void)
{
    autoconfig_plugin_t *valid_plugin = g_netmgr_cxt.autoconfig_chain;

    if (valid_plugin != NULL) {
        g_netmgr_cxt.doing_smartconfig = true;
        valid_plugin->autoconfig_start();
    } else {
        LOGW(TAG, "net mgr none config policy");
        start_mesh(false);
    }
}

static int32_t has_valid_ap(void)
{
    int32_t len = strlen(g_netmgr_cxt.ap_config.ssid);

    if (len <= 0) {
        return 0;
    }

    return 1;
}

static void add_autoconfig_plugin(autoconfig_plugin_t *plugin)
{
    plugin->next = g_netmgr_cxt.autoconfig_chain;
    g_netmgr_cxt.autoconfig_chain = plugin;
}

int  netmgr_get_ap_config(netmgr_ap_config_t *config)
{
    if (has_valid_ap() == 0) {
        return -1;
    }

    strncpy(config->ssid, g_netmgr_cxt.ap_config.ssid, MAX_SSID_SIZE);
    strncpy(config->pwd, g_netmgr_cxt.ap_config.pwd, MAX_PWD_SIZE);
    strncpy(config->bssid, g_netmgr_cxt.ap_config.bssid, MAX_BSSID_SIZE);
    return 0;
}

void netmgr_clear_ap_config(void)
{
    clear_wifi_ssid();
}

#define HOTSPOT_AP "aha"
int netmgr_set_ap_config(netmgr_ap_config_t *config)
{
    int ret = 0;

    strncpy(g_netmgr_cxt.ap_config.ssid, config->ssid,
            sizeof(g_netmgr_cxt.ap_config.ssid) - 1);
    strncpy(g_netmgr_cxt.ap_config.pwd, config->pwd,
            sizeof(g_netmgr_cxt.ap_config.pwd) - 1);

    strncpy(g_netmgr_cxt.saved_conf.ssid, config->ssid,
            sizeof(g_netmgr_cxt.saved_conf.ssid) - 1);
    strncpy(g_netmgr_cxt.saved_conf.pwd, config->pwd,
            sizeof(g_netmgr_cxt.saved_conf.pwd) - 1);
#ifdef STM32L475xx
    strncpy(g_netmgr_cxt.ap_config.security,
            config->security, MAX_SECURITY_SIZE);
    strncpy(g_netmgr_cxt.saved_conf.security, config->security,
            sizeof(g_netmgr_cxt.saved_conf.security) - 1);
    // STM32L475E ip stack running on WiFi MCU, can only configure with CLI(no ywss)
    // So save the wifi config while config from CLI

    if (valid_access_security(g_netmgr_cxt.ap_config.security)) {
        if (strcmp(config->ssid, HOTSPOT_AP) != 0)
            ret = aos_kv_set(NETMGR_WIFI_KEY, &g_netmgr_cxt.saved_conf,
                             sizeof(wifi_conf_t), 1);
    }
#else
    if (strcmp(config->ssid, HOTSPOT_AP) != 0) // Do not save hotspot AP
        ret = aos_kv_set(NETMGR_WIFI_KEY, &g_netmgr_cxt.saved_conf,
                         sizeof(wifi_conf_t), 1);
#endif
    return ret;
}

void netmgr_set_smart_config(autoconfig_plugin_t *plugin)
{
    add_autoconfig_plugin(plugin);
    netmgr_wifi_config_start();
}

static void read_persistent_conf(void)
{
    int ret;
    int len;

    len = sizeof(wifi_conf_t);
    ret = aos_kv_get(NETMGR_WIFI_KEY, &g_netmgr_cxt.saved_conf, &len);
    if (ret < 0) {
        return;
    }
    get_wifi_ssid();
}

static void handle_netmgr_cmd(char *pwbuf, int blen, int argc, char **argv)
{
    const char *rtype = argc > 1 ? argv[1] : "";
    if (strcmp(rtype, "clear") == 0) {
        netmgr_clear_ap_config();
    } else if (strcmp(rtype, "connect") == 0) {
#ifdef STM32L475xx
        if (argc != 5) {
#else
        if (argc != 4) {
#endif
            return;
        }

        netmgr_ap_config_t config;

        strncpy(config.ssid, argv[2], sizeof(config.ssid) - 1);
        strncpy(config.pwd, argv[3], sizeof(config.pwd) - 1);
#ifdef STM32L475xx
        strncpy(config.security, argv[4], sizeof(config.security) - 1);
#endif
        netmgr_set_ap_config(&config);
        netmgr_start(false);
    } else {
        netmgr_start(true);
    }
}

static struct cli_command ncmd = {
    .name = "netmgr",
#ifdef STM32L475xx
    .help = "netmgr [start|clear|connect ssid password open|wep|wpa|wpa2]",
#else
    .help = "netmgr [start|clear|connect ssid password]",
#endif
    .function = handle_netmgr_cmd,
};

int netmgr_init(void)
{
    hal_wifi_module_t *module;

    aos_register_event_filter(EV_WIFI, netmgr_events_executor, NULL);
    aos_cli_register_command(&ncmd);

    module = hal_wifi_get_default_module();
    memset(&g_netmgr_cxt, 0, sizeof(g_netmgr_cxt));
    g_netmgr_cxt.ip_available = false;
    g_netmgr_cxt.wifi_scan_complete_cb_finished = false;
    g_netmgr_cxt.wifi_hal_mod = module;
#if defined(CONFIG_YWSS) && !defined(CSP_LINUXHOST)
    add_autoconfig_plugin(&g_alink_smartconfig);
#else
    add_autoconfig_plugin(&g_def_smartconfig);
#endif
    hal_wifi_install_event(g_netmgr_cxt.wifi_hal_mod, &g_wifi_hal_event);
    read_persistent_conf();

#ifdef CONFIG_AOS_MESH
    umesh_init(MODE_RX_ON);
#endif

    return 0;
}

void netmgr_deinit(void)
{
    memset(&g_netmgr_cxt, 0, sizeof(g_netmgr_cxt));
}

int netmgr_start(bool autoconfig)
{
    stop_mesh();

    if (has_valid_ap() == 1) {
        aos_post_event(EV_WIFI, CODE_WIFI_CMD_RECONNECT, 0);
        return 0;
    }

    if (autoconfig) {
        netmgr_wifi_config_start();
        return 0;
    }

    start_mesh(false);
    return -1;
}

bool netmgr_get_ip_state()
{
    return g_netmgr_cxt.ip_available;
}

bool netmgr_get_scan_cb_finished()
{
    return g_netmgr_cxt.wifi_scan_complete_cb_finished;
}

static int def_smart_config_start(void)
{
    netmgr_ap_config_t config;

    strncpy(config.ssid, DEMO_AP_SSID, sizeof(config.ssid) - 1);
    strncpy(config.pwd, DEMO_AP_PASSWORD, sizeof(config.pwd) - 1);
    netmgr_set_ap_config(&config);
    aos_post_event(EV_WIFI, CODE_WIFI_CMD_RECONNECT, 0);
    return 0;
}

static void def_smart_config_stop(void)
{
    aos_post_event(EV_WIFI, CODE_WIFI_ON_GOT_IP,
                   (unsigned long)(&g_netmgr_cxt.ipv4_owned));
}

static void def_smart_config_result_cb(int result, uint32_t ip)
{
}

static autoconfig_plugin_t g_def_smartconfig = {
    .description = "def_smartconfig",
    .autoconfig_start = def_smart_config_start,
    .autoconfig_stop = def_smart_config_stop,
    .config_result_cb = def_smart_config_result_cb
};
