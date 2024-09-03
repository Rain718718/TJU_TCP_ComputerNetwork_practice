#include "tju_tcp.h"

#define SYN_SEQ 100
#define SYN_ACK 0
#define SYNACK_SEQ 300

char * syn = NULL;
char * synack = NULL;
char * ack = NULL;

/*
创建 TCP socket 
初始化对应的结构体
设置初始状态为 CLOSED
*/
tju_tcp_t* tju_socket(){
    tju_tcp_t* sock = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;
    
    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;
    
    if(pthread_cond_init(&sock->wait_cond, NULL) != 0){
        perror("ERROR condition variable not set\n");
        exit(-1);
    }

    sock->window.wnd_send = NULL;
    sock->window.wnd_recv = NULL;

    return sock;
}

/*
绑定监听的地址 包括ip和端口
*/
int tju_bind(tju_tcp_t* sock, tju_sock_addr bind_addr){
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t* sock){
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接 
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t* tju_accept(tju_tcp_t* listen_sock){
    //确保建立连接时的初始状态
    if(listen_sock->state != LISTEN){
        printf("The listen_sock state is not LISTEN while calling tju_accep!\n");
        exit(-1);
    }
    tju_tcp_t* new_conn = (tju_tcp_t*)malloc(sizeof(tju_tcp_t));
    memcpy(new_conn, listen_sock, sizeof(tju_tcp_t));
    connecting_sock = new_conn;

    
    /* printf("等待第一次握手\n"); */
    //等待对方发起的第一次握手
    //不用计时，因为SYN发生丢包，server阻塞，一直不发SYNACK，client等待超时就会重发SYN
    while(listen_sock->state == LISTEN){}
    //跳出后状态为SYN-RECEIVED
    /* printf("第一次握手成功\n"); */

    //收到SYN后自动发起第二次握手synack
    

    /* printf("等待第三次握手\n"); */
    //等待对方发起的第三次握手
    time_t start_time, end_time;
    double time_diff;
    time(&start_time);
    while(listen_sock->state == SYN_RECV){
        time(&end_time);
        // 计算时间间隔
        time_diff = difftime(end_time, start_time);
        if( time_diff > 0.002 ){ //超时重传，阈值为2s
            printf("retrans synack!\n");
            sendToLayer3(synack,DEFAULT_HEADER_LEN);
            time(&start_time);
        }
    }
    //跳出后状态为ESTABLISHED
    /* printf("第三次握手成功\n"); */

    //此后listensock要重新回到侦听状态
    listen_sock->state = LISTEN;
    new_conn->state = ESTABLISHED;
    return new_conn;
}


/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t* sock, tju_sock_addr target_addr){
    if(sock->state != CLOSED){
        printf("Sock is CLOSED while calling connect!");
        exit(-1);
    }

    //初始化客户端的sock的本地地址和远程地址
    connecting_sock = sock;
    sock->established_remote_addr = target_addr;
    tju_sock_addr local_addr;
    local_addr.ip = inet_network("172.17.0.2");
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;


    //先发送一个SYN段，<SEQ=100><CTL=SYN> ，第一次握手
    /* printf("发出第一次握手\n"); */
    syn = create_packet_buf(local_addr.port, target_addr.port, SYN_SEQ ,SYN_ACK,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , SYN_FLAG_MASK, 1, 0, NULL, 0);
    sendToLayer3(syn,DEFAULT_HEADER_LEN);
    sock->state = SYN_SENT;
    
    /* printf("等待第二次握手\n"); */
    //等待客户端的SYNACK段，等待对方发出第二次握手
    time_t start_time, end_time;
    double time_diff;
    time(&start_time);
    while(sock->state == SYN_SENT){
        // 计算时间间隔
        time(&end_time);
        time_diff = difftime(end_time, start_time);
        if( time_diff > 0.002 ){ //超时重传，阈值为2ms
            sendToLayer3(syn,DEFAULT_HEADER_LEN);
            printf("retrans syn!\n");
            time(&start_time);
        }
    } 
    //得到回应后变为建立状态也就是ESTABLISHED

    //在收到SYNACK后会自动在处理数据包时发起第三次握手

    return 0;
}

