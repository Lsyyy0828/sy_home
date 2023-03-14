typedef __gnu_cxx::hash_map<int, io_buf*> pool_t;

enum MEM_CAP {
    m4K     = 4096,
    m16K    = 16384,
    m64K    = 65536,
    m256K   = 262144,
    m1M     = 1048576,
    m4M     = 4194304,
    m8M     = 8388608
};


//总内存池最大限制 单位是Kb 所以目前限制是 5GB
#define EXTRA_MEM_LIMIT (5U *1024 *1024) 

/*
 *  定义buf内存池
 *  设计为单例
 * */
class buf_pool 
{
public:
    //初始化单例对象
    static void init() {
        //创建单例
        _instance = new buf_pool();
    }

    //获取单例方法
    static buf_pool *instance() {
        //保证init方法在这个进程执行中 只被执行一次
        pthread_once(&_once, init);
        return _instance;
    }

    //开辟一个io_buf
    io_buf *alloc_buf(int N);
    io_buf *alloc_buf() { return alloc_buf(m4K); }


    //重置一个io_buf
    void revert(io_buf *buffer);

    
private:
    buf_pool();

    //拷贝构造私有化
    buf_pool(const buf_pool&);
    const buf_pool& operator=(const buf_pool&);

    //所有buffer的一个map集合句柄
    pool_t _pool;

    //总buffer池的内存大小 单位为KB
    uint64_t _total_mem;

    //单例对象
    static buf_pool *_instance;

    //用于保证创建单例的init方法只执行一次的锁
    static pthread_once_t _once;

    //用户保护内存池链表修改的互斥锁
    static pthread_mutex_t _mutex;
};

class event_loop;

//IO事件触发的回调函数
typedef void io_callback(event_loop *loop, int fd, void *args);

/*
 * 封装一次IO触发实现 
 * */
struct io_event 
{
    io_event():read_callback(NULL),write_callback(NULL),rcb_args(NULL),wcb_args(NULL) {}

    int mask; //EPOLLIN EPOLLOUT
    io_callback *read_callback; //EPOLLIN事件 触发的回调 
    io_callback *write_callback;//EPOLLOUT事件 触发的回调
    void *rcb_args; //read_callback的回调函数参数
    void *wcb_args; //write_callback的回调函数参数
};
#define MAXEVENTS 10

// map: fd->io_event 
typedef __gnu_cxx::hash_map<int, io_event> io_event_map;
//定义指向上面map类型的迭代器
typedef __gnu_cxx::hash_map<int, io_event>::iterator io_event_map_it;
//全部正在监听的fd集合
typedef __gnu_cxx::hash_set<int> listen_fd_set;

class event_loop 
{
public:
    //构造，初始化epoll堆
    event_loop();

    //阻塞循环处理事件
    void event_process();

    //添加一个io事件到loop中
    void add_io_event(int fd, io_callback *proc, int mask, void *args=NULL);

    //删除一个io事件从loop中
    void del_io_event(int fd);

    //删除一个io事件的EPOLLIN/EPOLLOUT
    void del_io_event(int fd, int mask);
    
private:
    int _epfd; //epoll fd

    //当前event_loop 监控的fd和对应事件的关系
    io_event_map _io_evs;

    //当前event_loop 一共哪些fd在监听
    listen_fd_set listen_fds;

    //一次性最大处理的事件
    struct epoll_event _fired_evs[MAXEVENTS];


};
class io_buf {
public:
    //构造，创建一个io_buf对象
    io_buf(int size);

    //清空数据
    void clear();

    //将已经处理过的数据，清空,将未处理的数据提前至数据首地址
    void adjust();

    //将其他io_buf对象数据考本到自己中
    void copy(const io_buf *other);

    //处理长度为len的数据，移动head和修正length
    void pop(int len);

    //如果存在多个buffer，是采用链表的形式链接起来
    io_buf *next;

    //当前buffer的缓存容量大小
    int capacity;
    //当前buffer有效数据长度
    int length;
    //未处理数据的头部位置索引
    int head;
    //当前io_buf所保存的数据地址
    char *data;
};

struct msg_head
{
    int msgid;
    int msglen;
};

//消息头的二进制长度，固定数
#define MESSAGE_HEAD_LEN 8

//消息头+消息体的最大长度限制
#define MESSAGE_LENGTH_LIMIT (65535 - MESSAGE_HEAD_LEN)

//msg 业务回调函数原型

