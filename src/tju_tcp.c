#include "tju_tcp.h"

#define DEFAULT_WAIT_TIME 1
/*
创建 TCP socket
初始化对应的结构体
设置初始状态为 CLOSED
*/
char * syn = NULL;
char * synack = NULL;
char * ack = NULL;



tju_tcp_t *tju_socket()
{
    tju_tcp_t *sock = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
    sock->state = CLOSED;

    pthread_mutex_init(&(sock->send_lock), NULL);
    sock->sending_buf = NULL;
    sock->sending_len = 0;

    pthread_mutex_init(&(sock->recv_lock), NULL);
    sock->received_buf = NULL;
    sock->received_len = 0;

    if (pthread_cond_init(&sock->wait_cond, NULL) != 0)
    {
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
int tju_bind(tju_tcp_t *sock, tju_sock_addr bind_addr)
{
    sock->bind_addr = bind_addr;
    return 0;
}

/*
被动打开 监听bind的地址和端口
设置socket的状态为LISTEN
注册该socket到内核的监听socket哈希表
*/
int tju_listen(tju_tcp_t *sock)
{
    sock->state = LISTEN;
    int hashval = cal_hash(sock->bind_addr.ip, sock->bind_addr.port, 0, 0);
    tju_sock_queue *new_queue = (tju_sock_queue *)malloc(sizeof(tju_sock_queue));
    socks_queue = new_queue;
    for (int i = 0; i < QUEUE_LEN; i++)
    {
        socks_queue->accept_queue[i] = NULL;
    }
    for (int i = 0; i < QUEUE_LEN; i++)
    {
        socks_queue->syns_queue[i] = NULL;
    }
    listen_socks[hashval] = sock;
    return 0;
}

/*
接受连接
返回与客户端通信用的socket
这里返回的socket一定是已经完成3次握手建立了连接的socket
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
tju_tcp_t *tju_accept(tju_tcp_t *listen_sock)
{
    while (1)
    {
        // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
        // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
        // 每次调用accept 实际上就是取出这个队列中的一个元素
        // 队列为空,则阻塞
        
        //检查sock队列中的半连接sock，如果出现长时间未进入accept_queue,判断出现了ack包的丢失，重传SYNACK
        for(int i = 0 ; i < QUEUE_LEN ; i ++ ){
            if( socks_queue->syns_queue[i] != NULL){  //定位到每个半连接sock
                time_t now_time;
                double time_diff;  
                time(&now_time);  //获取当下时间
                
                time_diff = difftime(now_time, socks_queue->syns_queue_wait[i]); //计算时间差
                if(time_diff > DEFAULT_WAIT_TIME){    //等到超过2ms，重传SYNACK
                    sendToLayer3(synack,20);
                    printf("ACK lost,重传SYNACK\n");
                    time(&socks_queue->syns_queue_wait[i]); //重新计时
                }
            }
        }  //确保半连接队列每个元素没有等待太久，及时重传

        if (socks_queue->accept_queue[0] != NULL)
        {
            // 拿出一个socket(已经建立好连接)
            tju_tcp_t *return_socket = acc_pop(socks_queue);
            int return_sock_hashval = cal_hash(return_socket->established_local_addr.ip, return_socket->established_local_addr.port, return_socket->established_remote_addr.ip, return_socket->established_remote_addr.port);
            // 把这个socket注册到EST里面,这里的hash为新socket的hash
            established_socks[return_sock_hashval] = return_socket;
            printf("阻塞结束\n");

            return return_socket;
        }
        
        


    }
    tju_tcp_t *new_conn = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
    memcpy(new_conn, listen_sock, sizeof(tju_tcp_t));

    tju_sock_addr local_addr, remote_addr;
    /*
     这里涉及到TCP连接的建立
     正常来说应该是收到客户端发来的SYN报文
     从中拿到对端的IP和PORT
     换句话说 下面的处理流程其实不应该放在这里 应该在tju_handle_packet中
    */
    remote_addr.ip = inet_network("172.17.0.2"); // 具体的IP地址
    remote_addr.port = 5678;                     // 端口

    local_addr.ip = listen_sock->bind_addr.ip;     // 具体的IP地址
    local_addr.port = listen_sock->bind_addr.port; // 端口

    new_conn->established_local_addr = local_addr;
    new_conn->established_remote_addr = remote_addr;

    // 这里应该是经过三次握手后才能修改状态为ESTABLISHED
    new_conn->state = ESTABLISHED;

    // 将新的conn放到内核建立连接的socket哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, remote_addr.ip, remote_addr.port);
    established_socks[hashval] = new_conn;

    // 如果new_conn的创建过程放到了tju_handle_packet中 那么accept怎么拿到这个new_conn呢
    // 在linux中 每个listen socket都维护一个已经完成连接的socket队列
    // 每次调用accept 实际上就是取出这个队列中的一个元素
    // 队列为空,则阻塞
    return new_conn;
}

/*
连接到服务端
该函数以一个socket为参数
调用函数前, 该socket还未建立连接
函数正常返回后, 该socket一定是已经完成了3次握手, 建立了连接
因为只要该函数返回, 用户就可以马上使用该socket进行send和recv
*/
int tju_connect(tju_tcp_t *sock, tju_sock_addr target_addr)
{

    sock->established_remote_addr = target_addr;

    tju_sock_addr local_addr;
    local_addr.ip = inet_network("172.17.0.2");
    local_addr.port = 5678; // 连接方进行connect连接的时候 内核中是随机分配一个可用的端口
    sock->established_local_addr = local_addr;

    /* // 这里也不能直接建立连接 需要经过三次握手
    // 实际在linux中 connect调用后 会进入一个while循环
    // 循环跳出的条件是socket的状态变为ESTABLISHED 表面看上去就是 正在连接中 阻塞
    // 而状态的改变在别的地方进行 在我们这就是tju_handle_packet
    sock->state = ESTABLISHED;*/

    // 将建立了连接的socket放入内核 已建立连接哈希表中
    int hashval = cal_hash(local_addr.ip, local_addr.port, target_addr.ip, target_addr.port);
    established_socks[hashval] = sock;
    
    // 使用UDP发送SYN报文 SYN = 1 ，ACK = 0 ， seq = 0
    // 随后进入SYN_SENT
    
    syn = create_packet_buf(local_addr.port, target_addr.port, CLIENT_ISN, 0, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, SYN_FLAG_MASK, 1, 0, NULL, 0);
    printf("Client 端发送了第一次握手\n");
    sendToLayer3(syn, 20);
    sock->state = SYN_SENT;

    time_t start_time, end_time;
    double time_diff = 0;
    time(&start_time); //开始计时
    while (sock->state != ESTABLISHED)
    {
        // 阻塞
        time(&end_time);  //结束计时
        
        time_diff = difftime(end_time, start_time); //计算时间差
        
        if( time_diff > DEFAULT_WAIT_TIME ){ //超时重传，阈值为2ms
            printf("Client 重传了 SYN!\n");
            sendToLayer3(syn,DEFAULT_HEADER_LEN);
            time(&start_time); //重新计时
        }
    }
    printf("阻塞结束\n");

    return 0;
}

int tju_send(tju_tcp_t *sock, const void *buffer, int len)
{
    // 这里当然不能直接简单地调用sendToLayer3
    char *data = malloc(len);
    memcpy(data, buffer, len);

    char *msg;
    uint32_t seq = 464;
    uint16_t plen = DEFAULT_HEADER_LEN + len;

    msg = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, seq, 0,
                            DEFAULT_HEADER_LEN, plen, NO_FLAG, 1, 0, data, len);

    sendToLayer3(msg, plen);

    return 0;
}
int tju_recv(tju_tcp_t *sock, void *buffer, int len)
{
    while (sock->received_len <= 0)
    {
        // 阻塞
    }

    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
        ; // 加锁

    int read_len = 0;
    if (sock->received_len >= len)
    { // 从中读取len长度的数据
        read_len = len;
    }
    else
    {
        read_len = sock->received_len; // 读取sock->received_len长度的数据(全读出来)
    }

    memcpy(buffer, sock->received_buf, read_len);

    if (read_len < sock->received_len)
    { // 还剩下一些
        char *new_buf = malloc(sock->received_len - read_len);
        memcpy(new_buf, sock->received_buf + read_len, sock->received_len - read_len);
        free(sock->received_buf);
        sock->received_len -= read_len;
        sock->received_buf = new_buf;
    }
    else
    {
        free(sock->received_buf);
        sock->received_buf = NULL;
        sock->received_len = 0;
    }
    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    return 0;
}

