#include "tju_tcp.h"

#define DEFAULT_WAIT_TIME 1000  //单位ms
#define CLOSE_TIME_WAIT 2000  //单位ms
/*
创建 TCP socket
初始化对应的结构体
设置初始状态为 CLOSED
*/
char * syn = NULL;
char * synack = NULL;
char * ack = NULL;

char * close_ack = NULL;
char * fin = NULL;

pthread_mutex_t mutex;

char fgnm[10][20] = {"SYN...","ACK...","SYNACK","FIN...","ZERO..","FINACK","badnum"};
int pfindex (uint8_t flag){
    int ans = 0;
    switch (flag)
    {
    case 8:
        ans = 0;
        break;
    case 4:
        ans = 1;
        break;
    case 12:
        ans = 2;
        break;
    case 2:
        ans = 3;
        break;
    case 0:
        ans = 4;
        break;
    case 6:
        ans = 5;
        break;
    default:
        ans = 6;
        break;
    }

    return ans;
}

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
                int now_time;
                int time_diff;  
                now_time = clock() ; //获取当下时间
                time_diff = (double) (now_time - socks_queue->syns_queue_wait[i]) / CLOCKS_PER_SEC * 1000; //计算时间差
                if(time_diff > DEFAULT_WAIT_TIME){    //等到超过DEFAULT_WAIT_TIME ms，重传SYNACK
                    sendToLayer3(synack,20);
                    printf("wait: %d ms\n",time_diff);
                    printf("ACK lost,重传SYNACK\n");
                    socks_queue->syns_queue_wait[i] = clock(); //重新计时
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
    remote_addr.ip = inet_network(CLIENT_ADDR); // 具体的IP地址
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
    local_addr.ip = inet_network(CLIENT_ADDR);
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
    printf("Client 端发送了SYN段\n");
    sendToLayer3(syn, 20);
    sock->state = SYN_SENT;

    int start_time, end_time;
    int time_diff = 0;
    start_time = clock(); //开始计时
    while (sock->state != ESTABLISHED)
    {
        // 阻塞
        end_time = clock(); //结束计时
        
        time_diff = (double)(end_time - start_time) / CLOCKS_PER_SEC * 1000; //计算时间差
        
        if( time_diff > DEFAULT_WAIT_TIME ){ //超时重传，等到超过DEFAULT_WAIT_TIME ms
            printf("wait: %d ms\n",time_diff);
            printf("Client 重传了 SYN!\n");
            sendToLayer3(syn,DEFAULT_HEADER_LEN);
            start_time = clock(); //重新计时
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

/*******************************************************************************************/
    //解析包头
    uint32_t data_len = get_plen(pkt) - DEFAULT_HEADER_LEN;
    int pkt_seq = get_seq(pkt);
    int pkt_src = get_src(pkt);
    int pkt_ack = get_ack(pkt);
    int pkt_plen = get_plen(pkt);
    int pkt_flag = get_flags(pkt);


    printf("..............sock收到一个段,段的标志位为：%s..................\n",fgnm[pfindex(pkt_flag)]);

    // 收到的数据包可能有data，data都要被放入sock缓存区
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

/*******************************************************************************************/

    //根据sock的状态讨论要做特殊处理的包
    //使用switch语句时结构更加清晰，同时还能保证程序运行效率
    pthread_mutex_lock(&mutex);
    switch (sock->state )
    {
    case LISTEN: //处于侦听状态的sock只要要处理SYN包
    {
        // 收到SYN报文
        if (pkt_flag == SYN_FLAG_MASK)
        {
            printf("Server 侦听到一个SYN段\n");
            // 新建一个socket（半连接）
            tju_tcp_t *new_sock = (tju_tcp_t *)malloc(sizeof(tju_tcp_t));
            memcpy(new_sock, sock, sizeof(tju_tcp_t));
            tju_sock_addr remote_addr, local_addr;
            remote_addr.ip = inet_network(CLIENT_ADDR); 
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
            synack = create_packet_buf(new_sock->established_local_addr.port, new_sock->established_remote_addr.port,
                 SERVER_ISN, pkt_seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK | SYN_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(synack, 20);
            printf("Server 发送SYNACK段\n");
        }
        else
        {
            printf("Server当前为 LISTEN,接收到除SYN以外的报文,丢弃之\n");
        }
        
    }break;
    case SYN_SENT: //处于的SYN_SENT的sock只要处理SYNACK包
    {
        if (pkt_flag == (ACK_FLAG_MASK | SYN_FLAG_MASK))
        {
            printf("Client 收到SYNACK段\n");
            tju_packet_t *new_pack = create_packet(sock->established_local_addr.port, sock->established_remote_addr.port, pkt_ack, pkt_seq + 1,
                                                   DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            ack = packet_to_buf(new_pack);
            sendToLayer3(ack, DEFAULT_HEADER_LEN);
            printf("Client 发送了 ACK, Client 进入 ESTABLISHED \n");
            sock->state = ESTABLISHED;
        }
        else
        {
            printf("Client 当前为 SYN_SENT,接收到除SYNACK以外的报文,丢弃之\n");
        }
        
    }break;
    case SYN_RECV ://处于的SYN_RECV的sock只要处理ACK包和SYN包
    {
        if (pkt_flag == ACK_FLAG_MASK && get_plen(pkt) == DEFAULT_HEADER_LEN )   //确保是一个建立连接时的ack包
        {
            printf("Server 收到 ACK 段,Server 进入 ESTABLISHED\n");
            sock->state = ESTABLISHED;
            tju_tcp_t *new_sock = syn_pop(socks_queue);
            new_sock = acc_push(socks_queue, new_sock); //把新sock压入到已连接队列
            new_sock->state = ESTABLISHED;
        }
        else if(pkt_flag == SYN_FLAG_MASK)
        {
            printf("Server 收到了 duplicate SYN 段");
            sendToLayer3(synack, 20);
            printf("server 重传了 SYNACK\n");
        }
        
    }break;
    case ESTABLISHED ://处于建立态的客户端sock要处理SYNACK包,FIN段
    {
        if(pkt_flag == (ACK_FLAG_MASK | SYN_FLAG_MASK) ) //关系运算的优先级高于位运算
        {
            printf("Client 当前处于 ESTABLISHED, 收到 dulplicate SYNACK\n");
            sendToLayer3(ack,20);
            printf("client 重传了 ACK\n");  
        }
        else if (pkt_flag == FIN_FLAG_MASK || pkt_flag == (FIN_FLAG_MASK | ACK_FLAG_MASK) ) {//收到FIN段,发送ACK,进入CLOSE_WAIT
            printf("对方先发起关闭连接\n");
            close_ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                 pkt_ack, pkt_seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            printf("Sock 发送了关闭连接的ACK报文\n");
            sock -> state = CLOSE_WAIT;
            printf("Sock 切换至 CLOSE_WAIT \n");
        }
        else {
            //空语句
        }
        
    }break;
    case FIN_WAIT_1 :  //如果是FIN_WAIT_1，则处理ACK，或者FIN
    {
        printf("sock 处于FIN_WAIT_1\n");
        if(pkt_flag == FIN_FLAG_MASK || pkt_flag == (FIN_FLAG_MASK | ACK_FLAG_MASK)){  //如果收到FIN或者FINACK，说明同时断连
            //发送ACK，同时切换状态为closing
            printf("收到FIN,对方与本地sock同时发出断开连接的请求!\n");
            close_ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                 pkt_ack, pkt_seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            printf("Sock 发送了关闭连接的ACK报文\n");
            sock -> state = CLOSING;
            printf("Sock 状态切换至CLOSING\n");
        }
        else if( pkt_flag == ACK_FLAG_MASK){  //收到关闭连接的ACK包，切换状态为FIN_WAIT_2
            printf("Sock 收到关闭连接的ACK包\n");
            sock -> state = FIN_WAIT_2;
            printf("Sock 状态切换至FIN_WAIT_2\n");
        }
        else{
            //不做处理
        }
        
    }break;
    case CLOSING : //如果是closing,此时本地sock很清楚一点，那就是同时关闭连接
    {
        printf("sock当前为CLOSING\n");
        //唯一要做的就是得到对方的ack，确保对方也知道这是一个同时断连
        if(pkt_flag == ACK_FLAG_MASK){   //显然，对方收到了自己的FIN,双方都清楚对方也要断开连接
            printf("sock收到ACK,双方明确现在是同时断连\n");
            printf("补发一个ACK段\n");
            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            sock -> state = TIME_WAIT;
            printf("sock切换状态为TIME_WAIT\n");
        }
        else if( pkt_flag == FIN_FLAG_MASK || pkt_flag == (FIN_FLAG_MASK | ACK_FLAG_MASK)){ //自己明明收到对方的FIN，但是又再closing态下收到FIN，只有一种可能，就是对方超时重传了FIN
            //自己发过两个段，一个是FIN，一个是ACK
            //如果只有FIN段丢失，则对方还在FIN_WAIT_2，等待自己的FIN
            //如果只有ACK段丢失，则对方也在closing,等待自己的ACK
            //如果FIN，ACK段都丢失，那么对方还在FIN_WAIT_1，等待自己的ACK，或者FIN

            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            printf("自己发的FIN, 或者ACK发生丢包, Sock 重传了关闭连接的ACK报文,确保对方离开FIN_WAIT_1和closing\n");

        }
        else{
            //空语句
        }
        
    }break;
    case FIN_WAIT_2 :   //收到ACK不用管，收到FIN回ACK
    {
        printf("sock当前为FIN_WAIT_2\n");
        if(pkt_flag == FIN_FLAG_MASK || pkt_flag == (FIN_FLAG_MASK | ACK_FLAG_MASK)){
            close_ack = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port,
                 pkt_ack, pkt_seq + 1, DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, ACK_FLAG_MASK, 1, 0, NULL, 0);
            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            printf("sock发送ACK段\n");
            sock -> state = TIME_WAIT;
            printf("sock切换状态为TIME_WAIT\n");
        }
        else{
            //空语句
        }
        
    }break;
    case CLOSE_WAIT :   //如果是CLOSE_WAIT，则对方先手发起FIN被建立态的自己收到，进入CLOSE_WAIT
    {
        printf("sock当前为CLOSE_WAIT\n");
        //这里可能还会收到FIN，因为用户可能一直没有调用close,或者自己的ACK丢失，对方超时就会重传FIN
        if(pkt_flag == FIN_FLAG_MASK){  
            printf("对方超时重传了FIN段\n");
            sendToLayer3(close_ack, DEFAULT_HEADER_LEN);
            printf("Sock 重传了关闭连接的ACK报文\n");
        }
    }
    case LAST_ACK :         //等待ACK后或者超时自动断连
    {
        if(pkt_flag == ACK_FLAG_MASK){
            printf("sock收到ACK\n");
            sock -> state = CLOSED;
        }
        
    }break;
    default:{
        //空语句
        
    }break;
    }
    pthread_mutex_unlock(&mutex);
    return 0;
}



int tju_close(tju_tcp_t *sock)
{
    int hashval = cal_hash(sock->established_local_addr.ip, sock->established_local_addr.port, 
            sock->established_remote_addr.ip, sock->established_remote_addr.port);
    // 调用close时，可能已经收到对方的FIN段，也可能是主动发起的close
    printf("正在关闭sock......\n");
    if( !( sock -> state == ESTABLISHED || sock -> state == CLOSE_WAIT) ){  //错误的时间调用close
        printf("Sock could not be closed now! \n");
        return -1;
    }
    //为了防止出现主进程和后台线程的竞争条件,定义一把锁pthread_mutex_t mutex;
    pthread_mutex_lock(&mutex);
    if(sock -> state == ESTABLISHED){ //如果发起close时是ESTABLISHE，就认为尚未收到FIN
        //切换状态，发出FIN段，等待状态切换至TIME_WAIT,同时启动计时器
        printf("sock 当前是 ESTABLISHE \n");
        fin = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 0, 0,
            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, (FIN_FLAG_MASK | ACK_FLAG_MASK), 1, 0, NULL, 0);
        sendToLayer3(fin,DEFAULT_HEADER_LEN);
        printf("sock 发送FIN段\n");
        sock -> state = FIN_WAIT_1;
        printf("sock 状态切换至 FIN_WAIT_1\n");
        pthread_mutex_unlock(&mutex);            //临界区的终点处解锁
        int start = clock();  //启动计时器;
        int time_dff = 0;
        int end = 0;
        while(sock -> state != TIME_WAIT ){
            end = clock();  //获取当前时间
            time_dff = (double)(end - start) / CLOCKS_PER_SEC * 1000 ;  //计算等待时间,单位ms
            if( time_dff > CLOSE_TIME_WAIT){
                printf("wait : %d ms!\n" , time_dff);
                sendToLayer3(fin,DEFAULT_HEADER_LEN);
                printf("wait too long,sock 重传FIN\n");
                start = clock(); //重新计时
            }
        }
        // 进入TIME_WAIT后计时2ms自动关闭
        printf("The connection will be closed in SET_TIME\n");
        int start0 = clock();  //启动计时器;
        int time_dff0 = 0;
        int end0 = 0;
        while(sock -> state != CLOSED){
            end0 = clock();  //获取当前时间
            time_dff0 = (double)(end0 - start0) / CLOCKS_PER_SEC * 1000 ; //计算等待时间,单位ms
            if( time_dff0 > CLOSE_TIME_WAIT){
                printf("wait : %d ms, time up!\n" , time_dff0);
                sock -> state = CLOSED;
                established_socks[hashval] = NULL;
                printf("sock closed successfully!\n");
            }
        }
    }
    else{  
        pthread_mutex_unlock(&mutex);
        //如果close时sock状态是CLOSE_WAIT,说明是在收到对方FIN报文后用户才调用的close
        printf("对方已经请求关闭连接, 本地调用close,即将终止连接\n");
        //发送FINACK段，计时等待进入closed状态
        fin = create_packet_buf(sock->established_local_addr.port, sock->established_remote_addr.port, 0, 0,
            DEFAULT_HEADER_LEN, DEFAULT_HEADER_LEN, (FIN_FLAG_MASK | ACK_FLAG_MASK), 1, 0, NULL, 0);
        sendToLayer3(fin,DEFAULT_HEADER_LEN);
        printf("sock 发送FINACK段\n");
        sock->state = LAST_ACK;
        printf("sock 切换至LAST_ACK\n");
        printf("sock 等待ACK或者超时后自动断连\n");
        int start = clock();  //启动计时器;
        int time_dff = 0;
        int end = 0;
        int automatial = 0;
        while(sock -> state != CLOSED ){
            end = clock();  //获取当前时间
            time_dff = (double)(end - start) / CLOCKS_PER_SEC * 1000 ;  //计算等待时间,单位ms
            pthread_mutex_lock(&mutex);    //上锁
            if( time_dff > DEFAULT_WAIT_TIME){
                printf("wait : %d ms!\n" , time_dff);
                sendToLayer3(fin,DEFAULT_HEADER_LEN);
                printf("sock 重发FINACK段\n");
                sock -> state = CLOSED;
                automatial = 1;
                printf("wait too long,sock closed automatically!\n");
            }
            pthread_mutex_unlock(&mutex) ;
        }
        
        established_socks[hashval] = NULL;
        if(automatial == 0){
            printf("收到ACK,sock正常关闭\n");
        }

    }



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
            q->syns_queue_wait[i] = clock() ; //记录压入时间
            return new_socket;
        }
    }
    return NULL;
}