class tcp_client;
typedef void msg_callback(const char *data, uint32_t len, int msgid, tcp_client *client, void *user_data);
/*
 * 给业务层提供的最后tcp_buffer结构
 * */
class reactor_buf {
public:
    reactor_buf();
    ~reactor_buf();

    const int length() const;
    void pop(int len);
    void clear();

protected:
    io_buf *_buf;
};

//读(输入) 缓存buffer
class input_buf : public reactor_buf 
{
public:
    //从一个fd中读取数据到reactor_buf中
    int read_data(int fd);

    //取出读到的数据
    const char *data() const;

    //重置缓冲区
    void adjust();
};

//写(输出)  缓存buffer
class output_buf : public reactor_buf 
{
public:
    //将一段数据 写到一个reactor_buf中
    int send_data(const char *data, int datalen);

    //将reactor_buf中的数据写到一个fd中
    int write2fd(int fd);
};
//一个tcp的连接信息
class tcp_conn
{
public:
    //初始化tcp_conn
    tcp_conn(int connfd, event_loop *loop);

    //处理读业务
    void do_read();

    //处理写业务
    void do_write();

    //销毁tcp_conn
    void clean_conn();

    //发送消息的方法
    int send_message(const char *data, int msglen, int msgid);

private:
    //当前链接的fd
    int _connfd;
    //该连接归属的event_poll
    event_loop *_loop;
    //输出buf
    output_buf obuf;     
    //输入buf
    input_buf ibuf;
};
class tcp_server
{ 
public: 
    //server的构造函数
    tcp_server(event_loop* loop, const char *ip, uint16_t port); 

    //开始提供创建链接服务
    void do_accept();

    //链接对象释放的析构
    ~tcp_server();

private: 
    int _sockfd; //套接字
    struct sockaddr_in _connaddr; //客户端链接地址
    socklen_t _addrlen; //客户端链接地址长度

    //event_loop epoll事件机制
    event_loop* _loop;
}; 
//单例对象
buf_pool * buf_pool::_instance = NULL;

//用于保证创建单例的init方法只执行一次的锁
pthread_once_t buf_pool::_once = PTHREAD_ONCE_INIT;


//用户保护内存池链表修改的互斥锁
pthread_mutex_t buf_pool::_mutex = PTHREAD_MUTEX_INITIALIZER;


