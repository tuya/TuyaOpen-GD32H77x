#include "FreeRTOS.h"
#include "task.h"
#include "at_socket.h"
#include "at_device.h"
#include "gd32h77x_console.h"
#include <string.h>
#include <stdio.h>
static void status(int argc,char **argv)
{
    (void)argc;(void)argv;
    const char *commands[]={"AT+CWSTATUS","AT+CIFSR"};
    for(unsigned i=0;i<2;++i) {
        char reply[512]={0};at_result_t result=AT_RESULT_NONE;
        at_request_t req={.command=commands[i],.command_length=strlen(commands[i]),
            .reply=reply,.reply_capacity=sizeof(reply),.timeout_ms=5000};
        int rc=at_device_execute(&req,&result);
        gd32h77x_console_printf("NET_STATUS rc=%d %s\r\n%s\r\n",rc,commands[i],reply);
    }
}
static int execute(const char *command, unsigned timeout)
{
    char reply[512]={0};at_result_t result=AT_RESULT_NONE;
    at_request_t req={.command=command,.command_length=strlen(command),
        .reply=reply,.reply_capacity=sizeof(reply),.timeout_ms=timeout};
    int rc=at_device_execute(&req,&result);
    gd32h77x_console_printf("NET_COMMAND rc=%d\r\n%s\r\n",rc,reply);
    return rc==AT_STATUS_OK && result!=AT_RESULT_OK ? AT_STATUS_IO_ERROR : rc;
}
static volatile bool running;
static void join(int argc,char **argv)
{
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    if(argc!=3 || strlen(argv[1])>32 || strlen(argv[2])>63 ||
       strpbrk(argv[1],"\"\\\r\n") || strpbrk(argv[2],"\"\\\r\n")) return;
    char command[128];
    at_socket_wifi_join_set(true);
    int rc=execute("AT+CWMODE_CUR=1",5000);
    vTaskDelay(pdMS_TO_TICKS(2000));
    if(rc==AT_STATUS_OK) {
        snprintf(command,sizeof(command),"AT+CWJAP_CUR=\"%s\",\"%s\"",argv[1],argv[2]);
        rc=execute(command,45000);
        memset(command,0,sizeof(command));
    }
    at_socket_wifi_join_set(false);
    gd32h77x_console_printf("NET_JOIN rc=%d\r\n",rc);
    status(0,NULL);
}
static bool transfer(int fd,uint8_t *buf,size_t len,bool send)
{
    size_t done=0;
    while(done<len) {
        int n=send?at_socket_send(fd,buf+done,len-done):at_socket_recv(fd,buf+done,len-done);
        if(n<=0 || (size_t)n>len-done) return false;
        done+=(size_t)n;
    }
    return true;
}
static bool dns_suite(void)
{
    uint32_t addr;unsigned passed=0,failed=0;
    if(at_socket_gethostbyname("1.1.1.1",&addr)!=AT_STATUS_OK) {
        gd32h77x_console_printf("NET_RESOLVE_FAIL\r\n");return false;
    }
    for(unsigned type=0;type<2;++type) for(unsigned k=0;k<3;++k) {
        uint8_t query[]={0x53,0,1,0,0,1,0,0,0,0,0,0,
            7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
        uint8_t rx[512],prefix[2]={0,sizeof(query)};
        query[1]=(uint8_t)(type*3+k+1);
        size_t got=0;bool ok=true;
        int fd=at_socket_create(type?AT_SOCKET_TYPE_DATAGRAM:AT_SOCKET_TYPE_STREAM);
        if(fd<0) {++failed;continue;}
        at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_SEND);
        at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_RECV);
        if(type) {
            uint32_t peer=0;uint16_t port=0;
            ok=at_socket_sendto(fd,query,sizeof(query),addr,53)==sizeof(query);
            if(ok) {
                int n=at_socket_recvfrom(fd,rx,sizeof(rx),&peer,&port);
                ok=n>0 && peer==addr && port==53;
                if(n>0)got=(size_t)n;
            }
        } else {
            ok=at_socket_connect(fd,addr,53)==AT_SOCKET_ERRNO_NONE &&
                transfer(fd,prefix,2,true) && transfer(fd,query,sizeof(query),true) &&
                transfer(fd,prefix,2,false);
            if(ok) {
                got=((unsigned)prefix[0]<<8)|prefix[1];
                ok=got<=sizeof(rx) && got>=sizeof(query) && transfer(fd,rx,got,false);
            }
        }
        ok=ok && got>=sizeof(query) && rx[0]==query[0] && rx[1]==query[1] &&
            (rx[2]&0xfa)==0x80 && (rx[3]&15)==0 && rx[4]==0 && rx[5]==1 &&
            (rx[6] || rx[7]) && !memcmp(rx+12,query+12,sizeof(query)-12);
        if(ok)++passed;else ++failed;
        gd32h77x_console_printf("NET_DNS %s case=%u rx=%u %s errno=%d\r\n",
            type?"UDP":"TCP",k+1,(unsigned)got,ok?"PASS":"FAIL",ok?0:(int)at_socket_get_errno());
        int close_rc=at_socket_close(fd);
        if(close_rc!=AT_SOCKET_ERRNO_NONE) {
            ++failed;gd32h77x_console_printf("NET_CLOSE_FAIL rc=%d\r\n",close_rc);
        }
    }
    gd32h77x_console_printf("NET_RESULT pass=%u fail=%u stack_free=%u\r\n",passed,failed,(unsigned)uxTaskGetStackHighWaterMark(NULL));
    return failed==0 && passed==6;
}
static void network_task(void *arg)
{
    (void)arg;dns_suite();running=false;vTaskDelete(NULL);
}
static void run(int argc,char **argv)
{
    (void)argc;(void)argv;
    if(running)return;
    running=true;
    if(xTaskCreate(network_task,"net_probe",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
/* Runtime credentials only; erased when the bounded test finishes. */
static char cycle_ssid[33],cycle_password[64];
static bool wait_wifi(bool connected)
{
    for(unsigned attempt=0;attempt<15;++attempt) {
        char reply[512]={0};at_result_t result=AT_RESULT_NONE;
        at_request_t req={.command="AT+CWSTATUS",.command_length=11,
            .reply=reply,.reply_capacity=sizeof(reply),.timeout_ms=5000};
        int rc=at_device_execute(&req,&result);
        if(rc==AT_STATUS_OK && result==AT_RESULT_OK &&
           strstr(reply,connected?"+CWSTATUS: STA, connected,":"+CWSTATUS: STA, disconnected"))return true;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    return false;
}
static bool cycle_join(void)
{
    char command[128];
    at_socket_wifi_join_set(true);
    snprintf(command,sizeof(command),"AT+CWJAP_CUR=\"%s\",\"%s\"",cycle_ssid,cycle_password);
    int rc=execute(command,45000);
    memset(command,0,sizeof(command));
    at_socket_wifi_join_set(false);
    return rc==AT_STATUS_OK && wait_wifi(true);
}
static void cycle_task(void *arg)
{
    (void)arg;unsigned passed=0,failed=0;
    bool ready=execute("AT+CWMODE_CUR=1",5000)==AT_STATUS_OK;
    vTaskDelay(pdMS_TO_TICKS(2000));
    ready=ready && cycle_join() && dns_suite();
    if(!ready) {++failed;gd32h77x_console_printf("NET_CYCLE_BASELINE_FAIL\r\n");}
    for(unsigned cycle=1;ready && cycle<=3;++cycle) {
        uint32_t addr=0;int old[2]={-1,-1};bool ok=true;
        if(at_socket_gethostbyname("1.1.1.1",&addr)!=AT_STATUS_OK)ok=false;
        for(unsigned type=0;ok && type<2;++type) {
            old[type]=at_socket_create(type?AT_SOCKET_TYPE_DATAGRAM:AT_SOCKET_TYPE_STREAM);
            if(old[type]<0) {ok=false;break;}
            at_socket_set_timeout(old[type],1500,AT_SOCKET_TRANSFER_SEND);
            at_socket_set_timeout(old[type],1500,AT_SOCKET_TRANSFER_RECV);
            if(at_socket_connect(old[type],addr,53)!=AT_SOCKET_ERRNO_NONE)ok=false;
        }
        bool disconnected=ok && execute("AT+CWQAP",5000)==AT_STATUS_OK && wait_wifi(false);
        ok=ok && disconnected;
        gd32h77x_console_printf("NET_CYCLE phase=disconnect cycle=%u %s\r\n",cycle,disconnected?"PASS":"FAIL");
        for(unsigned type=0;type<2;++type)if(old[type]>=0) {
            if(disconnected) {
                uint8_t byte;int n=at_socket_recv(old[type],&byte,1);
                gd32h77x_console_printf("NET_CYCLE phase=old_receive cycle=%u type=%s rc=%d\r\n",cycle,type?"UDP":"TCP",n);
                if(n>0)ok=false;
            }
            int rc=at_socket_close(old[type]);
            gd32h77x_console_printf("NET_CYCLE phase=old_close cycle=%u type=%s rc=%d\r\n",cycle,type?"UDP":"TCP",rc);
            if(rc!=AT_SOCKET_ERRNO_NONE)ok=false;
        }
        /* Always try to restore Wi-Fi, even when the outage checks fail. */
        bool rejoined=cycle_join();
        gd32h77x_console_printf("NET_CYCLE phase=rejoin cycle=%u %s\r\n",cycle,rejoined?"PASS":"FAIL");
        bool network_ok=rejoined && dns_suite();
        ok=ok && network_ok;
        if(ok)++passed;else ++failed;
        gd32h77x_console_printf("NET_CYCLE cycle=%u %s\r\n",cycle,ok?"PASS":"FAIL");
        ready=rejoined;
    }
    memset(cycle_ssid,0,sizeof(cycle_ssid));memset(cycle_password,0,sizeof(cycle_password));
    gd32h77x_console_printf("NET_CYCLE_RESULT pass=%u fail=%u completed=%u stack_free=%u\r\n",
        passed,failed,passed+failed,(unsigned)uxTaskGetStackHighWaterMark(NULL));
    running=false;vTaskDelete(NULL);
}
static void cycle(int argc,char **argv)
{
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    if(argc!=3 || !strlen(argv[1]) || strlen(argv[1])>32 || strlen(argv[2])>63 ||
       strpbrk(argv[1],"\"\\\r\n") || strpbrk(argv[2],"\"\\\r\n")) {
        gd32h77x_console_printf("Usage: net_cycle <ssid> <password>\r\n");return;
    }
    strcpy(cycle_ssid,argv[1]);strcpy(cycle_password,argv[2]);running=true;
    if(xTaskCreate(cycle_task,"net_cycle",4096,NULL,2,NULL)!=pdPASS) {
        memset(cycle_ssid,0,sizeof(cycle_ssid));memset(cycle_password,0,sizeof(cycle_password));
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
static unsigned soak_rounds;
static void soak_task(void *arg)
{
    (void)arg;
    uint32_t addr=0;unsigned passed=0,failed=0,large_tcp=0,large_udp=0;
    unsigned consecutive_failures=0,completed=0;
    size_t heap_start=xPortGetFreeHeapSize(),heap_warm=0,heap_end=heap_start;
    bool resolved=at_socket_gethostbyname("1.1.1.1",&addr)==AT_STATUS_OK;
    if(!resolved)++failed;
    for(unsigned round=1;resolved && round<=soak_rounds;++round) {
        unsigned round_fail=0;
        for(unsigned type=0;type<2;++type)for(unsigned large=0;large<2;++large) {
            uint8_t small_query[]={0x63,0,1,0,0,1,0,0,0,0,0,0,
                7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
            /* Root DNSKEY with EDNS(1232): require a response over 240 bytes. */
            uint8_t large_query[]={0x64,0,1,0,0,1,0,0,0,0,0,1,
                0,0,48,0,1,0,0,41,4,208,0,0,0,0,0,0};
            uint8_t *query=large?large_query:small_query;
            size_t query_len=large?sizeof(large_query):sizeof(small_query);
            size_t question_len=large?5:sizeof(small_query)-12;
            query[1]=(uint8_t)(round*4+type*2+large);
            uint8_t rx[1536],prefix[2]={(uint8_t)(query_len>>8),(uint8_t)query_len};size_t got=0;
            int fd=at_socket_create(type?AT_SOCKET_TYPE_DATAGRAM:AT_SOCKET_TYPE_STREAM);
            bool ok=fd>=0;
            if(ok) {
                at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_SEND);
                at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_RECV);
                if(type) {
                    uint32_t peer=0;uint16_t port=0;
                    ok=at_socket_sendto(fd,query,query_len,addr,53)==(int)query_len;
                    if(ok) {
                        int n=at_socket_recvfrom(fd,rx,sizeof(rx),&peer,&port);
                        ok=n>0 && peer==addr && port==53;
                        if(n>0)got=(size_t)n;
                    }
                } else {
                    ok=at_socket_connect(fd,addr,53)==AT_SOCKET_ERRNO_NONE &&
                        transfer(fd,prefix,2,true) && transfer(fd,query,query_len,true) &&
                        transfer(fd,prefix,2,false);
                    if(ok) {
                        got=((unsigned)prefix[0]<<8)|prefix[1];
                        ok=got>=12+question_len && got<=sizeof(rx) && transfer(fd,rx,got,false);
                    }
                }
                ok=ok && got>=12+question_len && got<=sizeof(rx) &&
                    rx[0]==query[0] && rx[1]==query[1] && (rx[2]&0xfa)==0x80 &&
                    (rx[3]&15)==0 && rx[4]==0 && rx[5]==1 && (rx[6] || rx[7]) &&
                    !memcmp(rx+12,query+12,question_len) && (!large || got>240);
                int close_rc=at_socket_close(fd);
                if(close_rc!=AT_SOCKET_ERRNO_NONE)ok=false;
            }
            if(ok) {
                ++passed;
                if(large) {if(type)++large_udp;else ++large_tcp;}
            } else {++failed;++round_fail;}
            gd32h77x_console_printf("NET_SOAK round=%u type=%s query=%s rx=%u %s\r\n",
                round,type?"UDP":"TCP",large?"DNSKEY":"A",(unsigned)got,ok?"PASS":"FAIL");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        heap_end=xPortGetFreeHeapSize();if(round==1)heap_warm=heap_end;
        completed=round;
        gd32h77x_console_printf("NET_SOAK_ROUND round=%u fail=%u heap=%u heap_min=%u stack_free=%u\r\n",
            round,round_fail,(unsigned)heap_end,(unsigned)xPortGetMinimumEverFreeHeapSize(),
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
        consecutive_failures=round_fail?consecutive_failures+1:0;
        if(consecutive_failures>=3)break;
    }
    gd32h77x_console_printf("NET_SOAK_RESULT rounds=%u expected=%u pass=%u fail=%u large_tcp=%u large_udp=%u heap_start=%u heap_warm=%u heap_end=%u\r\n",
        completed,soak_rounds,passed,failed,large_tcp,large_udp,
        (unsigned)heap_start,(unsigned)heap_warm,(unsigned)heap_end);
    running=false;vTaskDelete(NULL);
}
static void soak(int argc,char **argv)
{
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    unsigned rounds=20;
    if(argc>2) return;
    if(argc==2) {
        rounds=0;
        for(const char *p=argv[1];*p;++p) {
            if(*p<'0' || *p>'9' || rounds>100)return;
            rounds=rounds*10+(unsigned)(*p-'0');
        }
        if(rounds<1 || rounds>100)return;
    }
    running=true;soak_rounds=rounds;
    if(xTaskCreate(soak_task,"net_soak",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
static void tx_task(void *arg)
{
    (void)arg;
    uint32_t addr=0;unsigned passed=0,failed=0,large_tcp=0,large_udp=0;
    unsigned consecutive_failures=0,completed=0;
    size_t heap_start=xPortGetFreeHeapSize(),heap_warm=0,heap_end=heap_start;
    bool resolved=at_socket_gethostbyname("1.1.1.1",&addr)==AT_STATUS_OK;
    if(!resolved)++failed;
    for(unsigned round=1;resolved && round<=3;++round) {
        unsigned round_fail=0;
        for(unsigned type=0;type<2;++type)for(unsigned large=0;large<4;++large) {
            /* Root DNSKEY with EDNS(1232): require a response over 240 bytes. */
            uint8_t large_query[]={0x64,0,1,0,0,1,0,0,0,0,0,1,
                0,0,48,0,1,0,0,41,4,208,0,0,0,0,0,0};
            const unsigned sizes[]={239,240,241,1024};
            uint8_t padded[1024]={0};
            size_t query_len=sizes[large];
            memcpy(padded,large_query,sizeof(large_query));
            unsigned rdlen=(unsigned)query_len-28,padlen=(unsigned)query_len-32;
            padded[26]=(uint8_t)(rdlen>>8);padded[27]=(uint8_t)rdlen;
            padded[28]=0;padded[29]=12;
            padded[30]=(uint8_t)(padlen>>8);padded[31]=(uint8_t)padlen;
            uint8_t *query=padded;
            size_t question_len=5;
            query[1]=(uint8_t)(round*8+type*4+large);
            uint8_t rx[1536],prefix[2]={(uint8_t)(query_len>>8),(uint8_t)query_len};size_t got=0;
            int fd=at_socket_create(type?AT_SOCKET_TYPE_DATAGRAM:AT_SOCKET_TYPE_STREAM);
            bool ok=fd>=0;
            if(ok) {
                at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_SEND);
                at_socket_set_timeout(fd,8000,AT_SOCKET_TRANSFER_RECV);
                if(type) {
                    uint32_t peer=0;uint16_t port=0;
                    ok=at_socket_sendto(fd,query,query_len,addr,53)==(int)query_len;
                    if(ok) {
                        int n=at_socket_recvfrom(fd,rx,sizeof(rx),&peer,&port);
                        ok=n>0 && peer==addr && port==53;
                        if(n>0)got=(size_t)n;
                    }
                } else {
                    ok=at_socket_connect(fd,addr,53)==AT_SOCKET_ERRNO_NONE &&
                        transfer(fd,prefix,2,true) && transfer(fd,query,query_len,true) &&
                        transfer(fd,prefix,2,false);
                    if(ok) {
                        got=((unsigned)prefix[0]<<8)|prefix[1];
                        ok=got>=12+question_len && got<=sizeof(rx) && transfer(fd,rx,got,false);
                    }
                }
                ok=ok && got>=12+question_len && got<=sizeof(rx) &&
                    rx[0]==query[0] && rx[1]==query[1] && (rx[2]&0xfa)==0x80 &&
                    (rx[3]&15)==0 && rx[4]==0 && rx[5]==1 && (rx[6] || rx[7]) &&
                    !memcmp(rx+12,query+12,question_len) && got>240;
                int close_rc=at_socket_close(fd);
                if(close_rc!=AT_SOCKET_ERRNO_NONE)ok=false;
            }
            if(ok) {
                ++passed;
                if(type)++large_udp;else ++large_tcp;
            } else {++failed;++round_fail;}
            gd32h77x_console_printf("NET_TX round=%u type=%s tx=%u rx=%u %s\r\n",
                round,type?"UDP":"TCP",(unsigned)query_len,(unsigned)got,ok?"PASS":"FAIL");
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        heap_end=xPortGetFreeHeapSize();if(round==1)heap_warm=heap_end;
        completed=round;
        gd32h77x_console_printf("NET_TX_ROUND round=%u fail=%u heap=%u heap_min=%u stack_free=%u\r\n",
            round,round_fail,(unsigned)heap_end,(unsigned)xPortGetMinimumEverFreeHeapSize(),
            (unsigned)uxTaskGetStackHighWaterMark(NULL));
        consecutive_failures=round_fail?consecutive_failures+1:0;
        if(consecutive_failures>=3)break;
    }
    gd32h77x_console_printf("NET_TX_RESULT rounds=%u expected=%u pass=%u fail=%u large_tcp=%u large_udp=%u heap_start=%u heap_warm=%u heap_end=%u\r\n",
        completed,3U,passed,failed,large_tcp,large_udp,
        (unsigned)heap_start,(unsigned)heap_warm,(unsigned)heap_end);
    running=false;vTaskDelete(NULL);
}
static void tx(int argc,char **argv)
{
    (void)argc;(void)argv;
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    running=true;
    if(xTaskCreate(tx_task,"net_tx",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
#include "multi_probe.inc"
#include "tkl_probe.inc"
static const gd32h77x_console_command_t commands[]={
    {"net_tkl","Production TKL network API hardware checks",tkl_probe},
    {"net_status","Wi-Fi status/IP",status},
    {"net_join","Join Wi-Fi (console input contains credentials)",join},
    {"net_public","Public TCP/UDP DNS response validation",run},
    {"net_cycle","Three Wi-Fi disconnect/rejoin cycles: ssid password",cycle},
    {"net_soak","Public DNS small/large repeated connections [1..100 rounds]",soak},
    {"net_tx","Padded DNS requests 239/240/241/1024 bytes",tx},
    {"net_multi","Two TCP and two UDP sockets with select",multi},
    {"net_multi_diag","Compare modes 0=mixed big 1=dual UDP big 2=dual UDP small 3=mixed small 4=single UDP big 5=paced UDP 6=alternate DNS",multi_diag}};
void app_entry(void *arg)
{
    (void)arg;
    at_device_subscribe(multi_observe,NULL);
    int rc=at_socket_init();
    gd32h77x_console_printf("NET_PROBE_READY transport=spi init=%d\r\n",rc);
    gd32h77x_console_start(commands,sizeof(commands)/sizeof(commands[0]));
    vTaskDelete(NULL);
}
