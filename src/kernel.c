#include "kernel.h"
/*
模拟Linux内核收到一份TCP报文的处理函数
*/
void onTCPPocket(char* pkt){
    // 当我们收到TCP包时 包中 源IP 源端口 是发送方的 也就是我们眼里的 远程(remote) IP和端口
    uint16_t remote_port = get_src(pkt);  //get的返回值已经是本机字节序了
    uint16_t local_port = get_dst(pkt);
    // remote ip 和 local ip 是读IP 数据包得到的 仿真的话这里直接根据hostname判断

    char hostname[8];
    gethostname(hostname, 8);
    uint32_t remote_ip, local_ip;
    if(strcmp(hostname,"server")==0){ // 自己是服务端 远端就是客户端
        local_ip = inet_network("172.17.0.3");
        remote_ip = inet_network("172.17.0.2");
    }else if(strcmp(hostname,"client")==0){ // 自己是客户端 远端就是服务端 
        local_ip = inet_network("172.17.0.2");
        remote_ip = inet_network("172.17.0.3");
    }
    
    int hashval;
    // 根据4个ip port 组成四元组 查找有没有已经建立连接的socket
    hashval = cal_hash(local_ip, local_port, remote_ip, remote_port);

    //服务端收到第一次握手包内核不用处理，直接交付给对应listensock即可

    //客户端收到第二次握手包就要建立连接
    if(strcmp(hostname,"client")==0){ // 自己是客户端 远端就是服务端 
        uint8_t pktflag = get_flags(pkt);
        if(pktflag == SYNACK_FLAG_MASK){
            /* printf("kernel recved SYNACK\n"); */
            established_socks[hashval] = connecting_sock;
        }
    }

     //服务端收到第三次握手包就要建立连接
    if(strcmp(hostname,"server")==0){ // 自己是客户端 远端就是服务端 
        uint8_t pktflag = get_flags(pkt);
        uint16_t plen = get_plen(pkt);

        if(pktflag == ACK_FLAG_MASK && plen == DEFAULT_HEADER_LEN){
            if(established_socks[hashval] == NULL ){
                established_socks[hashval] = connecting_sock;
                /* printf("kernel recved ACK\n"); */
                hashval = cal_hash(local_ip, local_port, 0, 0); 
                //交付给监听sock，否则会直接交付给newsock
                if (listen_socks[hashval]!=NULL){
                    /* 上面的判断是为了防止出现为建立连接，且listensock未在侦听
                    出现client直接发送ack包的行为 */
                    tju_handle_packet(listen_socks[hashval], pkt);
                    return;
                }
            }
        }
    }

    // 首先查找已经建立连接的socket哈希表
    if (established_socks[hashval]!=NULL){
        tju_handle_packet(established_socks[hashval], pkt);
        return;
    }

    // 没有的话再查找监听中的socket哈希表
    hashval = cal_hash(local_ip, local_port, 0, 0); //监听的socket只有本地监听ip和端口 没有远端
    if (listen_socks[hashval]!=NULL){
        tju_handle_packet(listen_socks[hashval], pkt);
        return;
    }

    // 都没找到 丢掉数据包
    printf("找不到能够处理该TCP数据包的socket, 丢弃该数据包\n");
    return;
}



/*
以用户填写的TCP报文为参数
根据用户填写的TCP的目的IP和目的端口,向该地址发送数据报
不可以修改此函数实现
*/
void sendToLayer3(char* packet_buf, int packet_len){
    if (packet_len>MAX_LEN){
        printf("ERROR: 不能发送超过 MAX_LEN 长度的packet, 防止IP层进行分片\n");
        return;
    }

    // 获取hostname 根据hostname 判断是客户端还是服务端
    char hostname[8];
    gethostname(hostname, 8);

    struct sockaddr_in conn;
    conn.sin_family      = AF_INET;            
    conn.sin_port        = htons(20218);
    int rst;
    if(strcmp(hostname,"server")==0){
        conn.sin_addr.s_addr = inet_addr("172.17.0.2");
        rst = sendto(BACKEND_UDPSOCKET_ID, packet_buf, packet_len, 0, (struct sockaddr*)&conn, sizeof(conn));
    }else if(strcmp(hostname,"client")==0){       
        conn.sin_addr.s_addr = inet_addr("172.17.0.3");
        rst = sendto(BACKEND_UDPSOCKET_ID, packet_buf, packet_len, 0, (struct sockaddr*)&conn, sizeof(conn));
    }else{
        printf("请不要改动hostname...\n");
        exit(-1);
    }
}

