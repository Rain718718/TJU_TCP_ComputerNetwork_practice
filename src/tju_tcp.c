#include "tju_tcp.h"

#define SYN_SEQ 100
#define SYN_ACK 0
#define SYNACK_SEQ 300


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
    while(listen_sock->state == LISTEN){}
    //跳出后状态为SYN-RECEIVED
    /* printf("第一次握手成功\n"); */

    /* printf("发出第二次握手\n"); */
    //发起第二次握手
    char * synack = create_packet_buf(listen_sock->bind_addr.port, listen_sock->established_remote_addr.port, SYNACK_SEQ ,SYN_SEQ+1,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , SYNACK_FLAG_MASK, 1, 0, NULL, 0);
    sendToLayer3(synack,DEFAULT_HEADER_LEN);

    /* printf("等待第三次握手\n"); */
    //等待对方发起的第三次握手
    while(listen_sock->state == SYN_RECV){}
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
    connecting_sock = sock;
    sock->established_remote_addr = target_addr;
    tju_sock_addr local_addr;
    local_addr.ip = inet_network("172.17.0.2");
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;


    //先发送一个SYN段，<SEQ=100><CTL=SYN> ，第一次握手
    /* printf("发出第一次握手\n"); */
    char * syn = create_packet_buf(local_addr.port, target_addr.port, SYN_SEQ ,SYN_ACK,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , SYN_FLAG_MASK, 1, 0, NULL, 0);
    sendToLayer3(syn,DEFAULT_HEADER_LEN);
    sock->state = SYN_SENT;
    
    /* printf("等待第二次握手\n"); */
    //等待客户端的SYNACK段，等待对方发出第二次握手
    while(sock->state == SYN_SENT){} 
    //得到回应后变为建立状态也就是ESTABLISHED
    /* printf("第二次握手成功\n"); */

    //再发送一个ack,发起第三次握手
    /* printf("发出第三次握手\n"); */
    char * ack = create_packet_buf(local_addr.port, target_addr.port, SYN_SEQ+1 ,SYNACK_SEQ+1,
        DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN , ACK_FLAG_MASK, 1, 0, NULL, 0);
    sendToLayer3(ack,DEFAULT_HEADER_LEN);
    // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet
    
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
    //如果是一个握手包则要更改sock的状态
    uint8_t pktflag = get_flags(pkt);
    uint16_t remote_port = get_src(pkt);

    //如果是第一次握手包，说明sock是listensock，此时把远端的ip和端口填写
    if(pktflag == SYN_FLAG_MASK){
        sock->established_remote_addr.port = remote_port;
        sock->state = SYN_RECV;
        return 0; 
    }

    //如果是第二次握手包，说明sock是客户端的连接sock，此时内核的sock已经提前建立连接
    if(pktflag == SYNACK_FLAG_MASK){
        /* printf("client handling SYNACK\n"); */
        sock->state = ESTABLISHED;
        return 0; 
    }

    //如果是第三次握手包，说明sock是newsock.此时内核的sock已经提前建立连接
    if(pktflag == ACK_FLAG_MASK){
        /* printf("server handling ACK\n"); */
        sock->state = ESTABLISHED;
        tju_sock_addr local_addr, remote_addr;
        /*
        这里涉及到TCP连接的建立
        正常来说应该是收到客户端发来的SYN报文
        从中拿到对端的IP和PORT
        换句话说 下面的处理流程其实不应该放在这里 应该在tju_handle_packet中
        */ 
        tju_tcp_t* new_conn = connecting_sock;
        remote_addr.ip = inet_network("172.17.0.2");  //具体的IP地址
        remote_addr.port = 5678;  //端口
        local_addr.ip = sock->bind_addr.ip;  //具体的IP地址
        local_addr.port = sock->bind_addr.port;  //端口
        new_conn->established_local_addr = local_addr;
        new_conn->established_remote_addr = remote_addr;
        // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
        // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
        // 每次调用accept 实际上就是取出这个队列中的一个元素
        // 队列为空,则阻塞 
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