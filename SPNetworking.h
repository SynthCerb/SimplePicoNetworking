// Type headers
//#include <string>
//#include <iostream>

extern "C" 
{
    #include <string.h>
    #include <stdlib.h>
    #include <time.h>
    #include "pico/stdlib.h"
    #include "pico/cyw43_arch.h"
    #include "lwip/pbuf.h"
    #include "lwip/tcp.h"
}

#ifndef SP_NETWORKING_H
#define SP_NETWORKING_H

#define BUF_SIZE 2048

class SPNetworking;

typedef struct TCP_CON_T_ 
{
    struct tcp_pcb *host_pcb; //protocol control block struts, handled by lwip
    struct tcp_pcb *remote_pcb;
    ip_addr_t remote_addr;
    uint8_t buffer_sent[BUF_SIZE];
    uint8_t buffer_recv[BUF_SIZE];
    uint16_t recv_len;
    uint16_t poll_time;
    SPNetworking* host;
    bool incoming = false;
    err_t error = ERR_OK;
} TCP_CON_T;

enum NET_MODE_T 
{
    CLIENT_MODE,
    SERVER_MODE
};

class SPNetworking
{
    public:
        SPNetworking(NET_MODE_T mode, char *ip, int port, int poll);
        ~SPNetworking();

        bool listen();
        uint8_t* read();
        bool send(uint8_t message[]);
        bool status();
        err_t end_con();
        err_t connect_to_wifi(const char* ssid, const char* password);

    private:
        void error_handler();
        char ip_adr[15]; //assumes IPv4 AAA.BBB.CCC.DDD
        uint16_t tcp_port;
        TCP_CON_T *state;
        uint8_t tmp_buf[BUF_SIZE];
        NET_MODE_T host_mode;
};

#endif