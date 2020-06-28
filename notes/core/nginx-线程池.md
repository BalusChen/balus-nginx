# nginx 线程池

nginx 是异步非阻塞的多进程模型，非阻塞要求操作不能占用太多时间，否则会让其他请求得不到服务；但是很难将所有的操作都非阻塞化，所以 nginx 还是提供了线程池机制。

以前没有接触过线程池，对于它的实现很是好奇；而线程池作为 nginx 官方代码，质量一定有保证，可以好好学习一下。

在阅读 nginx 线程池的源码之前，我对线程池主要有几个问题不太理解：

* 如何让没有活干的 thread 停留在 pool 中？毕竟线程是一种活动的东西
* 线程池的使用者怎么把需要在线程中完成的任务提交至线程池？
* 线程完成了任务之后怎么回到主线程？

## 线程池的组成

线程池使用`thread_pool`指令创建，其语法如下：

```nginx
thread_pool name threads=number [max_queue=number];
```

其中指定了线程池的名字以及池中线程个数，以及最大任务等待队列的长度。

线程与其相关实体的结构如下：

```c
typedef struct ngx_thread_pool_s  ngx_thread_pool_t;

struct ngx_thread_pool_s {
    ngx_thread_mutex_t        mtx;      // 线程同步用的互斥锁
    ngx_thread_pool_queue_t   queue;    // 任务队列，存放待线程中运行的任务
    ngx_int_t                 waiting;  // 等待的任务数
    ngx_thread_cond_t         cond;     // 线程同步用的条件变量

    ngx_log_t                *log;

    ngx_str_t                 name;     // 线程池的名字
    ngx_uint_t                threads;  // 池中线程的个数
    ngx_int_t                 max_queue;// 任务队列的最大长度

    u_char                   *file;     // TODO
    ngx_uint_t                line;     // TODO
};


typedef struct {
    ngx_thread_task_t        *first;
    ngx_thread_task_t       **last;
} ngx_thread_pool_queue_t;
```

每个线程池都用`ngx_thread_pool_t`来表示，其各个字段已经解释清楚了。而且线程池的实现采用了生产者-消费者模型，使用线程池的模块生产任务并投递至线程池，线程池中的线程则消费任务，任务在线程池中用`ngx_thread_task_t`表示：

```c
typedef struct ngx_thread_task_s     ngx_thread_task_t;

struct ngx_thread_task_s {
    ngx_thread_task_t   *next;
    ngx_uint_t           id;
    void                *ctx;
    void               (*handler)(void *data, ngx_log_t *log);
    ngx_event_t          event;
};
```

其中`handler`就是执行业务逻辑的函数，而`ctx`是`handler`的参数。需要注意的是`event`字段，它是 nginx 多线程机制和事件机制之间的桥梁。它是一个"虚"事件对象，并不在 nginx 事件机制的事件池里面，也不关联实际的网络连接或者定时器，而只关联线程任务，所以其中大部分的字段都是无意义的，主要用到的是`handler`和`data`字段，当线程完成任务时由事件机制回调。

线程池的实现还包含以下几个 static 变量：

```c
static ngx_str_t  ngx_thread_pool_default = ngx_string("default");

static ngx_uint_t               ngx_thread_pool_task_id;
static ngx_atomic_t             ngx_thread_pool_done_lock;
static ngx_thread_pool_queue_t  ngx_thread_pool_done;
```

task_id 目前用于日志记录，`ngx_thread_pool_done`则是本进程中所有已经完成了的任务的列表，需要注意，完成列表是本进程中所有线程池共享的。后续分配任务的时候可以直接从里面拿。

## 线程池初始化

线程池模块的配置结构体就是一个线程池的数组：

```c
typedef struct {
    ngx_array_t               pools;
} ngx_thread_pool_conf_t;
```

所以在进入 worker 循环时，就对数组中所有线程池进行初始化，这里看一下单个线程池是怎么初始化的：

