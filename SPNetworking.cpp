/*
 * Based on picow_tcp_server and picow_tcp_client examples for the raspberry pi pico C sdk Copyright (c) 2022 Raspberry Pi (Trading) Ltd.
 *
 * Authors: SynthCerb and RachelBirdy
 * Last update: 12/7/2023
 * 
 */

#include "SPNetworking.h"

static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err);
static err_t tcp_con_poll(void *arg, struct tcp_pcb *tpcb);
static err_t tcp_con_sent(void *arg, struct tcp_pcb *tpcb, u16_t len);
static err_t tcp_con_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err);
static void tcp_con_err(void *arg, err_t err);
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err);

SPNetworking::SPNetworking(NET_MODE_T mode, char *ip = (char*)"127.0.0.1", int port = 80, int poll = 5)
{
    this->state = new TCP_CON_T;
    if (cyw43_arch_init()) 
        return;                   //failure to init driver and lwip stack, need error handling
    cyw43_arch_enable_sta_mode();
    this->state->error = 0;
    //this->state = new TCP_CON_T();
    if (!this->state) 
        this->state->error = ERR_MEM; //failed to allocate memory for state
                                      //ideally throw an exception
    strcpy(this->ip_adr, ip);
    this->tcp_port = port;
    this->host_mode = mode;
    this->state->host = this;

    if (this->host_mode == SERVER_MODE) //server mode init
    {
        printf("Server mode init\n");
        struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_V4);
        if (!pcb) this->state->error = ERR_ABRT; //failed to create pcb struct
                                                 //ideally throw an exception
        
        err_t err = tcp_bind(pcb, NULL, this->tcp_port); // binds connection to ip and port 
        if (err) this->state->error = ERR_ABRT; //failed to bind to port
                                                //ideally throw an exception

        this->state->host_pcb = tcp_listen_with_backlog(pcb, 1); // set connection state to listen (server mode), default queue limit of 1
        if (!this->state->host_pcb) 
        {
            if (pcb) tcp_close(pcb);
            this->state->error = ERR_ABRT; //failed to listen
                                           //ideally throw an exception
        }

        this->state->poll_time = poll;
        tcp_arg(this->state->host_pcb, state); // lwip setup
        tcp_accept(this->state->host_pcb, tcp_server_accept); // function to call when listening connection is established
    }

    else //client mode init
    {
        printf("Client mode init\n");
        ip4addr_aton(this->ip_adr, &this->state->remote_addr);
        this->state->remote_pcb = tcp_new_ip_type(IP_GET_TYPE(&this->state->remote_addr));
        if (!this->state->remote_pcb) this->state->error = ERR_ABRT; //failed to create pcb
                                                                     //ideally throw an exception

        //setting up lwip
        tcp_arg(this->state->remote_pcb, this->state); //state to be passed to other callback functions
        tcp_poll(this->state->remote_pcb, tcp_con_poll, poll); //set polling time and polling callback function --> current config sets it as a simple timeout
        tcp_sent(this->state->remote_pcb, tcp_con_sent); //set function on data received by remote host
        tcp_recv(this->state->remote_pcb, tcp_con_recv); //set function on local data recceive 
        tcp_err(this->state->remote_pcb, tcp_con_err); // set function when fatal error occurs on the connection

        this->state->recv_len = 0;

        cyw43_arch_lwip_begin();
        err_t err = tcp_connect(this->state->remote_pcb, &this->state->remote_addr, port, tcp_client_connected); //initialize connection to remote host, function to call on success, on fail calls defined tcp_con_err
        cyw43_arch_lwip_end();
    }
}

SPNetworking::~SPNetworking()
{
    //make cleanup own method and call that instead
    this->end_con();
    delete this->state;
    cyw43_arch_deinit();
}

//blocking listen method, wait for incoming flag, abort if error flag
bool SPNetworking::listen()
{
    do
    {
        #if PICO_CYW43_ARCH_POLL
        // if you are using pico_cyw43_arch_poll, then you must poll periodically from your
        // main loop (not from a timer) to check for Wi-Fi driver or lwIP work that needs to be done.
        cyw43_arch_poll();
        // you can poll as often as you like, however if you have nothing else to do you can
        // choose to sleep until either a specified time, or cyw43_arch_poll() has work to do:
        cyw43_arch_wait_for_work_until(make_timeout_time_ms(500));
        #endif
        if (this->state->error != ERR_OK) {
            error_handler();
            return false;
        }
        sleep_ms(100);
    } while (!this->state->incoming);
    return true;
}

//empty struct buffer and makes content available through tmp_buffer attribute, reset incoming flag
uint8_t* SPNetworking::read()
{
    if (this->state->error != ERR_OK) 
        return NULL; //if registered error, cancel operation
    memset(this->tmp_buf, 0, this->state->recv_len + 1);
    memcpy(this->tmp_buf, this->state->buffer_recv, this->state->recv_len);
    this->state->recv_len = 0;
    this->state->incoming = false;
    return this->tmp_buf;
}

//copy data to send to internal buffer then sends it
bool SPNetworking::send(uint8_t message[])
{
    size_t msg_len = strlen((char*)message);
    if (msg_len > BUF_SIZE) return false;
    memcpy(this->state->buffer_sent, message, msg_len);
    //DEBUG_printf("Writing %ld bytes to client\n", BUF_SIZE);
    
    cyw43_arch_lwip_check();
    err_t err = tcp_write(this->state->remote_pcb, this->state->buffer_sent, BUF_SIZE, TCP_WRITE_FLAG_COPY);
    
    if (err != ERR_OK) //Failed to write data
    {
        this->state->error = err;
        this->end_con();//needed?
        return false;
    }
    return true;
}

