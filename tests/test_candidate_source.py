#!/usr/bin/env python3
"""Run small host checks against functions/macros extracted from a VW553 SDK tree.

This is opt-in because the proprietary candidate SDK is staged separately from
TuyaOpen.  The test compiles the extracted production text unchanged and stubs
only hardware/OS boundaries.
"""

from __future__ import annotations

import argparse
import os
import re
from pathlib import Path
import shlex
import subprocess
import tempfile


def extract_function(path: Path, signature: str) -> str:
    source = path.read_text(encoding="utf-8")
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    state = "code"
    escaped = False
    i = brace
    while i < len(source):
        char = source[i]
        nxt = source[i + 1] if i + 1 < len(source) else ""
        if state in ("string", "char"):
            if escaped:
                escaped = False
            elif char == "\\":
                escaped = True
            elif (state == "string" and char == '"') or (state == "char" and char == "'"):
                state = "code"
        elif state == "line_comment":
            if char == "\n":
                state = "code"
        elif state == "block_comment":
            if char == "*" and nxt == "/":
                state = "code"
                i += 1
        elif char == "/" and nxt == "/":
            state = "line_comment"
            i += 1
        elif char == "/" and nxt == "*":
            state = "block_comment"
            i += 1
        elif char == '"':
            state = "string"
        elif char == "'":
            state = "char"
        elif char == "{":
            depth += 1
        elif char == "}":
            depth -= 1
            if depth == 0:
                return source[start : i + 1]
        i += 1
    raise ValueError(f"unterminated production function: {signature}")


def extract_response_macros(path: Path) -> str:
    lines = path.read_text(encoding="utf-8").splitlines()
    start = next(i for i, line in enumerate(lines) if line.startswith("#define AT_RSP_START("))
    end = next(i for i, line in enumerate(lines[start:], start) if line.startswith("#define AT_RSP_ERR("))
    while not lines[end].rstrip().endswith("while(0)"):
        end += 1
    return "\n".join(lines[start : end + 1])


def assert_supported_at_response_formats(app: Path) -> None:
    """Reject printf conversions unsupported by the VW553 co_snprintf()."""
    for name in ("atcmd.c", "atcmd_tcpip.c", "atcmd_wifi.c"):
        text = (app / name).read_text(encoding="utf-8")
        for match in re.finditer(r'AT_RSP\(\s*"((?:[^"\\]|\\.)*)"', text, re.S):
            fmt = match.group(1)
            assert "%l" not in fmt, f"{name}: unsupported AT_RSP length modifier: {fmt!r}"
            assert "%z" not in fmt, f"{name}: unsupported AT_RSP size modifier: {fmt!r}"


def assert_prompt_dma_order(app: Path) -> None:
    tcpip = app / "atcmd_tcpip.c"
    for signature in ("static int at_tcp_send(int fd, uint32_t tx_len)",
                      "static int at_udp_send(int con_id, int fd, uint32_t tx_len, char *srv_ip, "
                      "uint16_t srv_port, int listener_fd)"):
        body = extract_function(tcpip, signature)
        assert "at_hw_dma_receive_with_prompt" in body, signature
        assert 'AT_RSP_DIRECT(">\r\n", 3);' not in body, signature

    atcmd = (app / "atcmd.c").read_text(encoding="utf-8")
    start = atcmd.index("int at_hw_dma_receive_with_prompt(")
    end = atcmd.index("\n}", start) + 2
    helper = atcmd[start:end]
    helper = helper[helper.index("#else"):]
    arm = helper.index("at_uart_prompt_dma_start")
    prompt = helper.index("at_hw_send(prompt, prompt_size)")
    wait = helper.index("at_uart_prompt_dma_wait")
    assert arm < prompt < wait, "UART prompt must be exposed only after DMA is armed"