int tju_handle_packet(tju_tcp_t *sock, char *pkt)
{

    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;

    // 把收到的数据放到接受缓冲区
    while (pthread_mutex_lock(&(sock->recv_lock)) != 0)
        ; // 加锁

    if (sock->received_buf == NULL)
    {
        sock->received_buf = malloc(data_len);
    }
    else
    {
        sock->received_buf = realloc(sock->received_buf, sock->received_len + data_len);
    }
    memcpy(sock->received_buf + sock->received_len, pkt + DEFAULT_HEADER_LEN, data_len);
    sock->received_len += data_len;

    pthread_mutex_unlock(&(sock->recv_lock)); // 解锁

    //讨论要做特殊处理的包
    int pkt_seq = get_seq(pkt);
    int pkt_src = get_src(pkt);
    int pkt_ack = get_ack(pkt);
    int pkt_plen = get_plen(pkt);
    int pkt_flag = get_flags(pkt);
    if (sock->state == LISTEN) //处于侦听状态的sock只要要处理SYN包
    {
        // 收到SYN报文
        if (pkt_flag == SYN_FLAG_MASK)
        {
            // 新建一个socket（半连接）
            tju_tcp_t *new_sock = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
            memcpy(new_sock, sock, sizeof(tju_tcp_t));
            tju_sock_addr remote_addr, local_addr;
            remote_addr.ip = inet_network("172.17.0.2"); // Listen 是 server 端的行为，所以远程地址就是 172.17.0.5
            remote_addr.port = pkt_src;
            local_addr.ip = sock->bind_addr.ip;
            local_addr.port = sock->bind_addr.port;

            new_sock->established_local_addr = local_addr;
            new_sock->established_remote_addr = remote_addr;
            sock->state = SYN_RECV;
            new_sock->state = SYN_RECV;

            new_sock = syn_push(socks_queue, new_sock);//把半连接sock压入sock队列的半连接队列
            //压入时要记录压入的时间,用来超时重传

            // 发送SYN+ACK报文
            printf("%d\n", pkt_seq);
            synack = create_packet_buf(new_sock->established_local_addr.port, new_sock->established_remote_addr.port, SERVER_ISN, pkt_seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK | SYN_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(synack, 20);
            printf("发送SYNACK_FLAG_MASK\n");
        }
        else
        {
            printf("当前为 LISTEN,接收到其他 flag 的报文，丢弃之\n");
        }
    }
    else if (sock->state == SYN_SENT)//处于的SYN_SENT的sock只要要处理SYNACK包
    {
        if (pkt_flag == ACK_FLAG_MASK | SYN_FLAG_MASK)
        {
            tju_packet_t *new_pack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port, pkt_ack, pkt_seq + 1,
                                                   DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            ack = packet_to_buf(new_pack);
            sendToLayer3(ack, DEFAULT_HEADER_LEN);
            printf("Client 端发送了第三次握手，建立成功\n");
            sock->state = ESTABLISHED;
        }
        else
        {
            printf("Client 端接收到了不正确的ack\n");
        }
    }
    else if (sock->state == SYN_RECV)//处于的SYN_RECV的sock只要处理ACK包和SYN包
    {
        if (pkt_flag == ACK_FLAG_MASK)
        {
            sock->state = ESTABLISHED;
            tju_tcp_t *new_sock = syn_pop(socks_queue);
            new_sock = acc_push(socks_queue, new_sock); //把新sock压入到已连接队列
            new_sock->state = ESTABLISHED;
            printf("Server 端接收到了FIN报文\n");
        }
        else if(pkt_flag == SYN_FLAG_MASK)
        {
            
            sendToLayer3(synack, 20);
            printf("server 重传了 SYNACK\n");
        }
    }
    else if( sock->state == ESTABLISHED ) //处于建立态的客户端sock要处理SYNACK包
    {
        if((pkt_flag == ACK_FLAG_MASK | SYN_FLAG_MASK ) && get_plen(pkt) == DEFAULT_HEADER_LEN )
        {
            sendToLayer3(ack,20);
            printf("client 重传了 ACK\n");  
        }
        
    }

    return 0;
}

int tju_close(tju_tcp_t *sock)
{
    return 0;
}

tju_tcp_t *acc_pop(tju_sock_queue *q)
{
    if (q->accept_queue[0] != NULL)
    {
        tju_tcp_t *return_socket = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
        memcpy(return_socket, q->accept_queue[0], sizeof(tju_tcp_t));
        // Shift the remaining sockets in the accept queue
        for (int i = 1; i < QUEUE_LEN; i++)
        {
            q->accept_queue[i - 1] = q->accept_queue[i];
        }
        q->accept_queue[QUEUE_LEN - 1] = NULL;
        return return_socket;
    }
    return NULL;
}
tju_tcp_t *acc_push(tju_sock_queue *q, tju_tcp_t *new_socket)
{
    for (int i = 0; i < QUEUE_LEN; i++)
    {
        if (q->accept_queue[i] == NULL)
        {
            q->accept_queue[i] = new_socket;
            return new_socket;
        }
    }
    return NULL;
}

tju_tcp_t *syn_pop(tju_sock_queue *q)
{
    if (q->syns_queue[0] != NULL)
    {
        tju_tcp_t *return_socket = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
        memcpy(return_socket, q->syns_queue[0], sizeof(tju_tcp_t));
        // Shift the remaining sockets in the accept queue
        for (int i = 1; i < QUEUE_LEN; i++)
        {
            q->syns_queue[i - 1] = q->syns_queue[i];
        }
        q->syns_queue[QUEUE_LEN - 1] = NULL;
        return return_socket;
    }
    return NULL;
}

tju_tcp_t *syn_push(tju_sock_queue *q, tju_tcp_t *new_socket)
{
    for (int i = 0; i < QUEUE_LEN; i++)
    {
        if (q->syns_queue[i] == NULL) //找到空位
        {
            q->syns_queue[i] = new_socket;
            time(&q->syns_queue_wait[i]);   //记录压入时间
            return new_socket;
        }
    }
    return NULL;
}