//构造函数 主要是预先开辟一定量的空间
//这里buf_pool是一个hash，每个key都是不同空间容量
//对应的value是一个io_buf集合的链表
//buf_pool -->  [m4K] -- io_buf-io_buf-io_buf-io_buf...
//              [m16K] -- io_buf-io_buf-io_buf-io_buf...
//              [m64K] -- io_buf-io_buf-io_buf-io_buf...
//              [m256K] -- io_buf-io_buf-io_buf-io_buf...
//              [m1M] -- io_buf-io_buf-io_buf-io_buf...
//              [m4M] -- io_buf-io_buf-io_buf-io_buf...
//              [m8M] -- io_buf-io_buf-io_buf-io_buf...
buf_pool::buf_pool():_total_mem(0)
{
    io_buf *prev; 
    
    //----> 开辟4K buf 内存池
    _pool[m4K] = new io_buf(m4K);
    if (_pool[m4K] == NULL) {
        fprintf(stderr, "new io_buf m4K error");
        exit(1);
    }

    prev = _pool[m4K];
    //4K的io_buf 预先开辟5000个，约20MB供开发者使用
    for (int i = 1; i < 5000; i ++) {
        prev->next = new io_buf(m4K);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m4K error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 4 * 5000;



    //----> 开辟16K buf 内存池
    _pool[m16K] = new io_buf(m16K);
    if (_pool[m16K] == NULL) {
        fprintf(stderr, "new io_buf m16K error");
        exit(1);
    }

    prev = _pool[m16K];
    //16K的io_buf 预先开辟1000个，约16MB供开发者使用
    for (int i = 1; i < 1000; i ++) {
        prev->next = new io_buf(m16K);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m16K error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 16 * 1000;



    //----> 开辟64K buf 内存池
    _pool[m64K] = new io_buf(m64K);
    if (_pool[m64K] == NULL) {
        fprintf(stderr, "new io_buf m64K error");
        exit(1);
    }

    prev = _pool[m64K];
    //64K的io_buf 预先开辟500个，约32MB供开发者使用
    for (int i = 1; i < 500; i ++) {
        prev->next = new io_buf(m64K);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m64K error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 64 * 500;


    //----> 开辟256K buf 内存池
    _pool[m256K] = new io_buf(m256K);
    if (_pool[m256K] == NULL) {
        fprintf(stderr, "new io_buf m256K error");
        exit(1);
    }

    prev = _pool[m256K];
    //256K的io_buf 预先开辟200个，约50MB供开发者使用
    for (int i = 1; i < 200; i ++) {
        prev->next = new io_buf(m256K);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m256K error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 256 * 200;


    //----> 开辟1M buf 内存池
    _pool[m1M] = new io_buf(m1M);
    if (_pool[m1M] == NULL) {
        fprintf(stderr, "new io_buf m1M error");
        exit(1);
    }

    prev = _pool[m1M];
    //1M的io_buf 预先开辟50个，约50MB供开发者使用
    for (int i = 1; i < 50; i ++) {
        prev->next = new io_buf(m1M);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m1M error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 1024 * 50;


    //----> 开辟4M buf 内存池
    _pool[m4M] = new io_buf(m4M);
    if (_pool[m4M] == NULL) {
        fprintf(stderr, "new io_buf m4M error");
        exit(1);
    }

    prev = _pool[m4M];
    //4M的io_buf 预先开辟20个，约80MB供开发者使用
    for (int i = 1; i < 20; i ++) {
        prev->next = new io_buf(m4M);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m4M error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 4096 * 20;



    //----> 开辟8M buf 内存池
    _pool[m8M] = new io_buf(m8M);
    if (_pool[m8M] == NULL) {
        fprintf(stderr, "new io_buf m8M error");
        exit(1);
    }

    prev = _pool[m8M];
    //8M的io_buf 预先开辟10个，约80MB供开发者使用
    for (int i = 1; i < 10; i ++) {
        prev->next = new io_buf(m8M);
        if (prev->next == NULL) {
            fprintf(stderr, "new io_buf m8M error");
            exit(1);
        }
        prev = prev->next;
    }
    _total_mem += 8192 * 10;
}


//开辟一个io_buf
//1 如果上层需要N个字节的大小的空间，找到与N最接近的buf hash组，取出，
//2 如果该组已经没有节点使用，可以额外申请
//3 总申请长度不能够超过最大的限制大小 EXTRA_MEM_LIMIT
//4 如果有该节点需要的内存块，直接取出，并且将该内存块从pool摘除
io_buf *buf_pool::alloc_buf(int N) 
{
    //1 找到N最接近哪hash 组
    int index;
    if (N <= m4K) {
        index = m4K;
    }
    else if (N <= m16K) {
        index = m16K;
    }
    else if (N <= m64K) {
        index = m64K;
    }
    else if (N <= m256K) {
        index = m256K;
    }
    else if (N <= m1M) {
        index = m1M;
    }
    else if (N <= m4M) {
        index = m4M;
    }
    else if (N <= m8M) {
        index = m8M;
    }
    else {
        return NULL;
    }


    //2 如果该组已经没有，需要额外申请，那么需要加锁保护
    pthread_mutex_lock(&_mutex);
    if (_pool[index] == NULL) {
        if (_total_mem + index/1024 >= EXTRA_MEM_LIMIT) {
            //当前的开辟的空间已经超过最大限制
            fprintf(stderr, "already use too many memory!\n");
            exit(1);
        }

        io_buf *new_buf = new io_buf(index);
        if (new_buf == NULL) {
            fprintf(stderr, "new io_buf error\n");
            exit(1);
        }
        _total_mem += index/1024;
        pthread_mutex_unlock(&_mutex);
        return new_buf;
    }

    //3 从pool中摘除该内存块
    io_buf *target = _pool[index];
    _pool[index] = target->next;
    pthread_mutex_unlock(&_mutex);
    
    target->next = NULL;
    
    return target;
}


//重置一个io_buf,将一个buf 上层不再使用，或者使用完成之后，需要将该buf放回pool中
void buf_pool::revert(io_buf *buffer)
{
    //每个buf的容量都是固定的 在hash的key中取值
    int index = buffer->capacity;
    //重置io_buf中的内置位置指针
    buffer->length = 0;
    buffer->head = 0;

    pthread_mutex_lock(&_mutex);
    //找到对应的hash组 buf首届点地址
    assert(_pool.find(index) != _pool.end());

    //将buffer插回链表头部
    buffer->next = _pool[index];
    _pool[index] = buffer;

    pthread_mutex_unlock(&_mutex);
}


//构造，初始化epoll堆
event_loop::event_loop() 
{
    //flag=0 等价于epll_craete
    _epfd = epoll_create1(0);
    if (_epfd == -1) {
        fprintf(stderr, "epoll_create error\n");
        exit(1);
    }
}


//阻塞循环处理事件
void event_loop::event_process()
{
    while (true) {
        io_event_map_it ev_it;

        int nfds = epoll_wait(_epfd, _fired_evs, MAXEVENTS, 10);
        for (int i = 0; i < nfds; i++) {
            //通过触发的fd找到对应的绑定事件
            ev_it = _io_evs.find(_fired_evs[i].data.fd);
            assert(ev_it != _io_evs.end());

            io_event *ev = &(ev_it->second);

            if (_fired_evs[i].events & EPOLLIN) {
                //读事件，掉读回调函数
                void *args = ev->rcb_args;
                ev->read_callback(this, _fired_evs[i].data.fd, args);
            }
            else if (_fired_evs[i].events & EPOLLOUT) {
                //写事件，掉写回调函数
                void *args = ev->wcb_args; 
                ev->write_callback(this, _fired_evs[i].data.fd, args);
            }
            else if (_fired_evs[i].events &(EPOLLHUP|EPOLLERR)) {
                //水平触发未处理，可能会出现HUP事件，正常处理读写，没有则清空
                if (ev->read_callback != NULL) {
                    void *args = ev->rcb_args;
                    ev->read_callback(this, _fired_evs[i].data.fd, args);
                }
                else if (ev->write_callback != NULL) {
                    void *args = ev->wcb_args;
                    ev->write_callback(this, _fired_evs[i].data.fd, args);
                }
                else {
                    //删除
                    fprintf(stderr, "fd %d get error, delete it from epoll\n", _fired_evs[i].data.fd);
                    this->del_io_event(_fired_evs[i].data.fd);
                }
            }

        }
    }
}

/*
 * 这里我们处理的事件机制是
 * 如果EPOLLIN 在mask中， EPOLLOUT就不允许在mask中
 * 如果EPOLLOUT 在mask中， EPOLLIN就不允许在mask中
 * 如果想注册EPOLLIN|EPOLLOUT的事件， 那么就调用add_io_event() 方法两次来注册。
 * */

//添加一个io事件到loop中
void event_loop::add_io_event(int fd, io_callback *proc, int mask, void *args)
{
    int final_mask;
    int op;

    //1 找到当前fd是否已经有事件
    io_event_map_it it = _io_evs.find(fd);
    if (it == _io_evs.end()) {
        //2 如果没有操作动作就是ADD
        //没有找到
        final_mask = mask;    
        op = EPOLL_CTL_ADD;
    }
    else {
        //3 如果有操作董酒是MOD
        //添加事件标识位
        final_mask = it->second.mask | mask;
        op = EPOLL_CTL_MOD;
    }

    //4 注册回调函数
    if (mask & EPOLLIN) {
        //读事件回调函数注册
        _io_evs[fd].read_callback = proc;
        _io_evs[fd].rcb_args = args;
    }
    else if (mask & EPOLLOUT) {
        _io_evs[fd].write_callback = proc;
        _io_evs[fd].wcb_args = args;
    }
    
    //5 epoll_ctl添加到epoll堆里
    _io_evs[fd].mask = final_mask;
    //创建原生epoll事件
    struct epoll_event event;
    event.events = final_mask;
    event.data.fd = fd;
    if (epoll_ctl(_epfd, op, fd, &event) == -1) {
        fprintf(stderr, "epoll ctl %d error\n", fd);
        return;
    }

    //6 将fd添加到监听集合中
    listen_fds.insert(fd);
}

//删除一个io事件从loop中
void event_loop::del_io_event(int fd)
{
    //将事件从_io_evs删除
    _io_evs.erase(fd);

    //将fd从监听集合中删除
    listen_fds.erase(fd);

    //将fd从epoll堆删除
    epoll_ctl(_epfd, EPOLL_CTL_DEL, fd, NULL);
}

//删除一个io事件的EPOLLIN/EPOLLOUT
void event_loop::del_io_event(int fd, int mask)
{
    //如果没有该事件，直接返回
    io_event_map_it it = _io_evs.find(fd);
    if (it == _io_evs.end()) {
        return ;
    }

    int &o_mask = it->second.mask;
    //修正mask
    o_mask = o_mask & (~mask);

    if (o_mask == 0) {
        //如果修正之后 mask为0，则删除
        this->del_io_event(fd);
    }
    else {
        //如果修正之后，mask非0，则修改
        struct epoll_event event;
        event.events = o_mask;
        event.data.fd = fd;
        epoll_ctl(_epfd, EPOLL_CTL_MOD, fd, &event);
    }
    
}



//构造，创建一个io_buf对象
io_buf::io_buf(int size)
{
    capacity = size; 
    length = 0;
    head = 0;
    next = 0;

    data = new char[size];
    assert(data);
}

//清空数据
void io_buf::clear() {
    length = head = 0;
}

//将已经处理过的数据，清空,将未处理的数据提前至数据首地址
void io_buf::adjust() {
    if (head != 0) {
        if (length != 0) {
            memmove(data, data+head, length);
        }
        head = 0;
    }
}

//将其他io_buf对象数据考本到自己中
void io_buf::copy(const io_buf *other) {
    memcpy(data, other->data + other->head, other->length);
    head = 0;
    length = other->length;
}

//处理长度为len的数据，移动head和修正length
void io_buf::pop(int len) {
    length -= len;
    head += len;
}

reactor_buf::reactor_buf() 
{
    _buf = NULL;
}

reactor_buf::~reactor_buf()
{
    clear();
}

const int reactor_buf::length() const 
{
    return _buf != NULL? _buf->length : 0;
}

void reactor_buf::pop(int len) 
{
    assert(_buf != NULL && len <= _buf->length);

    _buf->pop(len);

    //当此时_buf的可用长度已经为0
    if(_buf->length == 0) {
        //将_buf重新放回buf_pool中
        buf_pool::instance()->revert(_buf);
        _buf = NULL;
    }
}

void reactor_buf::clear()
{
    if (_buf != NULL)  {
        //将_buf重新放回buf_pool中
        buf_pool::instance()->revert(_buf);
        _buf = NULL;
    }
}

//================= reactor_buf ===============


//从一个fd中读取数据到reactor_buf中
int input_buf::read_data(int fd)
{
    int need_read;//硬件有多少数据可以读

    //一次性读出所有的数据
    //需要给fd设置FIONREAD,
    //得到read缓冲中有多少数据是可以读取的
    if (ioctl(fd, FIONREAD, &need_read) == -1) {
        fprintf(stderr, "ioctl FIONREAD\n");
        return -1;
    }

    
    if (_buf == NULL) {
        //如果io_buf为空,从内存池申请
        _buf = buf_pool::instance()->alloc_buf(need_read);
        if (_buf == NULL) {
            fprintf(stderr, "no idle buf for alloc\n");
            return -1;
        }
    }
    else {
        //如果io_buf可用，判断是否够存
        assert(_buf->head == 0);
        if (_buf->capacity - _buf->length < (int)need_read) {
            //不够存，冲内存池申请
            io_buf *new_buf = buf_pool::instance()->alloc_buf(need_read+_buf->length);
            if (new_buf == NULL) {
                fprintf(stderr, "no ilde buf for alloc\n");
                return -1;
            }
            //将之前的_buf的数据考到新申请的buf中
            new_buf->copy(_buf);
            //将之前的_buf放回内存池中
            buf_pool::instance()->revert(_buf);
            //新申请的buf成为当前io_buf
            _buf = new_buf;
        }
    }

    //读取数据
    int already_read = 0;
    do { 
        //读取的数据拼接到之前的数据之后
        if(need_read == 0) {
            //可能是read阻塞读数据的模式，对方未写数据
            already_read = read(fd, _buf->data + _buf->length, m4K);
        } else {
            already_read = read(fd, _buf->data + _buf->length, need_read);
        }
    } while (already_read == -1 && errno == EINTR); //systemCall引起的中断 继续读取
    if (already_read > 0)  {
        if (need_read != 0) {
            assert(already_read == need_read);
        }
        _buf->length += already_read;
    }

    return already_read;
}

//取出读到的数据
const char *input_buf::data() const 
{
    return _buf != NULL ? _buf->data + _buf->head : NULL;
}

//重置缓冲区
void input_buf::adjust()
{
    if (_buf != NULL) {
        _buf->adjust();
    }
}

//================= input_buf ===========

//将一段数据 写到一个reactor_buf中
int output_buf::send_data(const char *data, int datalen)
{
    if (_buf == NULL) {
        //如果io_buf为空,从内存池申请
        _buf = buf_pool::instance()->alloc_buf(datalen);
        if (_buf == NULL) {
            fprintf(stderr, "no idle buf for alloc\n");
            return -1;
        }
    }
    else {
        //如果io_buf可用，判断是否够存
        assert(_buf->head == 0);
        if (_buf->capacity - _buf->length < datalen) {
            //不够存，冲内存池申请
            io_buf *new_buf = buf_pool::instance()->alloc_buf(datalen+_buf->length);
            if (new_buf == NULL) {
                fprintf(stderr, "no ilde buf for alloc\n");
                return -1;
            }
            //将之前的_buf的数据考到新申请的buf中
            new_buf->copy(_buf);
            //将之前的_buf放回内存池中
            buf_pool::instance()->revert(_buf);
            //新申请的buf成为当前io_buf
            _buf = new_buf;
        }
    }

    //将data数据拷贝到io_buf中,拼接到后面
    memcpy(_buf->data + _buf->length, data, datalen);
    _buf->length += datalen;

    return 0;
}

//将reactor_buf中的数据写到一个fd中
int output_buf::write2fd(int fd)
{
    assert(_buf != NULL && _buf->head == 0);

    int already_write = 0;

    do { 
        already_write = write(fd, _buf->data, _buf->length);
    } while (already_write == -1 && errno == EINTR); //systemCall引起的中断，继续写


    if (already_write > 0) {
        //已经处理的数据清空
        _buf->pop(already_write);
        //未处理数据前置，覆盖老数据
        _buf->adjust();
    }

    //如果fd非阻塞，可能会得到EAGAIN错误
    if (already_write == -1 && errno == EAGAIN) {
        already_write = 0;//不是错误，仅仅返回0，表示目前是不可以继续写的
    }

    return already_write;
}





//回显业务
void callback_busi(const char *data, uint32_t len, int msgid, void *args, tcp_conn *conn)
{
    conn->send_message(data, len, msgid);
}


//连接的读事件回调
static void conn_rd_callback(event_loop *loop, int fd, void *args)
{
    tcp_conn *conn = (tcp_conn*)args;
    conn->do_read();
}
//连接的写事件回调
static void conn_wt_callback(event_loop *loop, int fd, void *args)
{
    tcp_conn *conn = (tcp_conn*)args;
    conn->do_write();
}

//初始化tcp_conn
tcp_conn::tcp_conn(int connfd, event_loop *loop)
{
    _connfd = connfd;
    _loop = loop;
    //1. 将connfd设置成非阻塞状态
    int flag = fcntl(_connfd, F_GETFL, 0);
    fcntl(_connfd, F_SETFL, O_NONBLOCK|flag);

    //2. 设置TCP_NODELAY禁止做读写缓存，降低小包延迟
    int op = 1;
    setsockopt(_connfd, IPPROTO_TCP, TCP_NODELAY, &op, sizeof(op));//need netinet/in.h netinet/tcp.h

    //3. 将该链接的读事件让event_loop监控 
    _loop->add_io_event(_connfd, conn_rd_callback, EPOLLIN, this);

    //4 将该链接集成到对应的tcp_server中
    //TODO
}

//处理读业务
void tcp_conn::do_read()
{
    //1. 从套接字读取数据
    int ret = ibuf.read_data(_connfd);
    if (ret == -1) {
        fprintf(stderr, "read data from socket\n");
        this->clean_conn();
        return ;
    }
    else if ( ret == 0) {
        //对端正常关闭
        printf("connection closed by peer\n");
        clean_conn();
        return ;
    }

    //2. 解析msg_head数据    
    msg_head head;    
    
    //[这里用while，可能一次性读取多个完整包过来]
    while (ibuf.length() >= MESSAGE_HEAD_LEN)  {
        //2.1 读取msg_head头部，固定长度MESSAGE_HEAD_LEN    
        memcpy(&head, ibuf.data(), MESSAGE_HEAD_LEN);
        if(head.msglen > MESSAGE_LENGTH_LIMIT || head.msglen < 0) {
            fprintf(stderr, "data format error, need close, msglen = %d\n", head.msglen);
            this->clean_conn();
            break;
        }
        if (ibuf.length() < MESSAGE_HEAD_LEN + head.msglen) {
            //缓存buf中剩余的数据，小于实际上应该接受的数据
            //说明是一个不完整的包，应该抛弃
            break;
        }

        //2.2 再根据头长度读取数据体，然后针对数据体处理 业务
        //TODO 添加包路由模式
        
        //头部处理完了，往后偏移MESSAGE_HEAD_LEN长度
        ibuf.pop(MESSAGE_HEAD_LEN);
        
        //处理ibuf.data()业务数据
        printf("read data: %s\n", ibuf.data());

        //回显业务
        callback_busi(ibuf.data(), head.msglen, head.msgid, NULL, this);

        //消息体处理完了,往后便宜msglen长度
        ibuf.pop(head.msglen);
    }

    ibuf.adjust();
    
    return ;
}

//处理写业务
void tcp_conn::do_write()
{
    //do_write是触发玩event事件要处理的事情，
    //应该是直接将out_buf力度数据io写会对方客户端 
    //而不是在这里组装一个message再发
    //组装message的过程应该是主动调用
    
    //只要obuf中有数据就写
    while (obuf.length()) {
        int ret = obuf.write2fd(_connfd);
        if (ret == -1) {
            fprintf(stderr, "write2fd error, close conn!\n");
            this->clean_conn();
            return ;
        }
        if (ret == 0) {
            //不是错误，仅返回0表示不可继续写
            break;
        }
    }

    if (obuf.length() == 0) {
        //数据已经全部写完，将_connfd的写事件取消掉
        _loop->del_io_event(_connfd, EPOLLOUT);
    }

    return ;
}

//发送消息的方法
int tcp_conn::send_message(const char *data, int msglen, int msgid)
{
    printf("server send_message: %s:%d, msgid = %d\n", data, msglen, msgid);
    bool active_epollout = false; 
    if(obuf.length() == 0) {
        //如果现在已经数据都发送完了，那么是一定要激活写事件的
        //如果有数据，说明数据还没有完全写完到对端，那么没必要再激活等写完再激活
        active_epollout = true;
    }

    //1 先封装message消息头
    msg_head head;
    head.msgid = msgid;
    head.msglen = msglen;
     
    //1.1 写消息头
    int ret = obuf.send_data((const char *)&head, MESSAGE_HEAD_LEN);
    if (ret != 0) {
        fprintf(stderr, "send head error\n");
        return -1;
    }

    //1.2 写消息体
    ret = obuf.send_data(data, msglen);
    if (ret != 0) {
        //如果写消息体失败，那就回滚将消息头的发送也取消
        obuf.pop(MESSAGE_HEAD_LEN);
        return -1;
    }

    if (active_epollout == true) {
        //激活EPOLLOUT写事件
        _loop->add_io_event(_connfd, conn_wt_callback, EPOLLOUT, this);
    }


    return 0;
}

//销毁tcp_conn
void tcp_conn::clean_conn()
{
    //链接清理工作
    //1 将该链接从tcp_server摘除掉    
    //TODO 
    //2 将该链接从event_loop中摘除
    _loop->del_io_event(_connfd);
    //3 buf清空
    ibuf.clear(); 
    obuf.clear();
    //4 关闭原始套接字
    int fd = _connfd;
    _connfd = -1;
    close(fd);
}


//回显业务
void callback_busi(const char *data, uint32_t len, int msgid, void *args, tcp_conn *conn)
{
    conn->send_message(data, len, msgid);
}


//连接的读事件回调
static void conn_rd_callback(event_loop *loop, int fd, void *args)
{
    tcp_conn *conn = (tcp_conn*)args;
    conn->do_read();
}
//连接的写事件回调
static void conn_wt_callback(event_loop *loop, int fd, void *args)
{
    tcp_conn *conn = (tcp_conn*)args;
    conn->do_write();
}

//初始化tcp_conn
tcp_conn::tcp_conn(int connfd, event_loop *loop)
{
    _connfd = connfd;
    _loop = loop;
    //1. 将connfd设置成非阻塞状态
    int flag = fcntl(_connfd, F_GETFL, 0);
    fcntl(_connfd, F_SETFL, O_NONBLOCK|flag);

    //2. 设置TCP_NODELAY禁止做读写缓存，降低小包延迟
    int op = 1;
    setsockopt(_connfd, IPPROTO_TCP, TCP_NODELAY, &op, sizeof(op));//need netinet/in.h netinet/tcp.h

    //3. 将该链接的读事件让event_loop监控 
    _loop->add_io_event(_connfd, conn_rd_callback, EPOLLIN, this);

    //4 将该链接集成到对应的tcp_server中
    //TODO
}

//处理读业务
void tcp_conn::do_read()
{
    //1. 从套接字读取数据
    int ret = ibuf.read_data(_connfd);
    if (ret == -1) {
        fprintf(stderr, "read data from socket\n");
        this->clean_conn();
        return ;
    }
    else if ( ret == 0) {
        //对端正常关闭
        printf("connection closed by peer\n");
        clean_conn();
        return ;
    }

    //2. 解析msg_head数据    
    msg_head head;    
    
    //[这里用while，可能一次性读取多个完整包过来]
    while (ibuf.length() >= MESSAGE_HEAD_LEN)  {
        //2.1 读取msg_head头部，固定长度MESSAGE_HEAD_LEN    
        memcpy(&head, ibuf.data(), MESSAGE_HEAD_LEN);
        if(head.msglen > MESSAGE_LENGTH_LIMIT || head.msglen < 0) {
            fprintf(stderr, "data format error, need close, msglen = %d\n", head.msglen);
            this->clean_conn();
            break;
        }
        if (ibuf.length() < MESSAGE_HEAD_LEN + head.msglen) {
            //缓存buf中剩余的数据，小于实际上应该接受的数据
            //说明是一个不完整的包，应该抛弃
            break;
        }

        //2.2 再根据头长度读取数据体，然后针对数据体处理 业务
        //TODO 添加包路由模式
        
        //头部处理完了，往后偏移MESSAGE_HEAD_LEN长度
        ibuf.pop(MESSAGE_HEAD_LEN);
        
        //处理ibuf.data()业务数据
        printf("read data: %s\n", ibuf.data());

        //回显业务
        callback_busi(ibuf.data(), head.msglen, head.msgid, NULL, this);

        //消息体处理完了,往后便宜msglen长度
        ibuf.pop(head.msglen);
    }

    ibuf.adjust();
    
    return ;
}

//处理写业务
void tcp_conn::do_write()
{
    //do_write是触发玩event事件要处理的事情，
    //应该是直接将out_buf力度数据io写会对方客户端 
    //而不是在这里组装一个message再发
    //组装message的过程应该是主动调用
    
    //只要obuf中有数据就写
    while (obuf.length()) {
        int ret = obuf.write2fd(_connfd);
        if (ret == -1) {
            fprintf(stderr, "write2fd error, close conn!\n");
            this->clean_conn();
            return ;
        }
        if (ret == 0) {
            //不是错误，仅返回0表示不可继续写
            break;
        }
    }

    if (obuf.length() == 0) {
        //数据已经全部写完，将_connfd的写事件取消掉
        _loop->del_io_event(_connfd, EPOLLOUT);
    }

    return ;
}

//发送消息的方法
int tcp_conn::send_message(const char *data, int msglen, int msgid)
{
    printf("server send_message: %s:%d, msgid = %d\n", data, msglen, msgid);
    bool active_epollout = false; 
    if(obuf.length() == 0) {
        //如果现在已经数据都发送完了，那么是一定要激活写事件的
        //如果有数据，说明数据还没有完全写完到对端，那么没必要再激活等写完再激活
        active_epollout = true;
    }

    //1 先封装message消息头
    msg_head head;
    head.msgid = msgid;
    head.msglen = msglen;
     
    //1.1 写消息头
    int ret = obuf.send_data((const char *)&head, MESSAGE_HEAD_LEN);
    if (ret != 0) {
        fprintf(stderr, "send head error\n");
        return -1;
    }

    //1.2 写消息体
    ret = obuf.send_data(data, msglen);
    if (ret != 0) {
        //如果写消息体失败，那就回滚将消息头的发送也取消
        obuf.pop(MESSAGE_HEAD_LEN);
        return -1;
    }

    if (active_epollout == true) {
        //激活EPOLLOUT写事件
        _loop->add_io_event(_connfd, conn_wt_callback, EPOLLOUT, this);
    }


    return 0;
}

//销毁tcp_conn
void tcp_conn::clean_conn()
{
    //链接清理工作
    //1 将该链接从tcp_server摘除掉    
    //TODO 
    //2 将该链接从event_loop中摘除
    _loop->del_io_event(_connfd);
    //3 buf清空
    ibuf.clear(); 
    obuf.clear();
    //4 关闭原始套接字
    int fd = _connfd;
    _connfd = -1;
    close(fd);
}

int main() 
{
    event_loop loop;

    tcp_server server(&loop, "127.0.0.1", 7777);

    loop.event_process();

    return 0;
}
