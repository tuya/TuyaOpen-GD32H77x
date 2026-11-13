#include "at_spi_transport.h"
static unsigned multi_first=0,multi_count=4;
static bool multi_small;
static unsigned multi_gap_ms;
static const char *multi_host="1.1.1.1";
static volatile uint32_t multi_rx_bytes[32],multi_rx_packets[32];
static void multi_observe(void *context,const at_device_event_t *event)
{
    (void)context;
    if(event->kind==AT_DEVICE_EVENT_DATA) {
        int raw=event->value.data.remote_fd;
        if(raw>=0 && raw<32) {
            multi_rx_bytes[raw]+=event->value.data.length;
            if(event->value.data.final)++multi_rx_packets[raw];
        }
    }
}
static void multi_dump(void)
{
    at_spi_transport_diag_t d;at_spi_transport_diag_get(&d);
    gd32h77x_console_printf("NET_DIAG_SPI valid=%u invalid=%u ready_hi=%u ready_lo=%u idle=%u tp=%u rp=%u et=%u phase=%u\r\n",
        (unsigned)d.valid_frame_count,(unsigned)d.invalid_frame_count,(unsigned)d.ready_high_timeout,
        (unsigned)d.ready_low_timeout,(unsigned)d.ready_idle_timeout,(unsigned)d.spi_tp_timeout,
        (unsigned)d.spi_rp_timeout,(unsigned)d.spi_et_timeout,d.link_phase);
    for(unsigned raw=0;raw<32;++raw)if(multi_rx_bytes[raw])
        gd32h77x_console_printf("NET_DIAG_RX raw=%u packets=%u bytes=%u\r\n",raw,
            (unsigned)multi_rx_packets[raw],(unsigned)multi_rx_bytes[raw]);
}
/* Diagnostic only: keep multiple sockets alive and verify response routing. */
static void multi_task(void *arg)
{
    (void)arg;
    uint32_t addr=0;
    unsigned passed=0,failed=0,closed=0,idle_checks=0,select_calls=0;
    memset((void*)multi_rx_bytes,0,sizeof(multi_rx_bytes));
    memset((void*)multi_rx_packets,0,sizeof(multi_rx_packets));
    multi_dump();
    size_t heap_start=xPortGetFreeHeapSize();
    bool resolved=at_socket_gethostbyname(multi_host,&addr)==AT_STATUS_OK;
    if(!resolved)++failed;
    for(unsigned batch=1;resolved && batch<=3;++batch) {
        int fd[4]={-1,-1,-1,-1};bool healthy=true;
        for(unsigned slot=multi_first;slot<multi_first+multi_count;++slot) {
            fd[slot]=at_socket_create(slot<2?AT_SOCKET_TYPE_STREAM:AT_SOCKET_TYPE_DATAGRAM);
            if(fd[slot]<0) {healthy=false;++failed;break;}
            at_socket_set_timeout(fd[slot],5000,AT_SOCKET_TRANSFER_SEND);
            at_socket_set_timeout(fd[slot],5000,AT_SOCKET_TRANSFER_RECV);
            if(at_socket_connect(fd[slot],addr,53)!=AT_SOCKET_ERRNO_NONE) {
                healthy=false;++failed;break;
            }
            gd32h77x_console_printf("NET_MULTI_OPEN batch=%u slot=%u fd=%d type=%s\r\n",
                batch,slot,fd[slot],slot<2?"TCP":"UDP");
        }
        if(healthy) {
            execute("AT+CIPSTATE?",5000);
            at_socket_fd_set_t reads={0};int bound=0;
            for(unsigned slot=0;slot<4;++slot) {
                if(fd[slot]<0)continue;
                reads.bits[fd[slot]/8]|=(uint8_t)(1U<<(fd[slot]%8));
                if(fd[slot]>=bound)bound=fd[slot]+1;
            }
            int rc=at_socket_select(bound,&reads,NULL,NULL,200);++select_calls;
            if(rc==0)++idle_checks;else {++failed;healthy=false;}
            gd32h77x_console_printf("NET_MULTI_IDLE batch=%u rc=%d %s\r\n",batch,rc,rc==0?"PASS":"FAIL");
        }
        for(unsigned round=1;healthy && round<=5;++round) {
            bool pending[4]={false,false,false,false};unsigned remaining=0;
            uint8_t ids[4];
            for(unsigned slot=0;slot<4;++slot) {
                if(fd[slot]<0)continue;
                uint8_t root_query[]={0x75,0,1,0,0,1,0,0,0,0,0,1,
                    0,0,48,0,1,0,0,41,4,208,0,0,0,0,0,0};
                uint8_t small_query[]={0x75,0,1,0,0,1,0,0,0,0,0,0,
                    7,'e','x','a','m','p','l','e',3,'c','o','m',0,0,1,0,1};
                uint8_t *query=multi_small?small_query:root_query;
                size_t query_len=multi_small?sizeof(small_query):sizeof(root_query);
                ids[slot]=(uint8_t)(batch*32+round*4+slot);query[1]=ids[slot];
                uint8_t prefix[2]={0,(uint8_t)query_len};
                bool ok=slot<2 ? transfer(fd[slot],prefix,2,true) && transfer(fd[slot],query,query_len,true) :
                    at_socket_sendto(fd[slot],query,query_len,addr,53)==(int)query_len;
                if(ok) {pending[slot]=true;++remaining;}
                else {++failed;healthy=false;}
                if(multi_gap_ms)vTaskDelay(pdMS_TO_TICKS(multi_gap_ms));
            }
            TickType_t started=xTaskGetTickCount();
            while(remaining && (xTaskGetTickCount()-started)<pdMS_TO_TICKS(8000)) {
                at_socket_fd_set_t reads={0},errors={0};int bound=0;
                for(unsigned slot=0;slot<4;++slot)if(pending[slot]) {
                    reads.bits[fd[slot]/8]|=(uint8_t)(1U<<(fd[slot]%8));
                    if(fd[slot]>=bound)bound=fd[slot]+1;
                }
                errors=reads;
                int rc=at_socket_select(bound,&reads,NULL,&errors,500);++select_calls;
                if(rc<0)break;
                for(unsigned slot=0;slot<4;++slot)if(pending[slot] &&
                    ((reads.bits[fd[slot]/8]|errors.bits[fd[slot]/8])&(1U<<(fd[slot]%8)))) {
                    uint8_t rx[1536],prefix[2];size_t got=0;bool ok;
                    if(slot<2) {
                        ok=transfer(fd[slot],prefix,2,false);
                        if(ok) {
                            got=((unsigned)prefix[0]<<8)|prefix[1];
                            ok=got>=17 && got<=sizeof(rx) && transfer(fd[slot],rx,got,false);
                        }
                    } else {
                        uint32_t peer=0;uint16_t port=0;
                        int n=at_socket_recvfrom(fd[slot],rx,sizeof(rx),&peer,&port);
                        ok=n>0 && peer==addr && port==53;
                        if(n>0)got=(size_t)n;
                    }
                    ok=ok && got>=(multi_small?29U:241U) && got<=sizeof(rx) && rx[0]==0x75 && rx[1]==ids[slot] &&
                        (rx[2]&0xfa)==0x80 && (rx[3]&15)==0 && rx[4]==0 && rx[5]==1 &&
                        (rx[6] || rx[7]) && (multi_small?(!memcmp(rx+12,"\x07" "example" "\x03" "com" "\0\0\1\0\1",17)):
                        (rx[12]==0 && rx[13]==0 && rx[14]==48 && rx[15]==0 && rx[16]==1));
                    if(ok)++passed;else {++failed;healthy=false;}
                    pending[slot]=false;--remaining;
                    gd32h77x_console_printf("NET_MULTI batch=%u round=%u slot=%u fd=%d type=%s rx=%u %s\r\n",
                        batch,round,slot,fd[slot],slot<2?"TCP":"UDP",(unsigned)got,ok?"PASS":"FAIL");
                }
            }
            if(remaining) {failed+=remaining;healthy=false;gd32h77x_console_printf("NET_MULTI_TIMEOUT batch=%u round=%u pending=%u\r\n",batch,round,remaining);multi_dump();}
            /* Verify the other three connections survive closing the first TCP. */
            if(round==3 && fd[0]>=0) {
                int rc=at_socket_close(fd[0]);
                if(rc==AT_SOCKET_ERRNO_NONE)++closed;else {++failed;healthy=false;}
                gd32h77x_console_printf("NET_MULTI_CLOSE batch=%u slot=0 rc=%d\r\n",batch,rc);
                fd[0]=-1;
            }
        }
        for(unsigned slot=0;slot<4;++slot)if(fd[slot]>=0) {
            int rc=at_socket_close(fd[slot]);
            if(rc==AT_SOCKET_ERRNO_NONE)++closed;else ++failed;
            gd32h77x_console_printf("NET_MULTI_CLOSE batch=%u slot=%u rc=%d\r\n",batch,slot,rc);
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        gd32h77x_console_printf("NET_MULTI_BATCH batch=%u heap=%u stack_free=%u\r\n",batch,
            (unsigned)xPortGetFreeHeapSize(),(unsigned)uxTaskGetStackHighWaterMark(NULL));
    }
    gd32h77x_console_printf("NET_MULTI_RESULT pass=%u expected=%u fail=%u closed=%u idle=%u select_calls=%u heap_start=%u heap_end=%u\r\n",
        passed,multi_first==0?54U:multi_count*15U,failed,closed,idle_checks,select_calls,(unsigned)heap_start,(unsigned)xPortGetFreeHeapSize());
    multi_dump();
    running=false;vTaskDelete(NULL);
}
static void multi(int argc,char **argv)
{
    (void)argc;(void)argv;
    if(running) {gd32h77x_console_printf("NET_BUSY\r\n");return;}
    multi_first=0;multi_count=4;multi_small=false;multi_gap_ms=0;multi_host="1.1.1.1";
    running=true;
    if(xTaskCreate(multi_task,"net_multi",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
static void multi_diag(int argc,char **argv)
{
    if(running || argc!=2 || strlen(argv[1])!=1 || argv[1][0]<'0' || argv[1][0]>'6')return;
    unsigned mode=(unsigned)(argv[1][0]-'0');
    multi_first=(mode==1 || mode==2 || mode>=4)?2:0;
    multi_count=mode==4?1:(multi_first?2:4);
    multi_small=mode==2 || mode==3;
    multi_gap_ms=mode==5?1000:0;
    multi_host=mode==6?"1.0.0.1":"1.1.1.1";
    gd32h77x_console_printf("NET_DIAG_MODE mode=%u first=%u count=%u small=%u gap=%u host=%s\r\n",mode,multi_first,multi_count,multi_small,multi_gap_ms,multi_host);
    running=true;
    if(xTaskCreate(multi_task,"net_diag",4096,NULL,2,NULL)!=pdPASS) {
        running=false;gd32h77x_console_printf("NET_TASK_CREATE_FAIL\r\n");
    }
}