int tju_send(tju_tcp_t* sock, const void *buffer, int len){
    // 这里当然不能直接简单地调用sendToLayer3
    char* data = malloc(len);
    memcpy(data, buffer, len);

    char* msg;
    uint32_t seq = 464;
    uint16_t plen = DEFAULT_HEADER_LEN + len;

    msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0, 
              DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);

    sendToLayer3(msg, plen);
    
    return 0;
}
int tju_recv(tju_tcp_t* sock, void *buffer, int len){
    while(sock->received_len<=0){
        // 阻塞
    }

    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    int read_len = 0;
    if (sock->received_len >= len){ // 从中读取len长度的数据
        read_len = len;
    }else{
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if(read_len < sock->received_len) { // 还剩下一些
        char* new_buf = malloc(sock->received_len - read_len);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }else{
        free(sock->received_buf);
        sock->received_buf = NULL;
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

int tju_handle_packet(tju_tcp_t* sock, char* pkt){
    
    uint8_t pktflag = get_flags(pkt);
    uint16_t remote_port = get_src(pkt);
    uint32_t seq_num = get_seq(pkt); 	
    uint32_t ack_num = get_ack(pkt); 	

   /*  printf("ack包的seq %d\n",seq_num);
    printf("ack包的ack %d\n",ack_num);
    printf("ack包的flag %d\n",pktflag);	
    printf("sock state before %d\n",sock->state);
 */
    //如果是第一次握手包SYN，说明sock是listensock，此时把远端的ip和端口填写
    if(pktflag == SYN_FLAG_MASK){
        //if是为了防止收到duplicate SYN
        if(sock->state == LISTEN){
             /* printf("server handling SYN\n"); */
            sock->established_remote_addr.port = remote_port;
            sock->state = SYN_RECV;
        }
        //收到SYN，则再发一次SYNACK
        	
        if(synack == NULL){ //初次发送才要建包
            synack = create_packet_buf(sock->bind_addr.port, sock->established_remote_addr.port, SYNACK_SEQ ,seq_num + 1,
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , SYNACK_FLAG_MASK, 1, 0, NULL, 0);
        }
        sendToLayer3(synack,DEFAULT_HEADER_LEN);
        return 0;
    }

    //如果是第二次握手包SYNACK，说明sock是客户端的连接sock，此时内核的sock已经提前建立连接
    if(pktflag == SYNACK_FLAG_MASK){
        //if是为了防止出现收到duplicate SYNACK
        if(sock->state == SYN_SENT){
            /* printf("client handling SYNACK\n"); */
            sock->state = ESTABLISHED;
        }
        //出现收到SYNACK,重发ack
        
        if(ack == NULL){ //初次发送才要建包
            ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, SYN_SEQ+1 , seq_num + 1	,
                DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , ACK_FLAG_MASK, 1, 0, NULL, 0);
        }
        sendToLayer3(ack,DEFAULT_HEADER_LEN);
        return 0;
        
    }

    //如果是第三次握手包ACK，说明sock是listensock.此时内核的sock已经提前建立连接
    if(pktflag == ACK_FLAG_MASK){
        //if是为了防止出现收到duplicate ACK
        
        if(sock->state == SYN_RECV){
            /* printf("server handling ACK\n"); */
            sock->state = ESTABLISHED;
            tju_sock_addr local_addr, remote_addr;
            tju_tcp_t* new_conn = connecting_sock;
            remote_addr.ip = inet_network("172.17.0.2");  //具体的IP地址
            remote_addr.port = 5678;  //端口
            local_addr.ip = sock->bind_addr.ip;  //具体的IP地址
            local_addr.port = sock->bind_addr.port;  //端口
            new_conn->established_local_addr = local_addr;
            new_conn->established_remote_addr = remote_addr;
        }
        return 0; 
    }





    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;

    // 把收到的数据放到接受缓冲区
    while(pthread_mutex_lock(&(sock->recv_lock)) != 0); // 加锁

    if(sock->received_buf == NULL){
        sock->received_buf = malloc(data_len);
    }else {
        sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
    }
    memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
    sock->received_len += data_len;

    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁


    return 0;
}

int tju_close (tju_tcp_t* sock){
    return 0;
}