```c
static ngx_int_t
ngx_thread_pool_init(ngx_thread_pool_t *tp, ngx_log_t *log, ngx_pool_t *pool)
{
    int             err;
    pthread_t       tid;
    ngx_uint_t      n;
    pthread_attr_t  attr;

    if (ngx_notify == NULL) {
        ...
        return NGX_ERROR;
    }

    ngx_thread_pool_queue_init(&tp->queue);

    if (ngx_thread_mutex_create(&tp->mtx, log) != NGX_OK) {
        return NGX_ERROR;
    }

    if (ngx_thread_cond_create(&tp->cond, log) != NGX_OK) {
        (void) ngx_thread_mutex_destroy(&tp->mtx, log);
        return NGX_ERROR;
    }

    tp->log = log;

    err = pthread_attr_init(&attr);
    if (err) {
        ...
        return NGX_ERROR;
    }

    err = pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    if (err) {
        ...
        return NGX_ERROR;
    }

    for (n = 0; n < tp->threads; n++) {
        err = pthread_create(&tid, &attr, ngx_thread_pool_cycle, tp);
        if (err) {
            ...
            return NGX_ERROR;
        }
    }

    (void) pthread_attr_destroy(&attr);

    return NGX_OK;
}
```

首先要确保事件通知机制是可用的，毕竟它是回到主线程的关键。然后初始化互斥量和条件变量，并设置线程的 detach 属性。最后依据 nginx.conf 中`thread_pool`指令的值来创建线程池中的线程。

### 线程循环

这里最重要的就是线程工作循环了，这个和 master 工作循环、worker 工作循环类似。这里主要看一下线程是如何停在线程池中，以及如何执行线程池使用者提交的代码片段。

```c
static void *
ngx_thread_pool_cycle(void *data)
{
    ngx_thread_pool_t *tp = data;

    int                 err;
    sigset_t            set;
    ngx_thread_task_t  *task;

    sigfillset(&set);

    sigdelset(&set, SIGILL);
    sigdelset(&set, SIGFPE);
    sigdelset(&set, SIGSEGV);
    sigdelset(&set, SIGBUS);

    err = pthread_sigmask(SIG_BLOCK, &set, NULL);
    if (err) {
        ...
        return NULL;
    }

    for ( ;; ) {

        if (ngx_thread_mutex_lock(&tp->mtx, tp->log) != NGX_OK) {
            return NULL;
        }

        /* the number may become negative */
        tp->waiting--;

        while (tp->queue.first == NULL) {
            if (ngx_thread_cond_wait(&tp->cond, &tp->mtx, tp->log)
                != NGX_OK)
            {
                (void) ngx_thread_mutex_unlock(&tp->mtx, tp->log);
                return NULL;
            }
        }

        task = tp->queue.first;
        tp->queue.first = task->next;

        if (tp->queue.first == NULL) {
            tp->queue.last = &tp->queue.first;
        }

        if (ngx_thread_mutex_unlock(&tp->mtx, tp->log) != NGX_OK) {
            return NULL;
        }

        task->handler(task->ctx, tp->log);

        task->next = NULL;

        ngx_spinlock(&ngx_thread_pool_done_lock, 1, 2048);

        *ngx_thread_pool_done.last = task;
        ngx_thread_pool_done.last = &task->next;

        ngx_memory_barrier();

        ngx_unlock(&ngx_thread_pool_done_lock);

        (void) ngx_notify(ngx_thread_pool_handler);
    }
}
```

TODO: 首先是阻塞了除`SIGILL`、`SIGFPE`、`SIGSEGV`和`SIGBUS`之外的所有信号，

然后在一个无线循环中检查任务队列是否为空，如果为队列空的话，就阻塞在该条件变量上，这样就让线程停下来了；否则的话，取出队列头部的任务并执行。执行完毕后将已经完成的任务放到完成队列的尾部，并通知事件模块，事件模块得到通知后便在主线程中执行`ngx_thread_pool_handler`：

```c
static void
ngx_thread_pool_handler(ngx_event_t *ev)
{
    ngx_event_t        *event;
    ngx_thread_task_t  *task;

    ngx_spinlock(&ngx_thread_pool_done_lock, 1, 2048);

    task = ngx_thread_pool_done.first;
    ngx_thread_pool_done.first = NULL;
    ngx_thread_pool_done.last = &ngx_thread_pool_done.first;

    ngx_memory_barrier();

    ngx_unlock(&ngx_thread_pool_done_lock);

    while (task) {
        event = &task->event;
        task = task->next;

        event->complete = 1;
        event->active = 0;

        event->handler(event);
    }
}
```

首先记录下完成任务队列的头部，然后将完成任务队列清空；而后通过这个头部对完成队列中的每个任务都执行其`event->handler`方法。

## 任务投递

## 线程池销毁

## 总结

## 参考