def find_app_dir(root: Path) -> Path:
    candidates = [root / "MSDK" / "app"]
    candidates.extend(root.glob("**/MSDK/app"))
    for candidate in candidates:
        if (candidate / "atcmd.c").is_file() and (candidate / "atcmd_wifi.c").is_file():
            return candidate
    raise FileNotFoundError(f"could not find MSDK/app in {root}")


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-root", type=Path, required=True)
    args = parser.parse_args()
    app = find_app_dir(args.candidate_root.resolve())
    assert_supported_at_response_formats(app)
    assert_prompt_dma_order(app)

    production = """\
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include "atcmd_tx_helpers.h"
#define AT_QUOTE '\"'
#define AT_QUESTION '?'
#define DEBUG_ASSERT(x) assert(x)
#define AT_TRACE(...) ((void)0)
static char sent[512];
static int sent_len;
static char captured_pwd[64];
static int ap_calls;
static int ps_mode_calls;
static int ps_interval_calls;
static unsigned ps_mode;
static unsigned ps_interval;
#define WIFI_VIF_INDEX_DEFAULT 0
#define WIFI_STA_PS_MODE_OFF 0
#define WIFI_STA_PS_MODE_ALW_ON 1
#define WIFI_STA_PS_MODE_BASED_ON_TD 2
static int wifi_netlink_ps_mode_set(int vif, uint8_t mode) {
    assert(vif == WIFI_VIF_INDEX_DEFAULT);
    ++ps_mode_calls;
    ps_mode = mode;
    return 0;
}
static int wifi_netlink_listen_interval_set(uint8_t interval) {
    ++ps_interval_calls;
    ps_interval = interval;
    return 0;
}
typedef struct {
    int tuple_valid;
    int tuple_calls;
    int send_calls;
    int sendto_calls;
    int retire_calls;
    int sleep_calls;
    int tcp_step;
    int partial_then_fail;
    int udp_result;
    uint8_t tcp_sent[32];
    size_t tcp_sent_len;
} tx_capture_t;
static int tx_tuple(void *ctx, int con_id, int fd, uint32_t generation,
                    int listener_fd, uint32_t listener_generation, int is_udp_listener) {
    tx_capture_t *c = ctx;
    (void)con_id; (void)fd; (void)generation; (void)listener_fd;
    (void)listener_generation; (void)is_udp_listener;
    ++c->tuple_calls;
    return c->tuple_valid;
}
static int tx_send(void *ctx, int fd, const void *buf, uint32_t len, int flags) {
    tx_capture_t *c = ctx;
    (void)fd; (void)flags;
    ++c->send_calls;
    if (c->partial_then_fail) {
        if (c->tcp_step++ == 0) {
            assert(len >= 2U);
            memcpy(c->tcp_sent + c->tcp_sent_len, buf, 2U);
            c->tcp_sent_len += 2U;
            return 2;
        }
        errno = EAGAIN;
        return -1;
    }
    if (c->tcp_step++ == 0) { errno = EAGAIN; return -1; }
    int n = c->tcp_step == 2 ? 2 : (int)len;
    assert(n <= (int)len && c->tcp_sent_len + (size_t)n <= sizeof(c->tcp_sent));
    memcpy(c->tcp_sent + c->tcp_sent_len, buf, (size_t)n);
    c->tcp_sent_len += (size_t)n;
    return n;
}
static void tx_retire(void *ctx, int con_id, int fd, uint32_t generation) {
    tx_capture_t *c = ctx;
    (void)con_id; (void)fd; (void)generation;
    ++c->retire_calls;
}
static int tx_sendto(void *ctx, int fd, const void *buf, uint32_t len, int flags,
                     const void *to, uint32_t tolen) {
    tx_capture_t *c = ctx;
    (void)fd; (void)buf; (void)len; (void)flags; (void)to; (void)tolen;
    ++c->sendto_calls;
    return c->udp_result;
}
static void tx_sleep(void *ctx, uint32_t delay_ms) {
    tx_capture_t *c = ctx;
    assert(delay_ms == ATCMD_TX_RETRY_DELAY_MS);
    ++c->sleep_calls;
}
static void *sys_zalloc(size_t n) { return calloc(1, n); }
static void sys_mfree(void *p) { free(p); }
void sys_memset(void *p, int v, size_t n) { memset(p, v, n); }
static int co_snprintf(char *out, int n, const char *fmt, ...) {
    assert(strstr(fmt, "%l") == NULL && strstr(fmt, "%z") == NULL);
    va_list ap; va_start(ap, fmt); int r = vsnprintf(out, (size_t)n, fmt, ap); va_end(ap); return r;
}
static void at_hw_send(const char *data, int n) {
    assert(n >= 0 && (size_t)n <= sizeof(sent));
    memcpy(sent, data, (size_t)n); sent_len = n;
}

#define CIP_TYPE_TCP 1
#define CIP_TYPE_UDP 2
static int multi_connection_enable;
static int ty553x_server_mode;
static int server_find_result;
static int server_start_result;
static int server_running;
typedef struct {
    int local_srv_fd[4];
    uint8_t local_srv_stop[4];
    uint8_t local_srv_type[4];
    uint16_t local_srv_port[4];
    uint32_t local_srv_generation[4];
} cip_info_stub_t;
static cip_info_stub_t cip_info;
static int cip_slot_lock;
static void sys_mutex_get(void *mutex) { (void)mutex; }
static void sys_mutex_put(void *mutex) { (void)mutex; }
static int cip_server_find(uint8_t type) {
    (void)type;
    return server_running ? 0 : server_find_result;
}
static int cip_server_find_port(uint8_t type, uint16_t port) {
    (void)type; (void)port;
    return server_running ? 0 : -1;
}
static int cip_server_free_slot(void) { return 0; }
static int cip_server_count(void) { return server_running ? 1 : 0; }
static int tcp_udp_server_start(uint8_t type, uint16_t port) {
    if (server_start_result == 0) {
        server_running = 1;
        cip_info.local_srv_fd[0] = 5;
        cip_info.local_srv_port[0] = port;
        cip_info.local_srv_type[0] = type;
        cip_info.local_srv_generation[0] = 7U;
    }
    return server_start_result;
}
static int tcp_udp_server_stop(uint8_t type, uint16_t port) { (void)type; (void)port; return 0; }
static int cip_recv_task_start(void) { return 0; }
"""
    production += extract_response_macros(app / "atcmd.h") + "\n"
    tcpip_header = (app / "atcmd_tcpip.h").read_text(encoding="utf-8").splitlines()
    max_clients = next(line for line in tcpip_header if line.startswith("#define MAX_CLIENT_NUM"))
    max_servers = next(line for line in tcpip_header if line.startswith("#define MAX_SERVER_NUM"))
    production += "#define MEMP_NUM_NETCONN 12\n" + max_servers + "\n" + max_clients + "\n"
    production += "typedef int wifi_ap_auth_mode_t;\n#define AUTH_MODE_WPA2_WPA3 1\n"
    production += """\
static int wifi_management_ap_start(char *ssid, char *pwd, uint32_t channel,
                                    wifi_ap_auth_mode_t auth, uint32_t hidden) {
    (void)ssid; (void)channel; (void)auth; (void)hidden;
    ++ap_calls;
    strncpy(captured_pwd, pwd, sizeof(captured_pwd) - 1U);
    return 0;
}
"""
    production += extract_function(app / "atcmd.c", "char *at_string_parse(char *param)") + "\n"
    production += extract_function(app / "atcmd_wifi.c", "void at_cw_ap_cur_start(int argc, char **argv)") + "\n"
    production += extract_function(app / "atcmd_wifi.c", "void at_ty553x_ps(int argc, char **argv)") + "\n"
    production += extract_function(app / "atcmd_tcpip.c", "void at_ty553x_cap(int argc, char **argv)") + "\n"
    production += extract_function(app / "atcmd_tcpip.c", "void at_ty553x_server(int argc, char **argv)") + "\n"
    production += r'''\
int main(void) {
    char ssid[] = "\"Tuya-Setup\"", pwd[] = "\"\"", channel[] = "6", hidden[] = "0";
    char *valid[] = {"AT+CWSAP_CUR", ssid, pwd, channel, hidden};
    at_cw_ap_cur_start(5, valid);
    assert(ap_calls == 1 && captured_pwd[0] == '\0');

    char bad_ssid[] = "\"Tuya-Setup\"", bad_pwd[] = "\"open", channel2[] = "6", hidden2[] = "0";
    char *invalid[] = {"AT+CWSAP_CUR", bad_ssid, bad_pwd, channel2, hidden2};
    at_cw_ap_cur_start(5, invalid);
    assert(ap_calls == 1);

    char ps_mode_off[] = "0", ps_dtim_zero[] = "0";
    char *ps_off_args[] = {"AT+TY553XPS", ps_mode_off, ps_dtim_zero};
    at_ty553x_ps(3, ps_off_args);
    assert(ps_mode_calls == 1 && ps_interval_calls == 1);
    assert(ps_mode == WIFI_STA_PS_MODE_OFF && ps_interval == 0U);
    assert(sent_len == 4 && memcmp(sent, "OK\r\n", 4) == 0);

    char ps_mode_bad[] = "3", ps_dtim_bad[] = "0";
    char *ps_bad_args[] = {"AT+TY553XPS", ps_mode_bad, ps_dtim_bad};
    at_ty553x_ps(3, ps_bad_args);
    assert(ps_mode_calls == 1 && ps_interval_calls == 1);
    assert(sent_len == 7 && memcmp(sent, "ERROR\r\n", 7) == 0);

    /* H77 sends the type token unquoted. A forced socket-start failure must
     * reach reason 12, proving unquoted TCP passed the 553 parser first. */
    multi_connection_enable = 1;
    server_find_result = -1;
    server_start_result = -1;
    server_running = 0;
    char server_type[] = "TCP", server_port[] = "6668";
    char *server_args[] = {"AT+TY553XSERVER", server_type, server_port};
    at_ty553x_server(3, server_args);
    assert(strstr(sent, "SERVERERR:8\r\nERROR\r\n") != NULL);

    server_start_result = 0;
    server_running = 0;
    at_ty553x_server(3, server_args);
    static const char server_ok[] = "+TY553XSERVER2:TCP,6668,5,7\r\nOK\r\n";
    assert(sent_len == (int)sizeof(server_ok) - 1);
    assert(memcmp(sent, server_ok, sizeof(server_ok) - 1U) == 0);

    char *cap_argv[] = {"AT+TY553XCAP?"};
    at_ty553x_cap(1, cap_argv);
    static const char cap_expected[] = "+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=6,MAX_SEND=2048,MAX_CLIENTS=6,MAX_SERVERS=4\r\nOK\r\n";
    assert(sent_len == (int)sizeof(cap_expected) - 1);
    assert(memcmp(sent, cap_expected, sizeof(cap_expected) - 1U) == 0);

    AT_RSP_START(128);
    AT_RSP("+TY553XCAP:3,ATOMIC_IPD,MULTI_SERVER,TCP_ACCEPT2,CLOSE_EVENT,CLOSE_ACK,GEN32,STATE_QUERY,UDP_SEND_ID=6,MAX_SEND=2048,MAX_CLIENTS=6,MAX_SERVERS=4\r\n");
    AT_RSP_OK();
    assert(sent_len == 7 && memcmp(sent, "ERROR\r\n", 7) == 0);

    const uint8_t tx_data[] = "hello";
    tx_capture_t tx = {.tuple_valid = 1};
    assert(atcmd_tcp_send_all(3, 55, 9, tx_data, 5, 0, &tx, tx_tuple, tx_retire, tx_send, tx_sleep) == 5);
    assert(tx.send_calls == 3 && tx.sleep_calls == 1 && tx.tcp_sent_len == 5);
    assert(memcmp(tx.tcp_sent, tx_data, 5) == 0);
    tx = (tx_capture_t){.tuple_valid = 0};
    assert(atcmd_tcp_send_all(3, 55, 9, tx_data, 5, 0, &tx, tx_tuple, tx_retire, tx_send, tx_sleep) == -1);
    assert(tx.send_calls == 0 && tx.retire_calls == 0 && errno == ECONNRESET);
    tx = (tx_capture_t){.tuple_valid = 1, .partial_then_fail = 1};
    assert(atcmd_tcp_send_all(3, 55, 9, tx_data, 5, 0, &tx, tx_tuple, tx_retire, tx_send, tx_sleep) == -1);
    assert(tx.send_calls == ATCMD_TX_MAX_ATTEMPTS && tx.retire_calls == 1 && errno == EAGAIN);

    const uint32_t destination = 0x01020304;
    tx = (tx_capture_t){.tuple_valid = 1, .udp_result = 4};
    assert(atcmd_udp_send_exact(3, 55, 9, -1, 0, 0, tx_data, 5, 0, &destination,
                                sizeof(destination), &tx, tx_tuple, tx_sendto, tx_sleep) == -1);
    assert(tx.sendto_calls == 1 && errno == EIO);
    tx = (tx_capture_t){.tuple_valid = 1, .udp_result = 5};
    assert(atcmd_udp_send_exact(3, 55, 9, -1, 0, 0, tx_data, 5, 0, &destination,
                                sizeof(destination), &tx, tx_tuple, tx_sendto, tx_sleep) == 5);
    assert(tx.sendto_calls == 1);
    puts("VW553 production-source checks passed");
    return 0;
}
'''

    with tempfile.TemporaryDirectory(prefix="vw553-source-test-") as temp_dir:
        temp = Path(temp_dir)
        source = temp / "candidate_test.c"
        binary = temp / "candidate_test"
        source.write_text(production, encoding="utf-8")
        compiler = shlex.split(os.environ.get("CC", "cc"))
        subprocess.run(
            compiler
            + [
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                "-fno-omit-frame-pointer",
                "-I",
                str(app),
                str(source),
                "-o",
                str(binary),
            ],
            check=True,
        )
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
