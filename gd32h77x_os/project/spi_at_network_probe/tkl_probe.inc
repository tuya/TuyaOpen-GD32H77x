#include "tkl_network.h"
#include "tuya_error_code.h"
static char tkl_probe_host[16]="1.1.1.1";
static bool tkl_transfer(int fd,uint8_t *data,size_t length,bool send)
{
    size_t done=0;
    while(done<length) {
        int n=send?tkl_net_send(fd,data+done,length-done):tkl_net_recv(fd,data+done,length-done);
        if(n<=0 || (size_t)n>length-done)return false;
        done+=(size_t)n;
    }
    return true;
}
static void tkl_task(void *arg)
{
    (void)arg;
    unsigned passed=0,failed=0;size_t heap_start=xPortGetFreeHeapSize();
    TUYA_IP_ADDR_T domain_ip=0,addr=tkl_net_str2addr(tkl_probe_host);
    bool ok=tkl_net_gethostbyname("example.com",&domain_ip)==OPRT_OK && domain_ip && domain_ip!=UINT32_MAX;
    if(ok)++passed;else ++failed;
    gd32h77x_console_printf("TKL_DNS %s\r\n",ok?"PASS":"FAIL");
    ok=addr!=UINT32_MAX && !strcmp(tkl_net_addr2str(addr),tkl_probe_host);
    if(ok)++passed;else ++failed;
    gd32h77x_console_printf("TKL_ADDRESS %s\r\n",ok?"PASS":"FAIL");
    ok=tkl_net_close(-1)==UNW_EBADF;
    if(ok)++passed;else ++failed;
    gd32h77x_console_printf("TKL_ERRNO %s\r\n",ok?"PASS":"FAIL");
    for(unsigned round=1;round<=3;++round)for(unsigned type=0;type<2;++type)for(unsigned k=0;k<4;++k) {
        const unsigned sizes[]={239,240,241,1024};unsigned length=sizes[k];
        uint8_t query[1024]={0x78,0,1,0,0,1,0,0,0,0,0,1,
            0,0,48,0,1,0,0,41,4,208,0,0,0,0,0,0};
        unsigned rdlen=length-28,padlen=length-32;
        query[1]=(uint8_t)(round*8+type*4+k);
        query[26]=(uint8_t)(rdlen>>8);query[27]=(uint8_t)rdlen;
        query[29]=12;query[30]=(uint8_t)(padlen>>8);query[31]=(uint8_t)padlen;
        uint8_t rx[1536],prefix[2]={(uint8_t)(length>>8),(uint8_t)length};
        size_t got=0;int fd=tkl_net_socket_create(type?PROTOCOL_UDP:PROTOCOL_TCP);
        ok=fd>=0;
        if(ok) {
            ok=tkl_net_set_timeout(fd,8000,TRANS_SEND)==OPRT_OK &&
                tkl_net_set_timeout(fd,8000,TRANS_RECV)==OPRT_OK &&
                tkl_net_connect(fd,addr,53)==UNW_SUCCESS;
            if(ok && type) {
                uint8_t byte;TUYA_IP_ADDR_T peer=0;uint16_t port=0;
                ok=tkl_net_set_block(fd,FALSE)==OPRT_OK && tkl_net_get_nonblock(fd)==1 &&
                    tkl_net_recvfrom(fd,&byte,1,&peer,&port)==UNW_EAGAIN;
                bool restored=tkl_net_set_block(fd,TRUE)==OPRT_OK && tkl_net_get_nonblock(fd)==0;
                ok=ok && restored;
            }
            TUYA_FD_SET_T reads;
            if(ok) {
                ok=tkl_net_fd_zero(&reads)==OPRT_OK && tkl_net_fd_set(fd,&reads)==OPRT_OK &&
                    tkl_net_select(fd+1,&reads,NULL,NULL,50)==0;
            }
            if(ok)ok=type?tkl_net_send_to(fd,query,length,addr,53)==(int)length:
                tkl_transfer(fd,prefix,2,true) && tkl_transfer(fd,query,length,true);
            if(ok) {
                tkl_net_fd_zero(&reads);tkl_net_fd_set(fd,&reads);
                ok=tkl_net_select(fd+1,&reads,NULL,NULL,8000)>0 && tkl_net_fd_isset(fd,&reads)>0;
            }
            if(ok && type) {
                TUYA_IP_ADDR_T peer=0;uint16_t port=0;
                int n=tkl_net_recvfrom(fd,rx,sizeof(rx),&peer,&port);
                ok=n>0 && peer==addr && port==53;
                if(n>0)got=(size_t)n;
            } else if(ok) {
                ok=tkl_transfer(fd,prefix,2,false);
                if(ok) {
                    got=((unsigned)prefix[0]<<8)|prefix[1];
                    ok=got>=17 && got<=sizeof(rx) && tkl_transfer(fd,rx,got,false);
                }
            }
            ok=ok && got>240 && got<=sizeof(rx) && rx[0]==query[0] && rx[1]==query[1] &&
                (rx[2]&0xfa)==0x80 && (rx[3]&15)==0 && rx[4]==0 && rx[5]==1 &&
                (rx[6] || rx[7]) && !memcmp(rx+12,query+12,5);
            int close_rc=tkl_net_close(fd);
            if(close_rc!=UNW_SUCCESS)ok=false;
        }
        if(ok)++passed;else ++failed;
        gd32h77x_console_printf("TKL_QUERY round=%u type=%s tx=%u rx=%u %s errno=%d\r\n",
            round,type?"UDP":"TCP",length,(unsigned)got,ok?"PASS":"FAIL",ok?0:(int)tkl_net_get_errno());
    }
    gd32h77x_console_printf("TKL_RESULT pass=%u expected=27 fail=%u heap_start=%u heap_end=%u stack_free=%u\r\n",
        passed,failed,(unsigned)heap_start,(unsigned)xPortGetFreeHeapSize(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    running=false;vTaskDelete(NULL);
}
static void tkl_probe(int argc,char **argv)
{
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    const char *host=argc==2?argv[1]:"1.1.1.1";
    if(argc>2 || strlen(host)>=sizeof(tkl_probe_host) || tkl_net_str2addr(host)==UINT32_MAX)return;
    strcpy(tkl_probe_host,host);
    gd32h77x_console_printf("TKL_TARGET %s:53\r\n",tkl_probe_host);
    running=true;
    if(xTaskCreate(tkl_task,"tkl_probe",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