/*
 仿真接受数据线程
 不断调用server或cliet监听在20218端口的UDPsocket的recvfrom
 一旦收到了大于TCPheader长度的数据 
 则接受整个TCP包并调用onTCPPocket()
*/
void* receive_thread(void* arg){

    char hdr[DEFAULT_HEADER_LEN];
    char* pkt;

    uint32_t plen = 0, buf_size = 0, n = 0;
    int len;

    struct sockaddr_in from_addr;
    int from_addr_size = sizeof(from_addr);

    while(1) {
        // MSG_PEEK 表示看一眼 不会把数据从缓冲区删除
        len = recvfrom(BACKEND_UDPSOCKET_ID, hdr, DEFAULT_HEADER_LEN, MSG_PEEK, (struct sockaddr *)&from_addr, &from_addr_size);
        // 一旦收到了大于header长度的数据 则接受整个TCP包
        if(len >= DEFAULT_HEADER_LEN){
            plen = get_plen(hdr); 
            pkt = malloc(plen);
            buf_size = 0;
            while(buf_size < plen){ // 直到接收到 plen 长度的数据 接受的数据全部存在pkt中
                n = recvfrom(BACKEND_UDPSOCKET_ID, pkt + buf_size, plen - buf_size, NO_FLAG, (struct sockaddr *)&from_addr, &from_addr_size);
                buf_size = buf_size + n;
            }
            // 通知内核收到一个完整的TCP报文
            onTCPPocket(pkt);
            free(pkt);
        }
    }
}

/*
 开启仿真, 运行起后台线程

 不论是server还是client
 都创建一个UDP socket 监听在20218端口
 然后创建新线程 不断调用该socket的recvfrom
*/
void startSimulation(){
    // 对于内核 初始化监听socket哈希表和建立连接socket哈希表
    int index;
    for(index=0;index<MAX_SOCK;index++){
        listen_socks[index] = NULL;
        established_socks[index] = NULL;
    }

    // 获取hostname 
    char hostname[8];
    gethostname(hostname, 8);
    // printf("startSimulation on hostname: %s\n", hostname);

    BACKEND_UDPSOCKET_ID = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (BACKEND_UDPSOCKET_ID < 0){
        printf("ERROR opening socket");
        exit(-1);
    }

    // 设置socket选项 SO_REUSEADDR = 1 
    // 意思是 允许绑定本地地址冲突 和 改变了系统对处于TIME_WAIT状态的socket的看待方式 
    int optval = 1;
    setsockopt(BACKEND_UDPSOCKET_ID, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval , sizeof(int));

    struct sockaddr_in conn;
    memset(&conn, 0, sizeof(conn)); 
    conn.sin_family = AF_INET;
    conn.sin_addr.s_addr = htonl(INADDR_ANY); // INADDR_ANY = 0.0.0.0
    conn.sin_port = htons((unsigned short)20218);

    if (bind(BACKEND_UDPSOCKET_ID, (struct sockaddr *) &conn, sizeof(conn)) < 0){
        printf("ERROR on binding");
        exit(-1);
    }

    pthread_t thread_id = 1001;
    int rst = pthread_create(&thread_id, NULL, receive_thread, (void*)(&BACKEND_UDPSOCKET_ID));
    if (rst<0){
        printf("ERROR open thread");
        exit(-1); 
    }
    // printf("successfully created bankend thread\n");
    return;
}

int cal_hash(uint32_t local_ip, uint16_t local_port, uint32_t remote_ip, uint16_t remote_port){
    // 实际上肯定不是这么算的
    return ((int)local_ip+(int)local_port+(int)remote_ip+(int)remote_port)%MAX_SOCK;
}