//status of the connection, probably best to delete object if error
bool SPNetworking::status()
{
    if (this->state->error == ERR_OK) return true;
    else return false;
}

//properly ends the connection
err_t SPNetworking::end_con()
{
    err_t err = ERR_OK;
    if (this->state->remote_pcb != NULL) 
    {
        tcp_arg(this->state->remote_pcb, NULL);
        tcp_poll(this->state->remote_pcb, NULL, 0);
        tcp_sent(this->state->remote_pcb, NULL);
        tcp_recv(this->state->remote_pcb, NULL);
        tcp_err(this->state->remote_pcb, NULL);
        err = tcp_close(this->state->remote_pcb);
        if (err != ERR_OK)  
            tcp_abort(this->state->remote_pcb);//close failed, calling abort
        this->state->remote_pcb = NULL;
    }

    return err;
}

//timeout function
static err_t tcp_con_poll(void *arg, struct tcp_pcb *tpcb) 
{
    TCP_CON_T *state = (TCP_CON_T*)arg;
    state->error = ERR_TIMEOUT;
    return state->host->end_con();
}

//error handling, ends the connection
static void tcp_con_err(void *arg, err_t err) 
{
    TCP_CON_T *state = (TCP_CON_T*)arg;
    state->error = err;
    state->host->end_con(); //end connection?
}

//function called when data is received
static err_t tcp_con_recv(void *arg, struct tcp_pcb *tpcb, struct pbuf *p, err_t err) 
{
    TCP_CON_T *state = (TCP_CON_T*)arg;
    if (!p) 
    {
        state->error = err;
        //end connection?
        return err;
    }
    
    cyw43_arch_lwip_check();
    if (p->tot_len > 0) 
    {
        // Receive the buffer
        const uint16_t buffer_left = BUF_SIZE - state->recv_len;
        state->recv_len += pbuf_copy_partial(p, state->buffer_recv + state->recv_len,
                                             p->tot_len > buffer_left ? buffer_left : p->tot_len, 0);
        tcp_recved(tpcb, p->tot_len);
    }
    pbuf_free(p);
    state->incoming = true;

    return ERR_OK;
}

//useful only if keeping track of sent data, unsure function needed or not
static err_t tcp_con_sent(void *arg, struct tcp_pcb *tpcb, u16_t len) 
{
    //TCP_CON_T *state = (TCP_CON_T*)arg;
    //DEBUG_printf("tcp_server_sent %u\n", len);
    //state->sent_len += len;

    //if (state->sent_len >= BUF_SIZE) {

        // We should get the data back from the client
        //state->recv_len = 0;
        //DEBUG_printf("Waiting for buffer from client\n");
   // }

    return ERR_OK;
}

//server init on new client connection, follow up with read or send
static err_t tcp_server_accept(void *arg, struct tcp_pcb *client_pcb, err_t err) 
{
    printf("Accepting connection request...\n");
    TCP_CON_T *state = (TCP_CON_T*)arg;
    if (err != ERR_OK || client_pcb == NULL || state->remote_pcb != NULL) //Failure in accept
    {
        printf("Failed to accept\n");
        //end server?
        return ERR_VAL;
    }

    state->remote_pcb = client_pcb;
    tcp_arg(client_pcb, state);
    tcp_sent(client_pcb, tcp_con_sent);
    tcp_recv(client_pcb, tcp_con_recv);
    tcp_poll(client_pcb, tcp_con_poll, state->poll_time * 2);
    tcp_err(client_pcb, tcp_con_err);

    state->incoming = true;
    return ERR_OK;
}

//connection to server successful
static err_t tcp_client_connected(void *arg, struct tcp_pcb *tpcb, err_t err) 
{
    TCP_CON_T *state = (TCP_CON_T*)arg;
    if (err != ERR_OK) //connect failed
    {
        //state->self->end_con(); //needed???
        return err;
    }
    return ERR_OK;
}

err_t SPNetworking::connect_to_wifi(const char* ssid, const char* password) {
    err_t err = 0;
    printf("Connecting to Wi-Fi...\n");
    if (cyw43_arch_wifi_connect_timeout_ms(ssid, password, CYW43_AUTH_WPA2_AES_PSK, 30000)) 
    {
        printf("failed to connect.\n");
        return 1;
    } 
    
    else printf("Connected.\n");
    return err;
}

void SPNetworking::error_handler() 
{
    switch (this->state->error)
    {
        case ERR_OK:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_MEM:
            printf("Out of memory error\n");
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_BUF:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_TIMEOUT:
            printf("Timeout error\n");
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            this->state->error = 0;
            break;
        case ERR_RTE:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_INPROGRESS:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_VAL:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_WOULDBLOCK:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_USE:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_ALREADY:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_ISCONN:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_CONN:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_IF:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_ABRT:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_RST:
            this->state->error = 0;
            break;
        case ERR_CLSD:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            break;
        case ERR_ARG:
            printf("Error handling not implemented\n");
            printf("Error state: %d\n", (int) this->state->error);
            this->state->error = 0;
            break;
    }
}