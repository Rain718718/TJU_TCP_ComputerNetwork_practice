#include "tju_tcp.h"
#include <string.h>

int main(int argc, char **argv) {
    // 开启仿真环境 
    startSimulation();

    tju_tcp_t* my_server = tju_socket();
    // printf("my_tcp state %d\n", my_server->state);
    
    tju_sock_addr bind_addr;
    bind_addr.ip = inet_network("172.17.0.3");
    bind_addr.port = 1234;
    
    if(tju_bind(my_server, bind_addr) != 0 ){
        printf("failed bind sock!\n");
        exit(-1);
    }

    if(tju_listen(my_server) != 0 ){
        printf("failed bind sock!\n");
        exit(-1);
    }
    
    /* printf("my_server state %d\n", my_server->state); */
    /* printf("sock accepting ......\n"); */
    tju_tcp_t* new_conn = tju_accept(my_server);
    /* printf("new_conn state %d\n", new_conn->state);    */   
    /* printf("sock accepted !\n"); */

    // uint32_t conn_ip;
    // uint16_t conn_port;

    // conn_ip = new_conn->established_local_addr.ip;
    // conn_port = new_conn->established_local_addr.port;
    // printf("new_conn established_local_addr ip %d port %d\n", conn_ip, conn_port);

    // conn_ip = new_conn->established_remote_addr.ip;
    // conn_port = new_conn->established_remote_addr.port;
    // printf("new_conn established_remote_addr ip %d port %d\n", conn_ip, conn_port);


    sleep(5);
    
    tju_send(new_conn, "hello world", 12);
    tju_send(new_conn, "hello tju", 10);

    char buf[2021];
    tju_recv(new_conn, (void*)buf, 12);
    printf("server recv %s\n", buf);

    tju_recv(new_conn, (void*)buf, 10);
    printf("server recv %s\n", buf);


    return EXIT_SUCCESS;
}
